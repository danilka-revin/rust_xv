#!/usr/bin/env python3
"""
Помощник для быстрой подготовки OTA-обновления на GitHub.
Станция проверяет обновления по последнему коммиту в ветке main и сравнивает SHA файла ota/firmware.bin,
поэтому менять цифру версии больше не обязательно.

Пример запуска после "Sketch -> Export Compiled Binary" в Arduino IDE:
  python3 ota/prepare_ota.py
  python3 ota/prepare_ota.py path/to/love_gift_esp32c6.ino.bin
"""

import glob
import json
import os
import shutil
import subprocess
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
    arg_path = sys.argv[1] if len(sys.argv) >= 2 and sys.argv[1].endswith(".bin") else None
    new_ver = None
    if len(sys.argv) >= 2 and not sys.argv[1].endswith(".bin"):
        new_ver = sys.argv[1].strip().lstrip("v")
        arg_path = sys.argv[2] if len(sys.argv) >= 3 else None

    bin_path = arg_path or find_compiled_bin()

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
        print("  В Arduino IDE нажмите Sketch -> Export Compiled Binary и запустите:")
        print("  python3 ota/prepare_ota.py path/to/love_gift_esp32c6.ino.bin")
        sys.exit(1)

    if os.path.isfile(VERSION_JSON):
        with open(VERSION_JSON, "r", encoding="utf-8") as f:
            data = json.load(f)
    else:
        data = {"version": "1.3.8", "bin_url": "https://raw.githubusercontent.com/danilka-revin/rust_xv/main/ota/firmware.bin"}

    if new_ver:
        data["version"] = new_ver
    try:
        head_sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
        if head_sha:
            data["commit"] = head_sha
    except Exception:
        pass

    with open(VERSION_JSON, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print("✓ Готово! Станция обнаружит изменение ota/firmware.bin при новом коммите в main.")
    print("Теперь сделайте: git add ota/ && git commit -m 'Update firmware binary' && git push origin main")


if __name__ == "__main__":
    main()
