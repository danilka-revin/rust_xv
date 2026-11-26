/**
 * Guards the index.html <-> main.ts contract: every element the app reaches for
 * by id must exist in the markup, and every tab/selector hook must be present.
 * A missing id would throw on startup, which is easy to miss in review.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const app = readFileSync(new URL('../src/main.ts', import.meta.url), 'utf8');

const htmlIds = new Set([...html.matchAll(/id="([^"]+)"/g)].map((m) => m[1]));
const usedIds = new Set([...app.matchAll(/\$\s*(?:<[^>]+>)?\s*\(\s*'([^']+)'\s*\)/g)].map((m) => m[1]));

const missing = [...usedIds].filter((id) => !htmlIds.has(id));
assert.deepEqual(missing, [], `main.ts refers to ids missing from index.html: ${missing.join(', ')}`);

// CSS classes toggled from JS should exist in the stylesheet, otherwise the UI
// silently stops reacting.
const css = readFileSync(new URL('../src/styles.css', import.meta.url), 'utf8');
for (const className of ['busy', 'collapsed', 'has-errors', 'has-warnings', 'live', 'over', 'active', 'dirty', 'hidden', 'visible']) {
  assert.ok(css.includes(`.${className}`), `styles.css has no rule for .${className}`);
}

// Every tab in the markup must be handled by the switch and the CSS.
const htmlTabs = [...html.matchAll(/data-tab="([^"]+)"/g)].map((m) => m[1]);
assert.ok(htmlTabs.length >= 3, 'expected log/problems/monitor tabs');
for (const tab of htmlTabs) {
  assert.ok(app.includes(`'${tab}'`) || app.includes(`"${tab}"`), `main.ts ignores the ${tab} tab`);
  assert.ok(html.includes(`id="output-${tab}"`), `missing panel for the ${tab} tab`);
}

console.log(`  ok   ${usedIds.size} ids, ${htmlTabs.length} tabs, styles consistent`);

// `hidden` is toggled from JS on elements that also carry their own display
// rule (.btn, .welcome), so the stylesheet must force it.
assert.ok(css.includes('[hidden]'), 'styles.css must force [hidden] { display: none }');
console.log('  ok   [hidden] override present');
