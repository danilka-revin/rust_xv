/**
 * Renders the built app in jsdom and walks through the main flows: startup,
 * local storage, compiling through the worker (stubbed) and error reporting.
 * `npm run test:dom` builds dist-test/ first.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';

let passed = 0;
async function check(label: string, fn: () => boolean | void | Promise<boolean | void>): Promise<void> {
  try {
    const result = await fn();
    assert.notEqual(result, false, label);
    passed++;
    console.log(`  ok   ${label}`);
  } catch (error) {
    console.error(`  FAIL ${label}`);
    console.error(error instanceof Error ? error.message : error);
    process.exitCode = 1;
  }
}

const markup = readFileSync(new URL('../dist-test/index.html', import.meta.url), 'utf8');
const bundle = readFileSync(new URL('../dist-test/app.js', import.meta.url), 'utf8');
const blinkHex = readFileSync(new URL('./fixtures/blink.hex', import.meta.url), 'utf8');

const dom = new JSDOM(markup, { url: 'http://localhost/', runScripts: 'outside-only', pretendToBeVisual: true });
// Node's `location` is read-only; the bundle reads the document URL through it.
Object.defineProperty(globalThis, 'location', { value: dom.window.location, configurable: true });
const { window } = dom;
const document = window.document;

// Web Serial is unavailable in jsdom, as in Safari — the app must degrade, not crash.
const workerMessages: any[] = [];
let workerListener: ((event: { data: unknown }) => void) | null = null;
class FakeWorker {
  constructor(url: URL) {
    (window as any).__workerUrl = String(url);
  }
  addEventListener(type: string, listener: (event: { data: unknown }) => void) {
    if (type === 'message') workerListener = listener;
  }
  postMessage(message: unknown) {
    workerMessages.push(message);
  }
  terminate() {}
}
(window as any).Worker = FakeWorker;
(window as any).requestAnimationFrame = (fn: () => void) => window.setTimeout(fn, 0);

// jsdom has no layout, but CodeMirror's highlight layer needs these calls to exist.
(window as any).Range.prototype.getClientRects = () => [] as unknown as DOMRectList;
(window as any).Range.prototype.getBoundingClientRect = () => new (window as any).DOMRect();
(window as any).document.elementFromPoint = () => null;

window.eval(bundle);

if (process.env.DEBUG_DOM) {
  window.eval('try { console.log("CHECK", location.href, document.baseURI, new URL("./avr/", document.baseURI).href) } catch (e) { console.log("CHECK ERR", String(e)); }');
}

const $ = (id: string) => document.getElementById(id)!;
const logLines = () => $('output-log').querySelectorAll('.log-line').length;

await check('app boots and loads the default sketch', () => {
  return ($('sketch-name') as HTMLInputElement).value === 'Blink';
});

await new Promise((resolve) => window.setTimeout(resolve, 30));

await check('sketch and active id are persisted', () => {
  const stored = window.localStorage.getItem('sketchflash.sketches.v1');
  assert.ok(stored && JSON.parse(stored)[0].name === 'Blink');
  return true;
});

await check('sidebar lists the sketch, examples and files', () => {
  assert.equal($('sketch-list').children.length, 1);
  assert.equal($('example-list').children.length, 7);
  assert.equal($('file-list').children.length, 1);
  assert.equal($('file-tabs').children.length, 1);
  return true;
});

await check('startup messages land in the output panel', () => logLines() >= 2);

await check('editor mounts CodeMirror with the sketch', () => {
  const content = document.querySelector('.cm-content');
  assert.ok(content, 'no .cm-content');
  assert.match(content!.textContent ?? '', /void loop/);
  return true;
});

await check('compile button shows the busy state and asks the worker', () => {
  ($('btn-compile') as HTMLButtonElement).click();
  assert.ok($('btn-compile').classList.contains('busy'));
  assert.equal(workerMessages.length, 1);
  const request = workerMessages[0] as { source: string; assetsBase: string };
  assert.match(request.source, /#include <Arduino\.h>/);
  assert.match(request.source, /void loop/);
  assert.equal(request.assetsBase, 'http://localhost/avr/');
  return true;
});

await check('successful build fills in sizes and enables export', () => {
  workerListener?.({
    data: {
      id: (workerMessages[0] as { id: number }).id,
      ok: true,
      hex: blinkHex,
      flashBytes: 5292,
      appFlashBytes: 32256,
      stderr: ['[cc1plus] wasm streaming compile failed: TypeError'],
      ms: 640,
    },
  });
  return true;
});

await new Promise((resolve) => window.setTimeout(resolve, 30));
if (process.env.DEBUG_DOM) console.log('DEBUG log:', $('output-log').textContent);
if (process.env.DEBUG_DOM) window.eval('try { new URL("./avr/", document.baseURI).href } catch (e) { console.log("URLERR", e.stack) }');

await check('flash usage reflects the built hex', () => {
  assert.match($('flash-numbers').textContent!, /^5\s?292/);
  assert.ok($('flash-fill').getAttribute('style')?.includes('%'));
  assert.ok($('build-meta').classList.contains('ok'));
  return true;
});

await check('RAM estimate is shown for the built sketch', () => {
  assert.match($('ram-numbers').textContent!, /≈/);
  return true;
});

await check('toolchain noise is filtered out of the log', () => {
  const text = $('output-log').textContent ?? '';
  assert.ok(!text.includes('wasm streaming'), 'noise leaked into the log');
  return true;
});

await check('a failed build reports problems and marks lines', async () => {
  ($('btn-compile') as HTMLButtonElement).click();
  const request = workerMessages[1] as { id: number };
  workerListener?.({
    data: {
      id: request.id,
      ok: false,
      error: [
        "[cc1plus] /build/HorangFirmware.cpp:12:3: error: 'nope' was not declared in this scope",
        '[cc1plus] falling back to ArrayBuffer instantiation',
      ].join('\n'),
      stderr: [],
      ms: 120,
    },
  });
  await new Promise((resolve) => window.setTimeout(resolve, 30));
  const problems = $('output-problems').querySelectorAll('.problem');
  assert.equal(problems.length, 1);
  assert.match(problems[0].textContent!, /nope/);
  assert.ok($('problem-count').classList.contains('has-errors'));
  assert.ok(document.querySelectorAll('.cm-diagnostic-error').length >= 1, 'no error line highlighted');
  return true;
});

await check('examples open in the editor', () => {
  const before = ($('sketch-name') as HTMLInputElement).value;
  const editorBefore = document.querySelector('.cm-content')!.textContent;
  ($('example-list') as unknown as { children: { click(): void }[] }).children[1].click();
  const after = ($('sketch-name') as HTMLInputElement).value;
  assert.notEqual(after, before);
  assert.notEqual($('sketch-name').textContent, editorBefore);
  assert.equal($('sketch-list').children.length, 2);
  return true;
});

await check('family switch reveals the ESP image panel', () => {
  const espSegment = document.querySelector('#family-switch .seg[data-family="esp"]') as HTMLElement;
  espSegment.click();
  assert.equal($('esp-files').hidden, false);
  assert.equal($('board-field').hidden, true);
  return true;
});

await check('output tabs switch panels', () => {
  (document.querySelector('.output-tab[data-tab="problems"]') as HTMLElement).click();
  assert.equal($('output-problems').classList.contains('hidden'), false);
  assert.equal($('output-log').classList.contains('hidden'), true);
  (document.querySelector('.output-tab[data-tab="monitor"]') as HTMLElement).click();
  assert.equal($('output-monitor').classList.contains('hidden'), false);
  return true;
});

await check('drag & drop of a .hex prepares it for flashing', async () => {
  const event = new window.Event('drop', { bubbles: true, cancelable: true }) as any;
  const file = new window.File([blinkHex], 'sketch.hex', { type: 'text/plain' });
  event.dataTransfer = { files: [file] };
  document.body.dispatchEvent(event);
  await new Promise((resolve) => window.setTimeout(resolve, 30));
  assert.match($('build-meta').textContent!, /из файла/);
  return true;
});

console.log(`\n${passed} DOM checks passed`);
// The app keeps a cursor-tracking interval alive; jsdom has no reason to stay up.
process.exit(process.exitCode ?? 0);
