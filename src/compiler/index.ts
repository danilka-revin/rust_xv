import { parseIntelHex, type HexImage } from '../hex.ts';
import { mapLine, mergeInoTabs, prepareSketch, type PreparedSketch } from '../sketch.ts';
import type { BuildResponse } from './worker.ts';

export interface Diagnostic {
  /** 1-based line in the user's own file; 0 for messages without a location. */
  line: number;
  column: number;
  severity: 'error' | 'warning' | 'note' | 'info';
  message: string;
}

export interface BuildOutcome {
  ok: boolean;
  hex?: string;
  image?: HexImage;
  flashBytes: number;
  appFlashBytes: number;
  /** Very rough static RAM estimate (globals + common String buffers). */
  ramBytes: number;
  ms: number;
  diagnostics: Diagnostic[];
  /** Raw, de-noised tool output for the output panel. */
  log: string[];
  prepared: PreparedSketch;
}

const TOOL_PREFIX = /^\[[a-z0-9-]+\]\s?/;
const NOISE = [
  /wasm streaming compile failed/i,
  /falling back to ArrayBuffer instantiation/i,
  /^\[.*\]\s*$/,
];

/** Turn raw cc1plus/ld output into structured diagnostics attached to sketch lines. */
export function parseDiagnostics(raw: string[], prepared: PreparedSketch): { diagnostics: Diagnostic[]; log: string[] } {
  const diagnostics: Diagnostic[] = [];
  const log: string[] = [];

  for (const entry of raw) {
    const line = entry.replace(TOOL_PREFIX, '');
    if (!line.trim()) continue;
    if (NOISE.some((pattern) => pattern.test(line))) continue;

    const located = /\/build\/HorangFirmware\.cpp:(\d+):(\d+):\s*(fatal error|error|warning|note):\s*(.*)/.exec(line);
    if (located) {
      const severity = located[3] === 'note' ? 'note' : located[3] === 'warning' ? 'warning' : 'error';
      diagnostics.push({
        line: mapLine(Number(located[1]), prepared),
        column: Number(located[2]),
        severity,
        message: located[4],
      });
      log.push(line);
      continue;
    }

    const unlocated = /\/build\/HorangFirmware\.cpp:(\d+):\s*(fatal error|error|warning|note):\s*(.*)/.exec(line);
    if (unlocated) {
      const severity = unlocated[2] === 'note' ? 'note' : unlocated[2] === 'warning' ? 'warning' : 'error';
      diagnostics.push({
        line: mapLine(Number(unlocated[1]), prepared),
        column: 0,
        severity,
        message: unlocated[3],
      });
      log.push(line);
      continue;
    }

    if (/undefined reference to `(setup|loop)'/.test(line)) continue;
    if (/in function `main'/.test(line)) continue;
    if (/did not produce/.test(line)) continue;

    if (/error:|fatal error:/.test(line)) {
      diagnostics.push({ line: 0, column: 0, severity: 'error', message: line.replace(/^.*?error:\s*/, '') });
    } else if (/warning:/.test(line)) {
      diagnostics.push({ line: 0, column: 0, severity: 'warning', message: line.replace(/^.*?warning:\s*/, '') });
    }
    log.push(line);
  }

  return { diagnostics, log };
}

let worker: Worker | null = null;
let nextRequest = 1;
const pending = new Map<number, (response: BuildResponse) => void>();

function assetsBase(): string {
  // Vite copies public/ into the build root. BASE_URL keeps the path correct
  // when the app is served from a sub-path; the document base is the fallback
  // for bundles that do not define it (e.g. the IIFE build used by tests).
  const base = (import.meta.env?.BASE_URL ?? './').replace(/\/$/, '');
  const root = document.baseURI || window.location.href;
  return new URL(`${base}/avr/`, root).href;
}

/** Worker URL, with a fallback for non-module bundles where import.meta is absent. */
function workerUrl(): URL {
  try {
    return new URL('./worker.ts', import.meta.url);
  } catch {
    return new URL('worker.js', document.baseURI || window.location.href);
  }
}

function ensureWorker(): Worker {
  if (worker) return worker;
  worker = new Worker(workerUrl(), { type: 'module' });
  worker.addEventListener('message', (event: MessageEvent<BuildResponse>) => {
    const resolve = pending.get(event.data.id);
    if (!resolve) return;
    pending.delete(event.data.id);
    resolve(event.data);
  });
  worker.addEventListener('error', (event) => {
    for (const [, resolve] of pending) {
      resolve({ id: -1, ok: false, error: event.message || 'Worker упал' });
    }
    pending.clear();
  });
  return worker;
}

export interface CompileInput {
  files: { name: string; content: string }[];
  mainFile: string;
}

/**
 * Estimate static SRAM use the way a human squints at a sketch: global scalars,
 * arrays and String objects, plus the runtime's own buffers. It is deliberately
 * conservative and labelled as an estimate in the UI — the exact figure needs
 * the ELF's .data/.bss sizes, which the toolchain result does not expose.
 */
export function estimateRam(source: string): number {
  let bytes = 32; // core runtime state (millis, timer 0 bookkeeping, …)
  const globals = source.replace(/#[^\n]*/g, '');
  const lines = globals.split('\n');
  let depth = 0;
  for (const raw of lines) {
    const line = raw.trim();
    if (!line || line.startsWith('//')) {
      depth += (raw.match(/{/g)?.length ?? 0) - (raw.match(/}/g)?.length ?? 0);
      continue;
    }
    if (depth === 0) {
      // Arrays: type name[size];
      const array = /\b(byte|char|uint8_t|int|unsigned int|uint16_t|long|uint32_t|float|double)\s+\w+\s*\[([^\]]*)\]/.exec(line);
      if (array) {
        const size = Number.parseInt(array[2], 10);
        const cell = /byte|char|uint8_t/.test(array[1]) ? 1 : /float|long|uint32_t|double/.test(array[1]) ? 4 : 2;
        bytes += (Number.isFinite(size) ? size : 64) * cell;
      } else if (/\bString\s+\w+/.test(line)) {
        bytes += 24;
      } else if (/\b(byte|char|uint8_t|int|unsigned int|uint16_t|long|uint32_t|float|double|bool|boolean)\s+\w+/.test(line)) {
        bytes += 2;
      } else if (/\b(Servo|DHT|Adafruit_\w+|VL53L0X|SoftwareSerial)\s+\w+/.test(line)) {
        bytes += 48;
      }
    }
    depth += (raw.match(/{/g)?.length ?? 0) - (raw.match(/}/g)?.length ?? 0);
    depth = Math.max(0, depth);
  }
  if (/Serial\.begin|Serial\.print/.test(source)) bytes += 190; // HardwareSerial + rx/tx buffers
  if (/String\b/.test(source)) bytes += 32;
  if (/analogRead|analogWrite/.test(source)) bytes += 16;
  return Math.min(2048, bytes);
}

export async function compileSketch(input: CompileInput): Promise<BuildOutcome> {
  const merged = mergeInoTabs(input.files, input.mainFile);
  const prepared = prepareSketch(merged);
  const id = nextRequest++;
  const response = await new Promise<BuildResponse>((resolve) => {
    pending.set(id, resolve);
    ensureWorker().postMessage({ id, source: prepared.code, assetsBase: assetsBase() });
  });

  if (!response.ok || !response.hex) {
    const raw = (response.error ?? 'Неизвестная ошибка компиляции').split('\n');
    const { diagnostics, log } = parseDiagnostics(raw, prepared);
    if (!diagnostics.length) {
      diagnostics.push({ line: 0, column: 0, severity: 'error', message: response.error ?? 'Сборка не удалась' });
    }
    return {
      ok: false,
      flashBytes: 0,
      appFlashBytes: 32256,
      ramBytes: 0,
      ms: response.ms ?? 0,
      diagnostics,
      log,
      prepared,
    };
  }

  const { diagnostics, log } = parseDiagnostics(response.stderr ?? [], prepared);
  let image: HexImage | undefined;
  try {
    image = parseIntelHex(response.hex);
  } catch (error) {
    diagnostics.push({ line: 0, column: 0, severity: 'error', message: `Не удалось разобрать HEX: ${String(error)}` });
  }

  return {
    ok: !!image,
    hex: response.hex,
    image,
    flashBytes: response.flashBytes ?? image?.dataBytes ?? 0,
    appFlashBytes: response.appFlashBytes ?? 32256,
    ramBytes: estimateRam(prepared.code),
    ms: response.ms ?? 0,
    diagnostics,
    log,
    prepared,
  };
}
