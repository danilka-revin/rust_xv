import { cp, mkdir, stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Copies the AVR toolchain (wasm compilers + prebuilt core/libraries) out of
// node_modules into public/avr so the browser can fetch it at compile time.
// These files are large and generated, so public/avr stays out of git.

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'node_modules', '@horang-corp', 'avr-gcc-wasm');
const target = join(root, 'public', 'avr');

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

if (!(await exists(source))) {
  console.error('[assets] @horang-corp/avr-gcc-wasm is not installed — run npm install first.');
  process.exit(1);
}

if (await exists(join(target, 'assets', 'manifest.json')) && await exists(join(target, 'tools', 'cc1plus.wasm'))) {
  console.log('[assets] AVR toolchain already in public/avr');
  process.exit(0);
}

await mkdir(target, { recursive: true });
for (const dir of ['tools', 'assets']) {
  process.stdout.write(`[assets] copying ${dir} … `);
  await cp(join(source, dir), join(target, dir), { recursive: true, force: true });
  console.log('done');
}
console.log('[assets] AVR toolchain ready in public/avr');
