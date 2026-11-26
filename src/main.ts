import './styles.css';
import { compileSketch, type BuildOutcome } from './compiler/index.ts';
import { createEditor, type SketchEditor } from './editor.ts';
import { parseIntelHex, toIntelHex, type HexImage } from './hex.ts';
import { Uploader, describePort, type BoardFamily } from './serial/uploader.ts';
import { ATMEGA328P } from './serial/stk500.ts';
import type { EspImage } from './serial/esp.ts';
import {
  BLINK,
  EXAMPLES,
  loadActiveId,
  loadSketches,
  newSketch,
  saveActiveId,
  saveSketches,
  uid,
  type Sketch,
  type SketchFile,
} from './storage.ts';

const RAM_BYTES = 2048;
const FLASH_BYTES = 32256;

type OutputTab = 'log' | 'problems' | 'monitor';

const $ = <T extends HTMLElement>(id: string): T => {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
};

interface EspUpload {
  id: string;
  name: string;
  address: number;
  data: Uint8Array;
}

class App {
  private sketches: Sketch[] = [];
  private active: Sketch;
  private activeFileId: string;
  private editor: SketchEditor;
  private uploader = new Uploader();
  private outcome: BuildOutcome | null = null;
  private binaryOutcome: { name: string; image: HexImage } | null = null;
  private family: BoardFamily = 'avr';
  private espUploads: EspUpload[] = [];
  private busy: 'compile' | 'flash' | null = null;
  private toolchainWarmed = false;
  private abort: AbortController | null = null;
  private logEntries: { text: string; kind: string }[] = [];
  private saveTimer: number | null = null;

  constructor() {
    this.sketches = loadSketches();
    if (!this.sketches.length) {
      this.sketches = [newSketch('Blink', BLINK)];
    }
    const storedActive = loadActiveId();
    this.active = this.sketches.find((s) => s.id === storedActive) ?? this.sketches[0];
    this.activeFileId = this.mainFileOf(this.active).id;
    const restored = this.currentFile;

    this.editor = createEditor($('editor'), restored.content, () => this.scheduleSave());
    this.renderAll();
    saveSketches(this.sketches);
    saveActiveId(this.active.id);
    this.bindEvents();
    this.bindSerialEvents();
    this.applyPlatformShortcuts();
    this.log('Готово. Напишите скетч, нажмите «Собрать» — и «Прошить».', 'info');
    this.log('Компиляция идёт в браузере: avr-gcc 5.4 + binutils в WebAssembly.', 'muted');
  }

  private applyPlatformShortcuts(): void {
    const isMac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
    if (isMac) return;
    for (const kbd of document.querySelectorAll<HTMLElement>('.topbar-actions kbd')) {
      kbd.textContent = kbd.textContent?.replace('⌘', 'Ctrl+') ?? '';
    }
  }

  /* ── sketch bookkeeping ─────────────────────────────────────────── */

  private mainFileOf(sketch: Sketch): SketchFile {
    return sketch.files.find((f) => f.name === sketch.mainFile) ?? sketch.files[0];
  }

  private get currentFile(): SketchFile {
    return this.active.files.find((f) => f.id === this.activeFileId) ?? this.mainFileOf(this.active);
  }

  private touch(): void {
    this.active.updatedAt = Date.now();
    const idx = this.sketches.findIndex((s) => s.id === this.active.id);
    if (idx >= 0) this.sketches[idx] = this.active;
  }

  private scheduleSave(): void {
    this.touch();
    $('save-state').textContent = 'изменено';
    $('save-state').classList.add('dirty');
    this.updateCursor();
    if (this.saveTimer) window.clearTimeout(this.saveTimer);
    this.saveTimer = window.setTimeout(() => this.persist(), 400);
  }

  private persist(): void {
    this.touch();
    saveSketches(this.sketches);
    saveActiveId(this.active.id);
    $('save-state').textContent = 'сохранено';
    $('save-state').classList.remove('dirty');
    this.renderSketchList();
  }

  private switchSketch(id: string): void {
    const next = this.sketches.find((s) => s.id === id);
    if (!next) return;
    this.persist();
    this.active = next;
    this.activeFileId = this.mainFileOf(next).id;
    this.outcome = null;
    this.binaryOutcome = null;
    this.editor.setValue(this.currentFile.content);
    this.editor.setDiagnostics([]);
    this.clearBuildInfo();
    this.renderAll();
    this.persist();
  }

  private switchFile(id: string): void {
    const file = this.active.files.find((f) => f.id === id);
    if (!file) return;
    this.persist();
    this.activeFileId = id;
    this.editor.setValue(file.content);
    this.editor.setDiagnostics(this.outcome?.diagnostics ?? []);
    this.renderTabs();
    this.renderFileList();
  }

  private addFile(): void {
    const input = $<HTMLInputElement>('file-input');
    input.value = '';
    input.click();
  }

  private addFileContent(name: string, content: string): void {
    const base = name.replace(/\.(ino|cpp|c|h|txt)$/i, '');
    let finalName = `${base}.ino`;
    let counter = 2;
    while (this.active.files.some((f) => f.name === finalName)) finalName = `${base}${counter++}.ino`;
    const file: SketchFile = { id: uid(), name: finalName, content };
    this.active.files.push(file);
    this.activeFileId = file.id;
    this.editor.setValue(content);
    this.renderAll();
    this.persist();
    this.log(`Добавлен файл ${finalName}`, 'muted');
  }

  private removeFile(id: string): void {
    if (this.active.files.length === 1) {
      this.log('В скетче должен остаться хотя бы один файл', 'warn');
      return;
    }
    const index = this.active.files.findIndex((f) => f.id === id);
    if (index < 0) return;
    const [removed] = this.active.files.splice(index, 1);
    if (this.active.mainFile === removed.name) this.active.mainFile = this.active.files[0].name;
    if (this.activeFileId === id) {
      this.activeFileId = this.mainFileOf(this.active).id;
      this.editor.setValue(this.currentFile.content);
    }
    this.renderAll();
    this.persist();
  }

  /* ── rendering ──────────────────────────────────────────────────── */

  private renderAll(): void {
    this.syncActiveFile();
    $<HTMLInputElement>('sketch-name').value = this.active.name;
    this.renderSketchList();
    this.renderExamples();
    this.renderTabs();
    this.renderFileList();
    this.renderPortBadge();
    this.renderEspFiles();
  }

  /**
   * Fall back to the main file when the stored tab id no longer exists (e.g.
   * after a file was removed by another tab of the app).
   */
  private syncActiveFile(): void {
    if (!this.active.files.some((f) => f.id === this.activeFileId)) {
      this.activeFileId = this.mainFileOf(this.active).id;
      this.editor.setValue(this.currentFile.content);
    }
  }

  private renderSketchList(): void {
    const host = $('sketch-list');
    host.replaceChildren();
    for (const sketch of this.sketches) {
      const item = document.createElement('div');
      item.className = 'sketch-item' + (sketch.id === this.active.id ? ' active' : '');
      item.innerHTML = `<span class="item-name"></span>`;
      item.querySelector('.item-name')!.textContent = sketch.name;
      item.addEventListener('click', () => this.switchSketch(sketch.id));
      const del = document.createElement('button');
      del.className = 'delete-btn';
      del.textContent = '×';
      del.title = 'Удалить скетч';
      del.addEventListener('click', (event) => {
        event.stopPropagation();
        this.deleteSketch(sketch.id);
      });
      item.append(del);
      host.append(item);
    }
  }

  private deleteSketch(id: string): void {
    if (this.sketches.length === 1) {
      this.log('Нельзя удалить последний скетч', 'warn');
      return;
    }
    const index = this.sketches.findIndex((s) => s.id === id);
    if (index < 0) return;
    this.sketches.splice(index, 1);
    if (this.active.id === id) {
      this.active = this.sketches[0];
      this.activeFileId = this.mainFileOf(this.active).id;
      this.editor.setValue(this.currentFile.content);
    }
    this.renderAll();
    this.persist();
  }

  private renderExamples(): void {
    const host = $('example-list');
    host.replaceChildren();
    for (const example of EXAMPLES) {
      const item = document.createElement('button');
      item.className = 'example-item';
      item.innerHTML = '<span class="item-name"></span><span class="item-sub"></span>';
      item.querySelector('.item-name')!.textContent = example.name;
      item.querySelector('.item-sub')!.textContent = example.description;
      item.addEventListener('click', () => {
        const sketch = newSketch(example.name, example.content);
        this.sketches.unshift(sketch);
        this.active = sketch;
        this.activeFileId = this.mainFileOf(sketch).id;
        this.outcome = null;
        this.editor.setValue(example.content);
        this.editor.setDiagnostics([]);
        this.clearBuildInfo();
        this.renderAll();
        this.persist();
        this.log(`Открыт пример «${example.name}»`, 'info');
      });
      host.append(item);
    }
  }

  private renderTabs(): void {
    const host = $('file-tabs');
    host.replaceChildren();
    const main = this.mainFileOf(this.active);
    for (const file of this.active.files) {
      const tab = document.createElement('button');
      tab.className = 'file-tab' + (file.id === this.activeFileId ? ' active' : '');
      if (file.name === main.name) {
        const dot = document.createElement('span');
        dot.className = 'dot';
        dot.title = 'Главный файл скетча';
        tab.append(dot);
      }
      const name = document.createElement('span');
      name.textContent = file.name;
      tab.append(name);
      tab.addEventListener('click', () => this.switchFile(file.id));
      tab.addEventListener('dblclick', () => this.renameFile(file.id));
      host.append(tab);
    }
  }

  private renderFileList(): void {
    const host = $('file-list');
    host.replaceChildren();
    const main = this.mainFileOf(this.active);
    for (const file of this.active.files) {
      const item = document.createElement('div');
      item.className = 'file-item' + (file.id === this.activeFileId ? ' active' : '');
      const glyph = document.createElement('span');
      glyph.className = 'file-glyph';
      glyph.textContent = file.name === main.name ? 'ino' : 'src';
      const name = document.createElement('span');
      name.className = 'item-name';
      name.textContent = file.name;
      name.title = file.name === main.name ? 'Главный файл (по нему назван скетч)' : 'Дополнительный файл';
      const del = document.createElement('button');
      del.className = 'delete-btn';
      del.textContent = '×';
      del.title = 'Удалить файл';
      del.addEventListener('click', (event) => {
        event.stopPropagation();
        this.removeFile(file.id);
      });
      item.append(glyph, name, del);
      item.addEventListener('click', () => this.switchFile(file.id));
      host.append(item);
    }
  }

  private renameFile(id: string): void {
    const file = this.active.files.find((f) => f.id === id);
    if (!file) return;
    const next = window.prompt('Новое имя файла', file.name);
    if (!next) return;
    const name = next.trim().endsWith('.ino') ? next.trim() : `${next.trim()}.ino`;
    if (this.active.files.some((f) => f.id !== id && f.name === name)) {
      this.log(`Файл ${name} уже есть в скетче`, 'warn');
      return;
    }
    const wasMain = this.active.mainFile === file.name;
    file.name = name;
    if (wasMain) this.active.mainFile = name;
    this.renderTabs();
    this.renderFileList();
    this.persist();
  }

  private renderPortBadge(): void {
    const badge = $('port-badge');
    if (this.uploader.connected) {
      badge.textContent = this.uploader.portLabel || 'порт выбран';
      badge.className = 'badge ok';
      $('status-device').textContent = `плата подключена · ${this.uploader.portLabel}`;
      $('status-device').className = 'status-item live';
    } else {
      badge.textContent = 'не подключена';
      badge.className = 'badge muted';
      $('status-device').textContent = 'плата не подключена';
      $('status-device').className = 'status-item';
    }
    $<HTMLButtonElement>('btn-disconnect').disabled = !this.uploader.connected;
  }

  private renderEspFiles(): void {
    const host = $('esp-file-list');
    host.replaceChildren();
    if (!this.espUploads.length) {
      const empty = document.createElement('p');
      empty.className = 'hint';
      empty.textContent = 'Файлы не выбраны — добавьте .bin из Arduino IDE («Экспорт бинарных файлов»).';
      host.append(empty);
      return;
    }
    for (const upload of this.espUploads) {
      const row = document.createElement('div');
      row.className = 'esp-file';
      const name = document.createElement('span');
      name.className = 'item-name';
      name.textContent = upload.name;
      name.title = upload.name;
      const addr = document.createElement('input');
      addr.value = '0x' + upload.address.toString(16).toUpperCase();
      addr.title = 'Адрес во флеш-памяти';
      addr.addEventListener('change', () => {
        const parsed = Number.parseInt(addr.value.replace(/^0x/i, ''), 16);
        if (Number.isNaN(parsed)) {
          addr.value = '0x' + upload.address.toString(16).toUpperCase();
          return;
        }
        upload.address = parsed;
        addr.value = '0x' + parsed.toString(16).toUpperCase();
      });
      const size = document.createElement('span');
      size.className = 'size';
      size.textContent = `${(upload.data.length / 1024).toFixed(1)} КБ`;
      const del = document.createElement('button');
      del.className = 'delete-btn';
      del.textContent = '×';
      del.addEventListener('click', () => {
        this.espUploads = this.espUploads.filter((u) => u.id !== upload.id);
        this.renderEspFiles();
      });
      row.append(name, addr, size, del);
      host.append(row);
    }
  }

  /* ── logging & build info ───────────────────────────────────────── */

  private log(text: string, kind: 'info' | 'ok' | 'err' | 'warn' | 'muted' | 'step' = 'muted'): void {
    const host = $('output-log');
    const atBottom = host.parentElement!.scrollTop + host.parentElement!.clientHeight >= host.parentElement!.scrollHeight - 30;
    this.logEntries.push({ text, kind });
    const line = document.createElement('div');
    line.className = `log-line ${kind}`;
    const time = document.createElement('span');
    time.className = 'log-time';
    time.textContent = new Date().toLocaleTimeString('ru-RU', { hour12: false });
    const body = document.createElement('span');
    body.textContent = text;
    line.append(time, body);
    host.append(line);
    if (atBottom) host.parentElement!.scrollTop = host.parentElement!.scrollHeight;
  }

  private clearLog(): void {
    this.logEntries = [];
    $('output-log').replaceChildren();
  }

  private setStatus(text: string, kind: 'idle' | 'ok' | 'error' = 'idle'): void {
    const node = $('output-status');
    node.textContent = text;
    node.style.color = kind === 'ok' ? 'var(--green)' : kind === 'error' ? 'var(--red)' : '';
  }

  private clearBuildInfo(): void {
    $('flash-numbers').textContent = `— / ${FLASH_BYTES.toLocaleString('ru-RU')} Б`;
    $('ram-numbers').textContent = `— / ${RAM_BYTES.toLocaleString('ru-RU')} Б`;
    $('flash-fill').style.width = '0%';
    $('ram-fill').style.width = '0%';
    $('flash-fill').classList.remove('over');
    $('ram-fill').classList.remove('over');
    $('build-hint').textContent = 'Соберите скетч — размеры появятся здесь.';
    $('build-meta').textContent = 'не собрано';
    $('build-meta').className = 'meta-chip';
    $('status-build').textContent = 'сборка не запускалась';
    this.setProblems([]);
  }

  private setProblems(diagnostics: BuildOutcome['diagnostics']): void {
    const host = $('output-problems');
    host.replaceChildren();
    const pill = $('problem-count');
    const errors = diagnostics.filter((d) => d.severity === 'error').length;
    const warnings = diagnostics.filter((d) => d.severity === 'warning').length;
    pill.textContent = String(errors + warnings);
    pill.className = 'pill' + (errors ? ' has-errors' : warnings ? ' has-warnings' : '');

    if (!diagnostics.length) {
      const empty = document.createElement('p');
      empty.className = 'empty-note';
      empty.textContent = 'Ошибок нет.';
      host.append(empty);
      return;
    }

    for (const diagnostic of diagnostics) {
      const row = document.createElement('div');
      row.className = `problem ${diagnostic.severity}`;
      const loc = document.createElement('span');
      loc.className = 'problem-loc';
      loc.textContent = diagnostic.line ? `${diagnostic.line}:${diagnostic.column || 1}` : '—';
      const msg = document.createElement('span');
      msg.className = 'problem-msg';
      msg.textContent = diagnostic.message;
      const severity = document.createElement('span');
      severity.className = 'problem-severity';
      severity.textContent =
        diagnostic.severity === 'error' ? 'ошибка' : diagnostic.severity === 'warning' ? 'предупр.' : 'заметка';
      row.append(loc, msg, severity);
      if (diagnostic.line) {
        row.addEventListener('click', () => {
          this.switchToTab('problems');
          this.editor.focusLine(diagnostic.line);
        });
      }
      host.append(row);
    }
  }

  private switchToTab(name: OutputTab): void {
    for (const tab of document.querySelectorAll<HTMLButtonElement>('.output-tab')) {
      tab.classList.toggle('active', tab.dataset.tab === name);
    }
    $('output-log').classList.toggle('hidden', name !== 'log');
    $('output-problems').classList.toggle('hidden', name !== 'problems');
    $('output-monitor').classList.toggle('hidden', name !== 'monitor');
    $('output-pane').classList.remove('collapsed');
  }

  private updateCursor(): void {
    const pos = this.editor.view.state.selection.main.head;
    const line = this.editor.view.state.doc.lineAt(pos);
    $('status-cursor').textContent = `стр ${line.number}, стлб ${pos - line.from + 1}`;
  }

  /* ── build ──────────────────────────────────────────────────────── */

  private async build(): Promise<BuildOutcome | null> {
    if (this.busy) return this.outcome;
    if (this.family !== 'avr') {
      this.log('Сборка скетчей в браузере доступна для AVR (Uno/Nano). Для ESP загрузите готовый .bin.', 'warn');
      return null;
    }
    this.setBusy('compile');
    this.setStatus('сборка…');
    this.log('Сборка скетча…', 'step');
    if (!this.toolchainWarmed) {
      this.toolchainWarmed = true;
      this.log('Первая сборка скачивает в браузер компилятор avr-gcc (~17 МБ) — дальше он берётся из кеша.', 'info');
    }
    const started = performance.now();
    try {
      const files = this.active.files.map((file) => ({
        name: file.name,
        content: file.id === this.activeFileId ? this.editor.getValue() : file.content,
      }));
      const outcome = await compileSketch({ files, mainFile: this.active.mainFile });
      this.outcome = outcome;
      const seconds = ((performance.now() - started) / 1000).toFixed(1);
      this.editor.setDiagnostics(outcome.diagnostics);

      if (outcome.ok) {
        const pct = Math.round((outcome.flashBytes / outcome.appFlashBytes) * 100);
        $('flash-numbers').textContent = `${outcome.flashBytes.toLocaleString('ru-RU')} / ${outcome.appFlashBytes.toLocaleString('ru-RU')} Б`;
        $('flash-fill').style.width = `${Math.min(100, pct)}%`;
        $('flash-fill').classList.toggle('over', pct > 100);
        $('build-hint').textContent = `Скетч занимает ${pct}% флеша. Сборка заняла ${seconds} с.`;
        const protoCount = outcome.prepared.protoCount;
        $('build-meta').textContent = `${outcome.flashBytes} Б · ${seconds} с${protoCount ? ` · ${protoCount} прототип(ов)` : ''}`;
        $('build-meta').className = 'meta-chip ok';
        $('status-build').textContent = `собрано: ${outcome.flashBytes} Б за ${seconds} с`;
        $('status-build').className = 'status-item live';
        $('btn-export').removeAttribute('disabled');
        this.setStatus('сборка ок', 'ok');
        this.log(`Собрано: ${outcome.flashBytes} Б (${pct}% флеша) за ${seconds} с`, 'ok');
        if (outcome.prepared.protoCount) {
          this.log(`Arduino-прототипы: добавлено ${outcome.prepared.protoCount} объявлений функций`, 'muted');
        }
        this.binaryOutcome = null;
        this.log('Flash: ' + outcome.flashBytes + ' байт · RAM-оценка в панели справа', 'muted');
      } else {
        const errors = outcome.diagnostics.filter((d) => d.severity === 'error');
        $('build-meta').textContent = `${errors.length} ошиб(ка/и)`;
        $('build-meta').className = 'meta-chip bad';
        $('status-build').textContent = `ошибка сборки · ${errors.length}`;
        $('status-build').className = 'status-item error';
        this.setStatus('ошибка сборки', 'error');
        for (const error of errors.slice(0, 12)) {
          this.log(`${error.line ? `${error.line}: ` : ''}${error.message}`, 'err');
        }
        this.switchToTab('problems');
      }

      for (const line of outcome.log.slice(0, 40)) this.log(line, 'muted');
      this.setProblems(outcome.diagnostics);
      this.updateRamEstimate(outcome);
      return outcome;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.log(`Сборка упала: ${message}`, 'err');
      this.setStatus('сборка упала', 'error');
      $('status-build').textContent = 'сборка упала';
      $('status-build').className = 'status-item error';
      return null;
    } finally {
      this.setBusy(null);
    }
  }

  private updateRamEstimate(outcome: BuildOutcome): void {
    if (!outcome.ok) return;
    const estimate = outcome.ramBytes;
    const pct = Math.round((estimate / RAM_BYTES) * 100);
    $('ram-numbers').textContent = `≈ ${estimate} / ${RAM_BYTES.toLocaleString('ru-RU')} Б`;
    $('ram-fill').style.width = `${Math.min(100, pct)}%`;
    $('ram-fill').classList.toggle('over', pct > 90);
  }

  private setBusy(mode: 'compile' | 'flash' | null): void {
    this.busy = mode;
    const compileBtn = $<HTMLButtonElement>('btn-compile');
    const flashBtn = $<HTMLButtonElement>('btn-flash');
    compileBtn.classList.toggle('busy', mode === 'compile');
    flashBtn.classList.toggle('busy', mode === 'flash');
    compileBtn.disabled = mode !== null;
    flashBtn.disabled = mode !== null;
    $('btn-stop').hidden = mode !== 'flash';
  }

  /* ── flashing ───────────────────────────────────────────────────── */

  private async ensureConnected(): Promise<boolean> {
    if (this.uploader.connected) return true;
    this.log('Выберите последовательный порт в появившемся окне браузера…', 'info');
    try {
      await this.uploader.requestPort();
    } catch (error) {
      if (error instanceof DOMException && error.name === 'NotFoundError') {
        this.log('Порт не выбран. Подключите плату и нажмите «Подключить» ещё раз.', 'warn');
      } else {
        this.log(`Не удалось открыть порт: ${error instanceof Error ? error.message : String(error)}`, 'err');
      }
      return false;
    }
    this.renderPortBadge();
    this.log(`Порт подключён: ${this.uploader.portLabel}`, 'ok');
    return true;
  }

  private async connect(): Promise<void> {
    if (!('serial' in navigator)) {
      this.log('Web Serial недоступен. Нужен Chrome или Edge на компьютере (не Safari, не Firefox, не телефон).', 'err');
      return;
    }
    if (this.uploader.connected) {
      await this.uploader.disconnect();
      this.renderPortBadge();
      this.log('Порт освобождён', 'muted');
      return;
    }
    await this.ensureConnected();
  }

  private async flash(): Promise<void> {
    if (this.busy) return;
    if (this.family === 'esp') {
      await this.flashEsp();
      return;
    }
    await this.flashAvr();
  }

  private async flashAvr(): Promise<void> {
    const outcome = this.outcome?.ok ? this.outcome : await this.build();
    if (!outcome?.ok || !outcome.image) {
      this.log('Сначала нужен успешно собранный скетч', 'warn');
      return;
    }
    if (!(await this.ensureConnected())) return;
    if (!('serial' in navigator)) return;

    if (this.monitorOpen) {
      await this.uploader.stopMonitor();
      this.markMonitorLive(false);
    }
    this.setBusy('flash');
    this.abort = new AbortController();
    this.setStatus('прошивка…');
    const track = $('progress-track');
    const fill = $('progress-fill');
    track.hidden = false;
    fill.style.width = '0%';
    this.switchToTab('log');
    this.log('Прошивка ATmega328P через STK500…', 'step');

    try {
      const result = await this.uploader.upload({
        family: 'avr',
        avrBoard: ATMEGA328P,
        avrImage: outcome.image.data,
        signal: this.abort.signal,
        onLog: (message) => this.log(message, 'muted'),
        onProgress: (done, total) => {
          fill.style.width = `${Math.round((done / Math.max(1, total)) * 100)}%`;
        },
      });
      fill.style.width = '100%';
      this.setStatus('прошито', 'ok');
      this.log(`Готово: ${result.summary}`, 'ok');
      $('status-build').textContent = 'прошивка записана';
      $('status-build').className = 'status-item live';
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.setStatus('ошибка прошивки', 'error');
      this.log(`Прошивка не удалась: ${message}`, 'err');
      $('status-build').textContent = 'ошибка прошивки';
      $('status-build').className = 'status-item error';
      if (/bootloader|sync/i.test(message)) {
        this.log('Подсказка: закройте монитор порта Arduino IDE, проверьте кабель (нужен data-кабель) и нажмите «Прошить» ещё раз.', 'warn');
      }
    } finally {
      this.setBusy(null);
      this.abort = null;
      window.setTimeout(() => {
        track.hidden = true;
      }, 900);
    }
  }

  private async flashEsp(): Promise<void> {
    if (!this.espUploads.length) {
      this.log('Добавьте хотя бы один .bin во вкладке «ESP32 / ESP8266»', 'warn');
      return;
    }
    if (!(await this.ensureConnected())) return;

    if (this.monitorOpen) {
      await this.uploader.stopMonitor();
      this.markMonitorLive(false);
    }
    this.setBusy('flash');
    this.abort = new AbortController();
    this.setStatus('прошивка…');
    const track = $('progress-track');
    const fill = $('progress-fill');
    track.hidden = false;
    fill.style.width = '0%';
    this.switchToTab('log');
    const images: EspImage[] = this.espUploads.map((upload) => ({
      name: upload.name,
      address: upload.address,
      data: upload.data,
    }));
    this.log(`Прошивка ESP: ${images.length} образ(ов), ${images.reduce((s, i) => s + i.data.length, 0)} Б`, 'step');

    try {
      const result = await this.uploader.upload({
        family: 'esp',
        espImages: images,
        eraseAll: $<HTMLInputElement>('esp-erase-all').checked,
        signal: this.abort.signal,
        onLog: (message) => this.log(message, 'muted'),
        onProgress: (done, total) => {
          fill.style.width = `${Math.round((done / Math.max(1, total)) * 100)}%`;
        },
      });
      fill.style.width = '100%';
      this.setStatus('прошито', 'ok');
      this.log(`Готово: ${result.summary}`, 'ok');
      $('status-build').textContent = 'прошивка записана';
      $('status-build').className = 'status-item live';
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.setStatus('ошибка прошивки', 'error');
      this.log(`Прошивка не удалась: ${message}`, 'err');
    } finally {
      this.setBusy(null);
      this.abort = null;
      window.setTimeout(() => {
        track.hidden = true;
      }, 900);
    }
  }

  /* ── serial monitor ─────────────────────────────────────────────── */

  private monitorBuffer = '';
  private monitorOpen = false;
  private monitorPending = '';
  private monitorFrame = 0;

  private monitorAppend(text: string): void {
    this.monitorPending += text;
    // Redraw at most once per frame: serial data arrives in small chunks.
    if (this.monitorFrame) return;
    this.monitorFrame = requestAnimationFrame(() => {
      this.monitorFrame = 0;
      if (!this.monitorPending) return;
      this.monitorBuffer += this.monitorPending;
      this.monitorPending = '';
      if (this.monitorBuffer.length > 200_000) this.monitorBuffer = this.monitorBuffer.slice(-120_000);
      this.renderMonitor();
    });
  }

  private renderMonitor(): void {
    const host = $('monitor-log');
    const stamp = $<HTMLInputElement>('monitor-timestamp').checked;
    const atBottom = host.scrollTop + host.clientHeight >= host.scrollHeight - 40;
    const fragment = document.createDocumentFragment();
    const lines = this.monitorBuffer.split('\n');
    const now = stamp ? new Date().toLocaleTimeString('ru-RU', { hour12: false }) : '';
    lines.forEach((line, index) => {
      if (index === lines.length - 1 && line === '') return;
      const row = document.createElement('div');
      if (stamp) {
        const time = document.createElement('span');
        time.className = 'stamp';
        time.textContent = now;
        row.append(time);
      }
      row.append(document.createTextNode(line));
      fragment.append(row);
    });
    host.replaceChildren(fragment);
    if ($<HTMLInputElement>('monitor-autoscroll').checked && atBottom) host.scrollTop = host.scrollHeight;
  }

  private markMonitorLive(live: boolean): void {
    this.monitorOpen = live;
    $('monitor-dot').classList.toggle('live', live);
    $('monitor-toggle').textContent = live ? 'Закрыть порт' : 'Открыть порт';
    $('monitor-toggle').classList.toggle('primary', live);
    $('monitor-send').toggleAttribute('disabled', !live);
    $<HTMLInputElement>('monitor-baud').disabled = live;
    $('status-device').textContent = live
      ? `монитор порта открыт · ${$<HTMLSelectElement>('monitor-baud').value} бод`
      : this.uploader.connected
        ? `плата подключена · ${this.uploader.portLabel}`
        : 'плата не подключена';
  }

  private async toggleMonitor(): Promise<void> {
    if (this.monitorOpen) {
      await this.uploader.stopMonitor();
      this.markMonitorLive(false);
      this.log('Монитор порта закрыт', 'muted');
      return;
    }
    if (!(await this.ensureConnected())) return;
    const baud = Number($<HTMLSelectElement>('monitor-baud').value);
    try {
      await this.uploader.startMonitor(baud, (text) => this.monitorAppend(text));
      this.markMonitorLive(true);
      this.switchToTab('monitor');
      this.log(`Монитор порта открыт на ${baud} бод`, 'ok');
    } catch (error) {
      this.log(`Не удалось открыть монитор: ${error instanceof Error ? error.message : String(error)}`, 'err');
    }
  }

  private async sendToMonitor(): Promise<void> {
    const input = $<HTMLInputElement>('monitor-send-text');
    const text = input.value;
    if (!this.monitorOpen || !text) return;
    try {
      await this.uploader.send(new TextEncoder().encode(text + '\n'));
      input.value = '';
    } catch (error) {
      this.log(`Не удалось отправить: ${error instanceof Error ? error.message : String(error)}`, 'err');
    }
  }

  /* ── export / import ────────────────────────────────────────────── */

  private exportHex(): void {
    const built = this.outcome?.ok ? this.outcome : this.binaryOutcome;
    if (!built?.image) {
      this.log('Экспортировать нечего: сначала соберите скетч или загрузите .hex/.bin', 'warn');
      return;
    }
    const hex = this.outcome?.hex ?? toIntelHex(built.image.data);
    const name = `${this.active.name.replace(/[^\p{L}\p{N}_-]+/gu, '_')}.ino.hex`;
    download(new Blob([hex], { type: 'text/plain' }), name);
    this.log(`Сохранено: ${name}`, 'ok');
  }

  private async loadFilesFromDisk(files: FileList | File[]): Promise<void> {
    for (const file of Array.from(files)) {
      const lower = file.name.toLowerCase();
      if (lower.endsWith('.bin')) {
        const data = new Uint8Array(await file.arrayBuffer());
        this.family = 'esp';
        this.syncFamilySwitch();
        this.espUploads.push({
          id: uid(),
          name: file.name,
          address: this.espUploads.length === 0 && data.length > 0 ? 0x0 : 0x10000,
          data,
        });
        this.renderEspFiles();
        this.log(`Образ ${file.name}: ${data.length} Б`, 'info');
        continue;
      }
      if (lower.endsWith('.hex')) {
        const text = await file.text();
        try {
          const image = parseHexOrThrow(text);
          this.binaryOutcome = { name: file.name, image };
          this.family = 'avr';
          this.syncFamilySwitch();
          const pct = Math.round((image.dataBytes / FLASH_BYTES) * 100);
          $('flash-numbers').textContent = `${image.dataBytes.toLocaleString('ru-RU')} / ${FLASH_BYTES.toLocaleString('ru-RU')} Б`;
          $('flash-fill').style.width = `${Math.min(100, pct)}%`;
          $('build-meta').textContent = `${image.dataBytes} Б · из файла`;
          $('build-meta').className = 'meta-chip ok';
          $('build-hint').textContent = `Загружен готовый ${file.name}. Можно прошивать без сборки.`;
          $('btn-export').removeAttribute('disabled');
          this.log(`Загружен HEX ${file.name}: ${image.dataBytes} Б`, 'ok');
        } catch (error) {
          this.log(`Не удалось разобрать ${file.name}: ${error instanceof Error ? error.message : String(error)}`, 'err');
        }
        continue;
      }
      if (lower.endsWith('.ino') || lower.endsWith('.cpp') || lower.endsWith('.c') || lower.endsWith('.h') || lower.endsWith('.txt')) {
        const content = await file.text();
        this.addFileContent(file.name, content);
      }
    }
  }

  /* ── events ─────────────────────────────────────────────────────── */

  private bindEvents(): void {
    $('btn-compile').addEventListener('click', () => void this.build());
    $('btn-flash').addEventListener('click', () => void this.flash());
    $('btn-export').addEventListener('click', () => this.exportHex());
    $('btn-connect').addEventListener('click', () => void this.connect());
    $('btn-disconnect').addEventListener('click', () => {
      void this.uploader.disconnect().then(() => {
        this.renderPortBadge();
        this.log('Порт освобождён', 'muted');
      });
    });
    $('btn-new-sketch').addEventListener('click', () => {
      const sketch = newSketch('Новый скетч', BLINK);
      this.sketches.unshift(sketch);
      this.active = sketch;
      this.activeFileId = this.mainFileOf(sketch).id;
      this.editor.setValue(BLINK);
      this.outcome = null;
      this.clearBuildInfo();
      this.renderAll();
      this.persist();
    });
    $('btn-add-file').addEventListener('click', () => this.addFile());
    $('btn-add-image').addEventListener('click', () => {
      const input = $<HTMLInputElement>('bin-input');
      input.value = '';
      input.click();
    });
    $<HTMLInputElement>('file-input').addEventListener('change', (event) => {
      const files = (event.target as HTMLInputElement).files;
      if (files) void this.loadFilesFromDisk(files);
    });
    $<HTMLInputElement>('bin-input').addEventListener('change', (event) => {
      const files = (event.target as HTMLInputElement).files;
      if (files) void this.loadFilesFromDisk(files);
    });
    $<HTMLInputElement>('sketch-name').addEventListener('input', (event) => {
      this.active.name = (event.target as HTMLInputElement).value || 'Без имени';
      this.scheduleSave();
    });
    $('btn-clear-output').addEventListener('click', () => this.clearLog());
    $('btn-toggle-output').addEventListener('click', () => {
      $('output-pane').classList.toggle('collapsed');
    });
    for (const tab of document.querySelectorAll<HTMLButtonElement>('.output-tab')) {
      tab.addEventListener('click', () => this.switchToTab(tab.dataset.tab as OutputTab));
    }
    $('monitor-toggle').addEventListener('click', () => void this.toggleMonitor());
    $('monitor-clear').addEventListener('click', () => {
      this.monitorBuffer = '';
      this.renderMonitor();
    });
    $('monitor-send').addEventListener('click', () => void this.sendToMonitor());
    $<HTMLInputElement>('monitor-send-text').addEventListener('keydown', (event) => {
      if (event.key === 'Enter') void this.sendToMonitor();
    });
    $('monitor-timestamp').addEventListener('change', () => this.renderMonitor());
    $('btn-stop').addEventListener('click', () => {
      this.abort?.abort();
      this.log('Отмена…', 'warn');
    });
    for (const button of document.querySelectorAll<HTMLButtonElement>('#family-switch .seg')) {
      button.addEventListener('click', () => {
        this.family = button.dataset.family as BoardFamily;
        this.syncFamilySwitch();
        this.log(
          this.family === 'avr'
            ? 'Режим AVR: сборка из скетча и прошивка через STK500'
            : 'Режим ESP: загрузите .bin (Arduino IDE → Экспорт бинарных файлов) и прошейте',
          'info',
        );
      });
    }

    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && this.busy) {
        this.abort?.abort();
        this.log('Отмена…', 'warn');
        return;
      }
      const mod = event.metaKey || event.ctrlKey;
      if (mod && event.key.toLowerCase() === 'b') {
        event.preventDefault();
        void this.build();
      }
      if (mod && event.key === 'Enter') {
        event.preventDefault();
        void this.flash();
      }
      if (mod && event.key.toLowerCase() === 's') {
        event.preventDefault();
        this.persist();
        this.log('Скетч сохранён локально', 'muted');
      }
    });

    // Drag & drop: .ino opens in the sketch, .hex/.bin goes to the flasher.
    const dropZone = document.body;
    ['dragenter', 'dragover'].forEach((type) =>
      dropZone.addEventListener(type, (event) => {
        event.preventDefault();
        $('welcome').classList.add('visible');
      }),
    );
    ['dragleave', 'drop'].forEach((type) =>
      dropZone.addEventListener(type, (event) => {
        event.preventDefault();
        if (type === 'dragleave' && (event as DragEvent).relatedTarget) return;
        $('welcome').classList.remove('visible');
      }),
    );
    dropZone.addEventListener('drop', (event) => {
      const files = (event as DragEvent).dataTransfer?.files;
      if (files?.length) void this.loadFilesFromDisk(files);
    });

    this.editor.view.dom.addEventListener('keyup', () => this.updateCursor());
    this.editor.view.dom.addEventListener('mouseup', () => this.updateCursor());
    window.setInterval(() => this.updateCursor(), 700);
  }

  private syncFamilySwitch(): void {
    for (const button of document.querySelectorAll<HTMLButtonElement>('#family-switch .seg')) {
      button.classList.toggle('active', button.dataset.family === this.family);
    }
    $('esp-files').hidden = this.family !== 'esp';
    $('board-field').hidden = this.family !== 'avr';
  }

  private bindSerialEvents(): void {
    if (!('serial' in navigator)) {
      $('btn-connect').setAttribute('title', 'Web Serial недоступен в этом браузере');
      this.log('Web Serial недоступен: откройте сайт в Chrome или Edge на компьютере.', 'warn');
      return;
    }
    navigator.serial.addEventListener('disconnect', (event) => {
      const port = (event as Event & { target?: SerialPort }).target;
      if (port && this.uploader.connected) {
        void this.uploader.disconnect();
        this.renderPortBadge();
        this.log('Плата отключена от USB', 'warn');
      }
    });
    void navigator.serial.getPorts().then((ports) => {
      if (ports.length === 1) {
        void this.uploader.attachPort(ports[0]).then(() => {
          this.renderPortBadge();
          this.log(`Найден ранее разрешённый порт: ${describePort(ports[0]).label}`, 'muted');
        });
      }
    });
  }
}

function download(blob: Blob, name: string): void {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = name;
  anchor.click();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function parseHexOrThrow(text: string): HexImage {
  const parsed = parseIntelHex(text);
  if (!parsed.dataBytes) throw new Error('в файле нет данных');
  return parsed;
}

// eslint-disable-next-line no-new
new App();
