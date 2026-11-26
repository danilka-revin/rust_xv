/**
 * STK500v1 bootloader programmer (what avrdude does for an Arduino Uno/Nano).
 *
 * Flow, as in Arduino's own upload: pulse DTR to reset into the bootloader,
 * then sync, read+verify the signature, set device params, enter programming
 * mode, write flash page by page, verify, and leave programming mode.
 */

import { AbortGate, SerialError, SerialLink } from './link.ts';

const STK = {
  GET_SYNC: 0x30,
  SET_DEVICE: 0x42,
  ENTER_PROGMODE: 0x50,
  LEAVE_PROGMODE: 0x51,
  LOAD_ADDRESS: 0x55,
  PROG_PAGE: 0x64,
  READ_PAGE: 0x74,
  READ_SIGN: 0x75,
  OK: 0x10,
  INSYNC: 0x14,
  NOSYNC: 0x15,
  EOP: 0x20,
} as const;

export interface AvrBoard {
  id: string;
  label: string;
  /** Device signature reported by the chip. */
  signature: [number, number, number];
  signatureName: string;
  pageSize: number;
  flashSize: number;
  baudRate: number;
}

export const ATMEGA328P: AvrBoard = {
  id: 'uno',
  label: 'Arduino Uno / Nano (ATmega328P)',
  signature: [0x1e, 0x95, 0x0f],
  signatureName: 'ATmega328P',
  pageSize: 128,
  flashSize: 32256,
  baudRate: 115200,
};

const TIMEOUT = 700;

export type ProgressFn = (done: number, total: number) => void;

export interface FlashResult {
  signature: string;
  bytes: number;
  pageSize: number;
  verified: boolean;
  ms: number;
}

class Stk500 {
  constructor(
    private link: SerialLink,
    private gate: AbortGate,
    private log: (msg: string) => void,
  ) {}

  private async command(payload: number[], expect: number, timeout = TIMEOUT): Promise<Uint8Array> {
    this.gate.check();
    this.link.flushInput();
    await this.link.write(new Uint8Array([...payload, STK.EOP]));
    const response = await this.link.read(expect, timeout);
    if (response[0] === STK.NOSYNC) throw new SerialError('Bootloader ответил NOSYNC');
    if (response[0] !== STK.INSYNC) throw new SerialError(`Неожиданный ответ bootloader: 0x${response[0].toString(16)}`);
    if (response[expect - 1] !== STK.OK) throw new SerialError('Bootloader не подтвердил команду (ожидался OK)');
    return response;
  }

  async sync(attempts = 12): Promise<void> {
    for (let i = 0; i < attempts; i++) {
      this.gate.check();
      this.link.flushInput();
      await this.link.write(new Uint8Array([STK.GET_SYNC, STK.EOP]));
      try {
        const response = await this.link.read(2, 250);
        if (response[0] === STK.INSYNC && response[1] === STK.OK) {
          this.log(`bootloader на связи (попытка ${i + 1})`);
          return;
        }
      } catch {
        /* retry */
      }
      await this.gate.sleep(60);
    }
    throw new SerialError('Не удалось синхронизироваться с bootloader. Проверьте порт и попробуйте ещё раз.');
  }

  async signature(): Promise<[number, number, number]> {
    const response = await this.command([STK.READ_SIGN], 5);
    return [response[1], response[2], response[3]];
  }

  async setDevice(pageSize: number, flashSize: number): Promise<void> {
    const flashWords = flashSize >> 1;
    await this.command(
      [
        STK.SET_DEVICE,
        0, // device code (not used by optiboot)
        0, // revision
        0xff, // progtype
        1, // parmode
        1, // polling
        1, // selftimed
        0xff, // lockbytes
        0xff, // fusebytes
        0xff, 0xff, // flash poll values
        0xff, 0xff, // eeprom poll values
        (pageSize >> 8) & 0xff,
        pageSize & 0xff,
        0x01, 0xe0, // eeprom size 512
        (flashWords >> 24) & 0xff,
        (flashWords >> 16) & 0xff,
        (flashWords >> 8) & 0xff,
        flashWords & 0xff,
      ],
      2,
    );
  }

  async enterProgrammingMode(): Promise<void> {
    await this.command([STK.ENTER_PROGMODE], 2);
  }

  async loadAddress(wordAddress: number): Promise<void> {
    await this.command([STK.LOAD_ADDRESS, wordAddress & 0xff, (wordAddress >> 8) & 0xff], 2);
  }

  async loadPage(data: Uint8Array): Promise<void> {
    await this.command([STK.PROG_PAGE, (data.length >> 8) & 0xff, data.length & 0xff, 0x46, ...data], 2);
  }

  async readPage(size: number): Promise<Uint8Array> {
    const response = await this.command([STK.READ_PAGE, (size >> 8) & 0xff, size & 0xff, 0x46], size + 2);
    return response.slice(1, 1 + size);
  }

  async leaveProgrammingMode(): Promise<void> {
    await this.command([STK.LEAVE_PROGMODE], 2);
  }
}

/**
 * Reset the board into its bootloader by pulsing DTR, the same way avrdude /
 * the Arduino IDE do it: hold the line low, then release it and give the
 * bootloader its ~1 s window.
 */
export async function resetIntoBootloader(link: SerialLink, gate: AbortGate): Promise<void> {
  await link.setDTR(false);
  await gate.sleep(250);
  await link.setDTR(true);
  await gate.sleep(50);
}

export async function flashAvr(
  link: SerialLink,
  image: Uint8Array,
  options: {
    board?: AvrBoard;
    onProgress?: ProgressFn;
    onLog?: (message: string) => void;
    gate?: AbortGate;
  } = {},
): Promise<FlashResult> {
  const board = options.board ?? ATMEGA328P;
  const gate = options.gate ?? new AbortGate();
  const log = options.onLog ?? (() => {});
  const started = performance.now();

  if (image.length > board.flashSize) {
    throw new SerialError(
      `Прошивка ${image.length} Б не влезает в ${board.flashSize} Б флеша ${board.signatureName}`,
    );
  }

  // Pad up to a whole page with 0xFF: the bootloader writes page-sized chunks.
  const padded = new Uint8Array(Math.ceil(image.length / board.pageSize) * board.pageSize || board.pageSize);
  padded.fill(0xff);
  padded.set(image);

  const stk = new Stk500(link, gate, log);

  log('Сброс платы в bootloader…');
  await resetIntoBootloader(link, gate);
  try {
    await stk.sync(10);
  } catch {
    // Second chance: some boards need the reset pulse again after the port
    // was opened (opening itself already toggles DTR on many USB bridges).
    log('Повторная попытка синхронизации…');
    await resetIntoBootloader(link, gate);
    await stk.sync(20);
  }
  log('Bootloader отвечает');

  const signature = await stk.signature();
  const signatureHex = signature.map((b) => b.toString(16).padStart(2, '0')).join(' ');
  const expected = board.signature.join(',');
  if (signature.join(',') !== expected) {
    throw new SerialError(
      `Подключён другой чип: подпись ${signatureHex} (ожидалась ${board.signature.map((b) => b.toString(16).padStart(2, '0')).join(' ')})`,
    );
  }
  log(`Чип определён: ${board.signatureName} (${signatureHex})`);

  await stk.setDevice(board.pageSize, board.flashSize);
  await stk.enterProgrammingMode();
  log('Режим программирования включён');

  const total = padded.length;
  for (let offset = 0; offset < total; offset += board.pageSize) {
    const page = padded.slice(offset, offset + board.pageSize);
    await stk.loadAddress(offset >> 1);
    await stk.loadPage(page);
    options.onProgress?.(Math.min(offset + board.pageSize, total) * 0.85, total);
    await gate.sleep(4);
  }
  log(`Записано ${total} Б`);

  // Verify: read every page back and compare. Cheap for 32 KB, and it catches
  // the classic "wrote nothing because the bootloader was busy" failure.
  log('Проверка записи…');
  let verified = true;
  for (let offset = 0; offset < total; offset += board.pageSize) {
    await stk.loadAddress(offset >> 1);
    const page = await stk.readPage(board.pageSize);
    const expectedPage = padded.slice(offset, offset + board.pageSize);
    for (let i = 0; i < board.pageSize; i++) {
      if (page[i] !== expectedPage[i]) {
        verified = false;
        throw new SerialError(
          `Ошибка проверки: страница по адресу 0x${(offset + i).toString(16)} содержит 0x${page[i]
            .toString(16)
            .padStart(2, '0')}, ожидалось 0x${expectedPage[i].toString(16).padStart(2, '0')}`,
        );
      }
    }
    options.onProgress?.(total * 0.85 + ((offset + board.pageSize) / total) * total * 0.15, total);
  }

  await stk.leaveProgrammingMode();
  log('Плата перезапущена с новым скетчем');

  return {
    signature: signatureHex,
    bytes: padded.length,
    pageSize: board.pageSize,
    verified,
    ms: performance.now() - started,
  };
}
