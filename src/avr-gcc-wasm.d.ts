declare module '@horang-corp/avr-gcc-wasm/firmware-builder' {
  export interface BuildFirmwareOptions {
    source: string;
    sensors?: string[];
    assetsBase?: string | URL;
  }

  export interface BuildFirmwareResult {
    hex: string;
    sensors: string[];
    target: { mcu: string; board: string; appFlashBytes: number };
    bytes: number;
    hexBytes: number;
    flashBytes: number;
    fitsTarget: boolean;
    objectCount: number;
    memory: { linearPeakBytesByTool: Record<string, number>; linearPeakBytesTotalSequential: number };
    timings: Record<string, number>;
    stderr: string[];
  }

  export const SENSOR: { readonly OLED: string; readonly TOF: string };

  export function buildFirmware(options: BuildFirmwareOptions): Promise<BuildFirmwareResult>;
  export function setAssetsBase(base: string | URL): void;
}
