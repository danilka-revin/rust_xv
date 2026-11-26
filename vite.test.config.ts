import { defineConfig } from 'vite';

/**
 * Single-file IIFE build used by the DOM smoke test (tests/dom.test.ts).
 * jsdom cannot execute ES modules, so the app is bundled as a classic script.
 */
export default defineConfig({
  base: './',
  build: {
    target: 'es2022',
    outDir: 'dist-test',
    emptyOutDir: true,
    minify: false,
    rollupOptions: {
      output: { format: 'iife', entryFileNames: 'app.js', assetFileNames: '[name][extname]' },
    },
  },
});
