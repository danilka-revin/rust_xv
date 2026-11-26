/**
 * Intel HEX parsing / conversion.
 *
 * The AVR flash pipeline needs a flat byte image: the flasher writes whole
 * pages, and the UI wants to show how many bytes of flash a sketch will use.
 */

export interface HexImage {
  /** Flash contents, byte 0 == address 0. Gaps are filled with 0xFF. */
  data: Uint8Array;
  /** First and last address covered by data records (inclusive). */
  start: number;
  end: number;
  /** Number of bytes that actually came from data records. */
  dataBytes: number;
}

export class HexError extends Error {
  readonly line?: number;

  constructor(message: string, line?: number) {
    super(message);
    this.name = 'HexError';
    this.line = line;
  }
}

export function parseIntelHex(text: string): HexImage {
  let base = 0;
  let start = Number.POSITIVE_INFINITY;
  let end = -1;
  let dataBytes = 0;
  const chunks: { address: number; data: Uint8Array }[] = [];

  const lines = text.split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    if (!line.startsWith(':')) throw new HexError('Запись Intel HEX должна начинаться с ":"', i + 1);

    const raw = line.slice(1);
    if (raw.length < 10 || raw.length % 2 !== 0) throw new HexError('Некорректная длина записи HEX', i + 1);

    const bytes = new Uint8Array(raw.length / 2);
    for (let b = 0; b < bytes.length; b++) {
      const value = Number.parseInt(raw.slice(b * 2, b * 2 + 2), 16);
      if (Number.isNaN(value)) throw new HexError('В записи HEX есть недопустимые символы', i + 1);
      bytes[b] = value;
    }

    const count = bytes[0];
    const address = (bytes[1] << 8) | bytes[2];
    const type = bytes[3];
    if (bytes.length !== count + 5) throw new HexError('Счётчик байт не совпадает с длиной записи', i + 1);

    let checksum = 0;
    for (const byte of bytes) checksum = (checksum + byte) & 0xff;
    if (checksum !== 0) throw new HexError('Неверная контрольная сумма записи HEX', i + 1);

    if (type === 0x00) {
      const data = bytes.slice(4, 4 + count);
      const offset = base + address;
      chunks.push({ address: offset, data });
      start = Math.min(start, offset);
      end = Math.max(end, offset + count - 1);
      dataBytes += count;
    } else if (type === 0x01) {
      break;
    } else if (type === 0x02) {
      base = ((bytes[4] << 8) | bytes[5]) * 16;
    } else if (type === 0x04) {
      base = ((bytes[4] << 8) | bytes[5]) * 0x10000;
    } else if (type === 0x03 || type === 0x05) {
      // Start address records carry no flash contents.
      continue;
    } else {
      throw new HexError(`Неизвестный тип записи HEX: 0x${type.toString(16)}`, i + 1);
    }
  }

  if (end < 0) return { data: new Uint8Array(0), start: 0, end: 0, dataBytes: 0 };

  const size = end + 1;
  const data = new Uint8Array(size).fill(0xff);
  for (const chunk of chunks) data.set(chunk.data, chunk.address);
  return { data, start, end, dataBytes };
}

/** Turn a raw binary image (e.g. an Arduino IDE "Export compiled binary") into a hex-less image. */
export function binaryImage(bytes: Uint8Array): HexImage {
  let end = bytes.length - 1;
  while (end >= 0 && bytes[end] === 0xff) end--;
  if (end < 0) return { data: new Uint8Array(0), start: 0, end: 0, dataBytes: 0 };
  return { data: bytes.slice(0, end + 1), start: 0, end, dataBytes: end + 1 };
}

/** Intel HEX for a binary image starting at `startAddress`. */
export function toIntelHex(data: Uint8Array, startAddress = 0): string {
  const out: string[] = [];
  const record = (type: number, address: number, payload: Uint8Array) => {
    const bytes = [payload.length, (address >> 8) & 0xff, address & 0xff, type, ...payload];
    let sum = 0;
    for (const byte of bytes) sum = (sum + byte) & 0xff;
    const checksum = (0x100 - sum) & 0xff;
    out.push(':' + [...bytes, checksum].map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join(''));
  };

  let upper = 0xffff;
  for (let offset = 0; offset < data.length; offset += 16) {
    const chunk = data.slice(offset, Math.min(offset + 16, data.length));
    const absolute = startAddress + offset;
    const high = Math.floor(absolute / 0x10000);
    if (high !== upper) {
      upper = high;
      record(0x04, 0, new Uint8Array([(high >> 8) & 0xff, high & 0xff]));
    }
    record(0x00, absolute & 0xffff, chunk);
  }
  out.push(':00000001FF');
  return out.join('\n') + '\n';
}
