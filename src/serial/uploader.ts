import { AbortGate, SerialLink } from './link.ts';
import { ATMEGA328P, type AvrBoard, type ProgressFn } from './stk500.ts';
import { flashEsp, type EspImage } from './esp.ts';
import { flashAvr } from './stk500.ts';

export type BoardFamily = 'avr' | 'esp';

export interface PortInfo {
  family: BoardFamily;
  label: string;
  usbVendorId?: number;
  usbProductId?: number;
}

/** Known USB-serial bridge chips and native USB PIDs, to guess the board family. */
const VENDORS: Record<number, string> = {
  0x2341: 'Arduino',
  0x2a03: 'Arduino',
  0x1a86: 'CH340/CH341',
  0x10c4: 'CP210x',
  0x0403: 'FTDI',
  0x303a: 'Espressif',
  0x1b4f: 'SparkFun',
  0x239a: 'Adafruit',
};

export function describePort(port: SerialPort): PortInfo {
  const info = (port as SerialPort & { getInfo?: () => { usbVendorId?: number; usbProductId?: number } }).getInfo?.() ?? {};
  const vendor = info.usbVendorId ? VENDORS[info.usbVendorId] ?? `USB ${info.usbVendorId.toString(16)}` : 'Serial';
  // Espressif's native USB vendor ID (0x303a) is a reliable ESP signal; bridge
  // chips are ambiguous (they sit on both AVR and ESP boards), so we pick ESP
  // only for the native-USB case and default the rest to AVR, which the user
  // can override with the family switch.
  const family: BoardFamily = info.usbVendorId === 0x303a ? 'esp' : 'avr';
  return {
    family,
    label: vendor,
    usbVendorId: info.usbVendorId,
    usbProductId: info.usbProductId,
  };
}

export interface UploadRequest {
  family: BoardFamily;
  avrBoard?: AvrBoard;
  /** AVR: flat flash image from the compiled sketch. */
  avrImage?: Uint8Array;
  /** ESP: images with flash addresses. */
  espImages?: EspImage[];
  baudRate?: number;
  eraseAll?: boolean;
  /** AVR progress: (done, total) in bytes. ESP: (written, total) of the current image. */
  onProgress?: ProgressFn;
  onLog?: (message: string) => void;
  signal?: AbortSignal;
}

export class Uploader {
  private port: SerialPort | null = null;
  private link: SerialLink | null = null;
  private monitor: SerialLink | null = null;

  get connected(): boolean {
    return this.port !== null;
  }

  get portLabel(): string {
    return this.port ? describePort(this.port).label : '';
  }

  async requestPort(): Promise<PortInfo> {
    if (!('serial' in navigator)) {
      throw new Error('Браузер не поддерживает Web Serial. Откройте страницу в Chrome или Edge на компьютере.');
    }
    const port = await navigator.serial.requestPort();
    await this.disconnect();
    this.port = port;
    return describePort(port);
  }

  async attachPort(port: SerialPort): Promise<PortInfo> {
    await this.disconnect();
    this.port = port;
    return describePort(port);
  }

  get monitoring(): boolean {
    return this.monitor !== null;
  }

  /** Open the port as a plain serial terminal (no bootloader protocol). */
  async startMonitor(baudRate: number, onData: (text: string) => void): Promise<void> {
    if (!this.port) throw new Error('Сначала подключите плату');
    await this.stopMonitor();
    await this.closePortQuietly();
    const link = new SerialLink(this.port);
    const decoder = new TextDecoder();
    link.onData((chunk) => onData(decoder.decode(chunk, { stream: true })));
    await link.open(baudRate);
    this.monitor = link;
  }

  async send(data: Uint8Array): Promise<void> {
    if (!this.monitor) throw new Error('Монитор порта не открыт');
    await this.monitor.write(data);
  }

  async stopMonitor(): Promise<void> {
    if (!this.monitor) return;
    const link = this.monitor;
    this.monitor = null;
    await link.close();
  }

  private async closePortQuietly(): Promise<void> {
    try {
      await this.port?.close();
    } catch {
      /* already closed */
    }
  }

  async disconnect(): Promise<void> {
    await this.stopMonitor();
    if (this.link) {
      await this.link.close();
      this.link = null;
    }
    this.port = null;
  }

  async upload(request: UploadRequest): Promise<{ summary: string }> {
    if (!this.port) throw new Error('Сначала подключите плату');
    await this.stopMonitor();
    if (request.family === 'avr') {
      return this.uploadAvr(request);
    }
    return this.uploadEsp(request);
  }

  private async uploadAvr(request: UploadRequest): Promise<{ summary: string }> {
    const gate = new AbortGate(request.signal);
    const board = request.avrBoard ?? ATMEGA328P;
    const link = new SerialLink(this.port!);
    this.link = link;
    try {
      await link.open(board.baudRate);
      const result = await flashAvr(link, request.avrImage ?? new Uint8Array(), {
        board,
        gate,
        onLog: request.onLog,
        onProgress: request.onProgress,
      });
      return {
        summary: `ATmega328P · ${result.bytes} Б · проверено за ${(result.ms / 1000).toFixed(1)} с`,
      };
    } finally {
      await link.close();
      this.link = null;
    }
  }

  private async uploadEsp(request: UploadRequest): Promise<{ summary: string }> {
    const gate = new AbortGate(request.signal);
    try {
      const result = await flashEsp(this.port!, request.espImages ?? [], {
        baudRate: request.baudRate,
        eraseAll: request.eraseAll,
        onLog: request.onLog,
        onProgress: (written, total) => request.onProgress?.(written, total),
        gate,
      });
      return {
        summary: `${result.chip} · ${result.bytes} Б · ${(result.ms / 1000).toFixed(1)} с`,
      };
    } catch (error) {
      // esptool closes the port on failure; keep the selection so the user can retry.
      throw error;
    }
  }
}
