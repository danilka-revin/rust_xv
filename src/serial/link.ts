/**
 * Serial transport for board flashing.
 *
 * A tiny wrapper over the Web Serial API: connects, buffers incoming bytes,
 * and lets a protocol implementation wait for an exact number of bytes.
 * Everything here is cancelled cleanly on disconnect so the port can be
 * reopened for the next action.
 */

export class SerialError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'SerialError';
  }
}

export class SerialAbort extends Error {
  constructor() {
    super('Отменено');
    this.name = 'SerialAbort';
  }
}

type Listener = (chunk: Uint8Array) => void;

export class SerialLink {
  private port: SerialPort;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private readLoop: Promise<void> | null = null;
  private buffer = new Uint8Array(0);
  private listeners = new Set<Listener>();
  private closed = false;

  constructor(port: SerialPort) {
    this.port = port;
  }

  get isOpen(): boolean {
    return this.writer !== null && !this.closed;
  }

  async open(baudRate: number): Promise<void> {
    if (!this.port.readable) {
      await this.port.open({ baudRate, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 4096 });
    }
    this.closed = false;
    this.writer = this.port.writable?.getWriter() ?? null;
    if (!this.writer) {
      // The port was opened without a writable stream (older Chromium builds).
      await this.port.close();
      await this.port.open({ baudRate, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 4096 });
      this.writer = this.port.writable?.getWriter() ?? null;
    }
    if (!this.writer) throw new SerialError('Порт открыт, но недоступен для записи');
    this.readLoop = this.pump();
  }

  private async pump(): Promise<void> {
    const readable = this.port.readable;
    if (!readable) return;
    const reader = readable.getReader();
    try {
      while (!this.closed) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value || value.length === 0) continue;
        this.push(value);
      }
    } catch {
      // Device unplugged / port closed while reading: nothing left to do.
    } finally {
      try {
        reader.releaseLock();
      } catch {
        /* already released */
      }
    }
  }

  private push(chunk: Uint8Array): void {
    const merged = new Uint8Array(this.buffer.length + chunk.length);
    merged.set(this.buffer, 0);
    merged.set(chunk, this.buffer.length);
    this.buffer = merged;
    for (const listener of this.listeners) listener(chunk);
  }

  /** Forget everything received so far (used before issuing a command). */
  flushInput(): void {
    this.buffer = new Uint8Array(0);
  }

  async write(data: Uint8Array): Promise<void> {
    if (!this.writer) throw new SerialError('Порт не открыт');
    await this.writer.write(data);
  }

  /** Wait until at least `length` bytes are buffered, then return them. */
  async read(length: number, timeoutMs: number): Promise<Uint8Array> {
    const deadline = performance.now() + timeoutMs;
    while (this.buffer.length < length) {
      if (this.closed) throw new SerialError('Порт закрыт');
      if (performance.now() > deadline) {
        throw new SerialError(`Таймаут: плата не ответила за ${timeoutMs} мс (получено ${this.buffer.length} из ${length} байт)`);
      }
      await sleep(4);
    }
    const out = this.buffer.slice(0, length);
    this.buffer = this.buffer.slice(length);
    return out;
  }

  /** Subscribe to raw incoming bytes; returns an unsubscribe function. */
  onData(callback: Listener): () => void {
    this.listeners.add(callback);
    return () => this.listeners.delete(callback);
  }

  /** Wait for data, then flush whatever else arrived shortly after. */
  async drain(quietMs = 120): Promise<void> {
    await sleep(quietMs);
    this.flushInput();
  }

  async setDTR(value: boolean): Promise<void> {
    try {
      await this.port.setSignals({ dataTerminalReady: value, requestToSend: false });
    } catch {
      /* some bridges do not expose modem lines */
    }
  }

  async close(): Promise<void> {
    this.closed = true;
    try {
      await this.readLoop;
    } catch {
      /* ignore */
    }
    this.readLoop = null;
    try {
      this.writer?.releaseLock();
    } catch {
      /* ignore */
    }
    this.writer = null;
    for (const listener of [...this.listeners]) this.listeners.delete(listener);
    try {
      await this.port.close();
    } catch {
      /* already closed */
    }
  }
}

export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export class AbortGate {
  private aborted = false;
  private external?: AbortSignal;

  constructor(external?: AbortSignal) {
    this.external = external;
    external?.addEventListener('abort', () => this.abort(), { once: true });
  }

  abort(): void {
    this.aborted = true;
  }

  check(): void {
    if (this.aborted || this.external?.aborted) throw new SerialAbort();
  }

  async sleep(ms: number): Promise<void> {
    const step = 25;
    for (let waited = 0; waited < ms; waited += step) {
      this.check();
      await sleep(Math.min(step, ms - waited));
    }
    this.check();
  }
}
