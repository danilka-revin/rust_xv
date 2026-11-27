#!/usr/bin/env python3
"""
Помощник для быстрой подготовки OTA-обновления на GitHub.
Пример запуска после "Sketch -> Export Compiled Binary" в Arduino IDE:
  python3 ota/prepare_ota.py 1.0.1
"""

import glob
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_JSON = os.path.join(ROOT, "ota", "version.json")
TARGET_BIN = os.path.join(ROOT, "ota", "firmware.bin")


def main():
    if len(sys.argv) < 2:
        print("Использование: python3 ota/prepare_ota.py <новая_версия> [путь_к_ino.bin]")
        print("Пример:        python3 ota/prepare_ota.py 1.0.1")
        sys.exit(1)

    new_ver = sys.argv[1].strip().lstrip("v")

    bin_path = None
    if len(sys.argv) >= 3:
        bin_path = sys.argv[2]
    else:
        candidates = glob.glob(
            os.path.join(ROOT, "love_gift_esp32c6", "build", "**", "*.ino.bin"),
            recursive=True,
        )
        candidates = [c for c in candidates if not c.endswith(".merged.bin")]
        if candidates:
            candidates.sort(key=os.path.getmtime, reverse=True)
            bin_path = candidates[0]

    if bin_path:
        if not os.path.isfile(bin_path):
            print(f"Файл не найден: {bin_path}")
            sys.exit(1)
        with open(bin_path, "rb") as f:
            magic = f.read(1)
        if magic != b"\xe9":
            print("ОШИБКА: Это не образ приложения ESP32 (первый байт != 0xE9).")
            print("Используйте файл *.ino.bin (НЕ *.merged.bin)!")
            sys.exit(1)
        shutil.copy2(bin_path, TARGET_BIN)
        size_kb = os.path.getsize(TARGET_BIN) / 1024.0
        print(f"✓ Скопирован бинарник: {bin_path} -> ota/firmware.bin ({size_kb:.1f} КБ)")
    else:
        print("! Файл *.ino.bin в love_gift_esp32c6/build не найден.")
        print("  В Arduino IDE нажмите Sketch -> Export Compiled Binary и скопируйте *.ino.bin в ota/firmware.bin.")

    with open(VERSION_JSON, "r", encoding="utf-8") as f:
        data = json.load(f)
    data["version"] = new_ver
    with open(VERSION_JSON, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(f"✓ Обновлён ota/version.json -> version: {new_ver}")
    print("Теперь сделайте git add ota/ && git commit -m 'Update firmware' && git push!")


if __name__ == "__main__":
    main()
