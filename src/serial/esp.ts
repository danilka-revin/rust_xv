/**
 * ESP32 / ESP8266 flashing through esptool-js.
 *
 * The UI can push either a single merged `.bin` (address 0x0) or a set of
 * images with explicit addresses (bootloader, partition table, app), which is
 * what a `.bin` exported from the Arduino IDE for an ESP board looks like.
 */

import { ESPLoader, Transport, type FlashFreqValues, type FlashModeValues, type FlashSizeValues } from 'esptool-js';
import { AbortGate } from './link.ts';

export interface EspImage {
  name: string;
  address: number;
  data: Uint8Array;
}

export interface EspFlashResult {
  chip: string;
  bytes: number;
  ms: number;
}

export interface EspFlashOptions {
  baudRate?: number;
  eraseAll?: boolean;
  onProgress?: (written: number, total: number) => void;
  onLog?: (message: string) => void;
  gate?: AbortGate;
}

export async function flashEsp(
  port: SerialPort,
  images: EspImage[],
  options: EspFlashOptions = {},
): Promise<EspFlashResult> {
  if (!images.length) throw new Error('Нет файлов для прошивки');
  const baudRate = options.baudRate ?? 460800;
  const log = options.onLog ?? (() => {});
  const started = performance.now();

  const transport = new Transport(port, false);
  const terminal = {
    clean: () => undefined,
    writeLine: (data: string) => log(data),
    write: (data: string) => {
      const text = data.trim();
      if (text) log(text);
    },
  };

  const loader = new ESPLoader({
    transport,
    baudrate: baudRate,
    romBaudrate: 115200,
    terminal,
    debugLogging: false,
  });

  try {
    log('Определение чипа…');
    const chip = await loader.main('default_reset');
    log(`Чип: ${chip}`);

    const fileArray = images.map((image) => ({ data: image.data, address: image.address }));
    await loader.writeFlash({
      fileArray,
      flashMode: 'keep' as FlashModeValues,
      flashFreq: 'keep' as FlashFreqValues,
      flashSize: 'keep' as FlashSizeValues,
      eraseAll: options.eraseAll ?? false,
      compress: true,
      reportProgress: (_index, written, total) => {
        options.onProgress?.(written, total);
        options.gate?.check();
      },
    });
    await loader.after('hard_reset');
    log('Плата перезапущена');

    return {
      chip,
      bytes: images.reduce((sum, image) => sum + image.data.length, 0),
      ms: performance.now() - started,
    };
  } finally {
    try {
      await transport.disconnect();
    } catch {
      /* port may already be gone */
    }
  }
}

/** Reboot an ESP into its normal firmware without writing anything. */
export async function resetEsp(port: SerialPort, log: (message: string) => void = () => {}): Promise<string> {
  const transport = new Transport(port, false);
  const loader = new ESPLoader({
    transport,
    baudrate: 115200,
    romBaudrate: 115200,
    terminal: { clean: () => undefined, writeLine: log, write: () => undefined },
  });
  try {
    const chip = await loader.main('default_reset');
    await loader.after('hard_reset');
    return chip;
  } finally {
    try {
      await transport.disconnect();
    } catch {
      /* ignore */
    }
  }
}
