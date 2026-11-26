/**
 * Unit checks for the pure parts of the app: hex parsing, Arduino sketch
 * preparation (prototypes / line mapping) and compiler output parsing.
 * Run with `npm test` (Node's type stripping, no build step).
 */
import assert from 'node:assert/strict';
import { parseIntelHex, toIntelHex } from '../src/hex.ts';
import { collectPrototypes, mapLine, mergeInoTabs, prepareSketch } from '../src/sketch.ts';
import { readFileSync } from 'node:fs';
import { estimateRam, parseDiagnostics } from '../src/compiler/index.ts';

let passed = 0;
function test(name: string, fn: () => void): void {
  try {
    fn();
    passed++;
    console.log(`  ok   ${name}`);
  } catch (error) {
    console.error(`  FAIL ${name}`);
    console.error(error);
    process.exitCode = 1;
  }
}

console.log('hex');

test('round-trips a binary image through Intel HEX', () => {
  const data = new Uint8Array(40);
  for (let i = 0; i < data.length; i++) data[i] = (i * 7) & 0xff;
  const parsed = parseIntelHex(toIntelHex(data));
  assert.deepEqual(Array.from(parsed.data), Array.from(data));
  assert.equal(parsed.dataBytes, 40);
});

test('respects extended segment/linear address records', () => {
  const hex = [':020000040001F9', ':0400000001020304F2', ':00000001FF'].join('\n');
  const parsed = parseIntelHex(hex);
  assert.equal(parsed.start, 0x10000);
  assert.deepEqual(Array.from(parsed.data.slice(0x10000)), [1, 2, 3, 4]);
});

test('rejects a broken checksum', () => {
  assert.throws(() => parseIntelHex(':0400000001020304F3\n:00000001FF'), /контрольная сумма/i);
});

test('parses a HEX produced by the real AVR toolchain', () => {
  const hex = readFileSync(new URL('./fixtures/blink.hex', import.meta.url), 'utf8');
  const image = parseIntelHex(hex);
  assert.equal(image.start, 0);
  assert.ok(image.end > 4000 && image.end < 32256, `unexpected flash size ${image.end}`);
  assert.equal(image.data.length, image.end + 1);
  // The ATmega328P reset vector jumps into the sketch: first word is a JMP.
  assert.ok(image.data[0] > 0x80 || image.data[1] > 0, 'no reset vector');
  // Re-encoding the image must produce a loadable HEX again.
  const again = parseIntelHex(toIntelHex(image.data));
  assert.deepEqual(Array.from(again.data), Array.from(image.data));
});

console.log('sketch preparation');

test('adds Arduino.h and prototypes for helpers used above their definition', () => {
  const source = ['void setup() { blink(3); }', 'void loop() {}', '', 'void blink(int times) {', '  (void)times;', '}'].join('\n');
  const prepared = prepareSketch(source);
  assert.match(prepared.code, /^#include <Arduino\.h>/);
  assert.match(prepared.code, /void blink\(int times\);/);
  assert.equal(prepared.protoCount, 2); // step-up: blink
  const classic = prepareSketch(['void setup() {', '}', 'void loop() {', '}'].join('\n'));
  assert.equal(classic.protoCount, 2);
});

test('keeps preprocessor lines at the very top', () => {
  const source = ['#include <Servo.h>', '#define PIN 9', '', 'Servo s;', 'void setup() {}', 'void loop() {}'].join('\n');
  const prepared = prepareSketch(source);
  const lines = prepared.code.split('\n');
  assert.equal(lines[0], '#include <Arduino.h>');
  assert.equal(lines[1], '#include <Servo.h>');
  assert.equal(lines[2], '#define PIN 9');
  assert.ok(lines.some((line) => line.includes('Servo s;')));
});

test('ignores control-flow blocks and comments', () => {
  const source = ['void setup() {', '  if (digitalRead(2)) {', '    digitalWrite(13, HIGH);', '  }', '}', 'void loop() {}', '// void fake() {', '/* void blocky() { */'].join('\n');
  const prototypes = collectPrototypes(source);
  assert.deepEqual(prototypes.sort(), ['void loop();', 'void setup();']);
});

test('maps generated line numbers back to the user file', () => {
  const source = ['void setup() {', '  oops();', '}', 'void loop() {}'].join('\n');
  const prepared = prepareSketch(source);
  assert.equal(mapLine(1, prepared), 0);
  assert.equal(mapLine(prepared.generatedLines + 2, prepared), 2);
});

test('merges .ino tabs with the sketch file first', () => {
  const merged = mergeInoTabs([{ name: 'b.ino', content: 'int b;' }, { name: 'main.ino', content: 'int a;' }], 'main.ino');
  assert.ok(merged.indexOf('main.ino') < merged.indexOf('b.ino'));
});

console.log('diagnostics');

test('attaches compiler errors to sketch lines and drops tool noise', () => {
  const prepared = prepareSketch(['void setup() {', '  nope();', '}', 'void loop() {}'].join('\n'));
  const raw = [
    '[cc1plus] wasm streaming compile failed: Error',
    '[cc1plus] falling back to ArrayBuffer instantiation',
    `[cc1plus] /build/HorangFirmware.cpp:${prepared.headerLines + prepared.protoCount + 3}:3: error: 'nope' was not declared in this scope`,
    '[avr-ld] /objects/core_main.o: in function `main\':',
    "[avr-ld] undefined reference to `setup'",
  ];
  const { diagnostics, log } = parseDiagnostics(raw, prepared);
  assert.equal(diagnostics.length, 1);
  assert.equal(diagnostics[0].severity, 'error');
  assert.equal(diagnostics[0].line, 2);
  assert.match(diagnostics[0].message, /nope/);
  assert.ok(log.every((line) => !line.includes('wasm streaming')));
});

console.log('ram estimate');

test('counts global arrays and ignores locals', () => {
  const small = estimateRam(['void setup() {}', 'void loop() {}'].join('\n'));
  const big = estimateRam(['char buffer[128];', 'int values[64];', 'void setup() {}', 'void loop() { int local; }'].join('\n'));
  assert.ok(big > small + 128 + 128, `expected arrays to add ~256 B, got ${big - small}`);
});

test('adds serial buffers when Serial is used', () => {
  const quiet = estimateRam(['int counter;', 'void setup() {}', 'void loop() {}'].join('\n'));
  const noisy = estimateRam(['int counter;', 'void setup() { Serial.begin(9600); }', 'void loop() { Serial.println(counter); }'].join('\n'));
  assert.ok(noisy > quiet + 100);
});

test('never exceeds the chip RAM', () => {
  assert.ok(estimateRam('char huge[100000];'.repeat(10)) <= 2048);
});

console.log(`\n${passed} checks passed`);
