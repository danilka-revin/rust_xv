import { defineConfig } from 'vite';

export default defineConfig({
  base: './',
  server: {
    host: '0.0.0.0',
    port: 5173,
    strictPort: false,
    allowedHosts: true,
    watch: { ignored: ['**/dist/**', '**/dist-test/**', '**/.scratch/**'] },
  },
  worker: {
    format: 'es',
  },
  optimizeDeps: {
    // The AVR toolchain glue is plain ESM that resolves its own wasm files;
    // keep it out of the pre-bundler so URL resolution stays intact.
    exclude: ['@horang-corp/avr-gcc-wasm'],
  },
  build: {
    target: 'es2022',
    chunkSizeWarningLimit: 2048,
  },
});
