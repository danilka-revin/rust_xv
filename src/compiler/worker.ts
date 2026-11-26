import { buildFirmware } from '@horang-corp/avr-gcc-wasm/firmware-builder';

/**
 * Compiles a prepared Arduino sketch for the ATmega328P inside a dedicated
 * worker, so the editor stays responsive while ~17 MB of wasm tooling runs.
 */

export interface BuildRequest {
  id: number;
  source: string;
  assetsBase: string;
}

export interface BuildResponse {
  id: number;
  ok: boolean;
  hex?: string;
  flashBytes?: number;
  appFlashBytes?: number;
  stderr?: string[];
  error?: string;
  ms?: number;
}

self.addEventListener('message', async (event: MessageEvent<BuildRequest>) => {
  const { id, source, assetsBase } = event.data;
  const started = performance.now();
  try {
    // Both optional library groups (OLED / TOF) are always linked in; the AVR
    // linker's --gc-sections drops whatever the sketch does not reference.
    const result = await buildFirmware({ source, sensors: ['OLED', 'TOF'], assetsBase });
    const response: BuildResponse = {
      id,
      ok: true,
      hex: result.hex,
      flashBytes: result.flashBytes,
      appFlashBytes: result.target?.appFlashBytes ?? 32256,
      stderr: result.stderr ?? [],
      ms: performance.now() - started,
    };
    self.postMessage(response);
  } catch (error) {
    const response: BuildResponse = {
      id,
      ok: false,
      error: error instanceof Error ? error.message : String(error),
      stderr: [],
      ms: performance.now() - started,
    };
    self.postMessage(response);
  }
});
