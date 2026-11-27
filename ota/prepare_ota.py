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
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_JSON = os.path.join(ROOT, "ota", "version.json")
TARGET_BIN = os.path.join(ROOT, "ota", "firmware.bin")
MAX_APP_BYTES = 1310720  # Default 4MB with spiffs (1.2MB APP)


def find_compiled_bin():
    patterns = [
        os.path.join(ROOT, "love_gift_esp32c6", "build", "**", "*.ino.bin"),
        os.path.join(ROOT, "build", "**", "*.ino.bin"),
        os.path.join(tempfile.gettempdir(), "arduino*", "*love_gift_esp32c6*.bin"),
        os.path.join(tempfile.gettempdir(), "arduino*", "**", "*love_gift_esp32c6*.bin"),
    ]
    candidates = []
    for pat in patterns:
        candidates.extend(glob.glob(pat, recursive=True))
    candidates = [c for c in candidates if os.path.isfile(c) and not c.endswith(".merged.bin") and "bootloader" not in c and "partitions" not in c]
    if not candidates:
        return None
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def main():
    if len(sys.argv) < 2:
        print("Использование: python3 ota/prepare_ota.py <новая_версия> [путь_к_ino.bin]")
        print("Пример:        python3 ota/prepare_ota.py 1.3.8")
        sys.exit(1)

    new_ver = sys.argv[1].strip().lstrip("v")

    bin_path = sys.argv[2] if len(sys.argv) >= 3 else find_compiled_bin()

    if bin_path:
        if not os.path.isfile(bin_path):
            print(f"Файл не найден: {bin_path}")
            sys.exit(1)
        size_b = os.path.getsize(bin_path)
        if size_b > MAX_APP_BYTES:
            print(f"ОШИБКА: Бинарник слишком большой ({size_b} > {MAX_APP_BYTES} байт для 1.2MB APP)!")
            sys.exit(1)
        with open(bin_path, "rb") as f:
            magic = f.read(1)
        if magic != b"\xe9":
            print("ОШИБКА: Это не образ приложения ESP32 (первый байт != 0xE9).")
            print("Используйте файл *.ino.bin (НЕ *.merged.bin)!")
            sys.exit(1)
        shutil.copy2(bin_path, TARGET_BIN)
        size_kb = os.path.getsize(TARGET_BIN) / 1024.0
        print(f"✓ Скопирован бинарник: {bin_path} -> ota/firmware.bin ({size_kb:.1f} КБ / {size_b} байт)")
    else:
        print("! Файл *.ino.bin не найден автоматически.")
        print("  В Arduino IDE нажмите Sketch -> Export Compiled Binary и передайте путь аргументом:")
        print(f"  python3 ota/prepare_ota.py {new_ver} path/to/love_gift_esp32c6.ino.bin")
        sys.exit(1)

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
