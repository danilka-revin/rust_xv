#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>

// Встроенный кириллический шрифт 5x7 (UTF-8 без необходимости патчить glcdfont.c!)
// Индексы 0..63 -> U+0410..U+044F (А..Я, а..я), 64 -> Ё (U+0401), 65 -> ё (U+0451)
static const uint8_t PROGMEM CYRILLIC_5X7[66][5] = {
  { 0x7E, 0x09, 0x09, 0x09, 0x7E }, //  0: А
  { 0x7F, 0x49, 0x49, 0x49, 0x31 }, //  1: Б
  { 0x7F, 0x49, 0x49, 0x49, 0x36 }, //  2: В
  { 0x7F, 0x01, 0x01, 0x01, 0x01 }, //  3: Г
  { 0x60, 0x3E, 0x21, 0x21, 0x7E }, //  4: Д
  { 0x7F, 0x49, 0x49, 0x49, 0x41 }, //  5: Е
  { 0x63, 0x14, 0x7F, 0x14, 0x63 }, //  6: Ж
  { 0x22, 0x41, 0x49, 0x49, 0x36 }, //  7: З
  { 0x7F, 0x10, 0x08, 0x04, 0x7F }, //  8: И
  { 0x7E, 0x10, 0x09, 0x04, 0x7E }, //  9: Й
  { 0x7F, 0x08, 0x14, 0x22, 0x41 }, // 10: К
  { 0x40, 0x3E, 0x01, 0x01, 0x7F }, // 11: Л
  { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, // 12: М
  { 0x7F, 0x08, 0x08, 0x08, 0x7F }, // 13: Н
  { 0x3E, 0x41, 0x41, 0x41, 0x3E }, // 14: О
  { 0x7F, 0x01, 0x01, 0x01, 0x7F }, // 15: П
  { 0x7F, 0x09, 0x09, 0x09, 0x06 }, // 16: Р
  { 0x3E, 0x41, 0x41, 0x41, 0x22 }, // 17: С
  { 0x01, 0x01, 0x7F, 0x01, 0x01 }, // 18: Т
  { 0x27, 0x48, 0x48, 0x48, 0x3F }, // 19: У
  { 0x0E, 0x11, 0x7F, 0x11, 0x0E }, // 20: Ф
  { 0x63, 0x14, 0x08, 0x14, 0x63 }, // 21: Х
  { 0x3F, 0x20, 0x20, 0x3F, 0x60 }, // 22: Ц
  { 0x07, 0x08, 0x08, 0x08, 0x7F }, // 23: Ч
  { 0x7F, 0x40, 0x7F, 0x40, 0x7F }, // 24: Ш
  { 0x3F, 0x20, 0x3F, 0x20, 0x7F }, // 25: Щ
  { 0x01, 0x7F, 0x48, 0x48, 0x30 }, // 26: Ъ
  { 0x7F, 0x48, 0x78, 0x00, 0x7F }, // 27: Ы
  { 0x7F, 0x48, 0x48, 0x48, 0x30 }, // 28: Ь
  { 0x22, 0x41, 0x49, 0x49, 0x3E }, // 29: Э
  { 0x7F, 0x08, 0x3E, 0x41, 0x3E }, // 30: Ю
  { 0x46, 0x29, 0x19, 0x09, 0x7F }, // 31: Я
  { 0x20, 0x54, 0x54, 0x54, 0x78 }, // 32: а
  { 0x30, 0x4C, 0x4A, 0x4A, 0x3A }, // 33: б
  { 0x7C, 0x54, 0x54, 0x54, 0x28 }, // 34: в
  { 0x7C, 0x04, 0x04, 0x04, 0x08 }, // 35: г
  { 0x60, 0x38, 0x24, 0x24, 0x78 }, // 36: д
  { 0x38, 0x54, 0x54, 0x54, 0x18 }, // 37: е
  { 0x44, 0x28, 0x7C, 0x28, 0x44 }, // 38: ж
  { 0x00, 0x44, 0x54, 0x54, 0x28 }, // 39: з
  { 0x7C, 0x20, 0x10, 0x08, 0x7C }, // 40: и
  { 0x7C, 0x20, 0x11, 0x08, 0x7C }, // 41: й
  { 0x7C, 0x10, 0x28, 0x44, 0x00 }, // 42: к
  { 0x40, 0x38, 0x04, 0x04, 0x7C }, // 43: л
  { 0x7C, 0x08, 0x10, 0x08, 0x7C }, // 44: м
  { 0x7C, 0x10, 0x10, 0x10, 0x7C }, // 45: н
  { 0x38, 0x44, 0x44, 0x44, 0x38 }, // 46: о
  { 0x7C, 0x04, 0x04, 0x04, 0x7C }, // 47: п
  { 0x7C, 0x14, 0x14, 0x14, 0x08 }, // 48: р
  { 0x38, 0x44, 0x44, 0x44, 0x28 }, // 49: с
  { 0x04, 0x04, 0x7C, 0x04, 0x04 }, // 50: т
  { 0x0C, 0x50, 0x50, 0x50, 0x3C }, // 51: у
  { 0x08, 0x14, 0x7E, 0x14, 0x08 }, // 52: ф
  { 0x44, 0x28, 0x10, 0x28, 0x44 }, // 53: х
  { 0x3C, 0x20, 0x20, 0x3C, 0x60 }, // 54: ц
  { 0x0C, 0x10, 0x10, 0x10, 0x7C }, // 55: ч
  { 0x7C, 0x40, 0x7C, 0x40, 0x7C }, // 56: ш
  { 0x3C, 0x20, 0x3C, 0x20, 0x7C }, // 57: щ
  { 0x04, 0x7C, 0x50, 0x50, 0x20 }, // 58: ъ
  { 0x7C, 0x50, 0x70, 0x00, 0x7C }, // 59: ы
  { 0x7C, 0x50, 0x50, 0x50, 0x20 }, // 60: ь
  { 0x00, 0x44, 0x54, 0x54, 0x38 }, // 61: э
  { 0x7C, 0x10, 0x38, 0x44, 0x38 }, // 62: ю
  { 0x48, 0x34, 0x14, 0x14, 0x7C }, // 63: я
  { 0x7C, 0x55, 0x54, 0x55, 0x44 }, // 64: Ё
  { 0x38, 0x55, 0x54, 0x55, 0x18 }, // 65: ё
};

// Общий компактный рендерер 5x7 глифа (экономит Flash)
static inline void drawBitmap5x7(Adafruit_GFX &gfx, int16_t x, int16_t y, const uint8_t cols[5], bool fromProgmem, uint16_t color, uint16_t bg, uint8_t size) {
  gfx.startWrite();
  for (int8_t col = 0; col < 5; col++) {
    uint8_t line = fromProgmem ? pgm_read_byte(&cols[col]) : cols[col];
    for (int8_t row = 0; row < 8; row++, line >>= 1) {
      if (line & 1) {
        if (size == 1) gfx.writePixel(x + col, y + row, color);
        else gfx.writeFillRect(x + col * size, y + row * size, size, size, color);
      } else if (bg != color) {
        if (size == 1) gfx.writePixel(x + col, y + row, bg);
        else gfx.writeFillRect(x + col * size, y + row * size, size, size, bg);
      }
    }
  }
  if (bg != color) {
    if (size == 1) gfx.writeFastVLine(x + 5, y, 8, bg);
    else gfx.writeFillRect(x + 5 * size, y, size, 8 * size, bg);
  }
  gfx.endWrite();
}

// Рисует один кириллический глиф 5x7 в масштабе size
inline void drawCyrillicGlyph(Adafruit_GFX &gfx, int16_t x, int16_t y, int glyphIdx, uint16_t color, uint16_t bg, uint8_t size) {
  if (glyphIdx < 0 || glyphIdx >= 66) return;
  drawBitmap5x7(gfx, x, y, CYRILLIC_5X7[glyphIdx], true, color, bg, size);
}

// Подсчёт видимой длины UTF-8 строки в символах (с игнорированием селектора U+FE0F у эмодзи ❤️)
inline uint16_t utf8CharCount(const char *str) {
  uint16_t count = 0;
  while (*str) {
    uint8_t c = (uint8_t)*str;
    if (c == 0xEF && (uint8_t)str[1] == 0xB8 && (uint8_t)str[2] == 0x8F) {
      str += 3; // U+FE0F variation selector (нулевая ширина)
      continue;
    }
    if ((c & 0xC0) != 0x80) count++;
    str++;
  }
  return count;
}

// Рисует аккуратное пиксельное сердечко 5x7
inline void drawInlineHeartGlyph(Adafruit_GFX &gfx, int16_t x, int16_t y, uint16_t heartCol, uint16_t bg, uint8_t size) {
  static const uint8_t HEART_COLS[5] = { 0x0C, 0x1E, 0x3C, 0x1E, 0x0C };
  drawBitmap5x7(gfx, x, y, HEART_COLS, false, heartCol, bg, size);
}

// Вывод UTF-8 строки (русский + английский + цифры + • + — + ❤️) в координаты (x, y)
static __attribute__((noinline)) void drawTextUTF8(Adafruit_GFX &gfx, int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size) {
  int16_t curX = x;
  const int16_t step = 6 * size;
  const int16_t maxW = gfx.width();
  while (*str) {
    uint8_t b1 = (uint8_t)*str++;
    bool visible = (curX + step > 0 && curX < maxW);
    if (b1 < 0x80) {
      if (visible) gfx.drawChar(curX, y, (unsigned char)b1, color, bg, size);
      curX += step;
    } else if (b1 == 0xC2 && (uint8_t)str[0] == 0xB0) {
      // Знак градуса '°' (U+00B0)
      str += 1;
      if (visible) {
        if (bg != color) gfx.fillRect(curX, y, step, 8 * size, bg);
        gfx.drawCircle(curX + 2 * size, y + size, size, color);
      }
      curX += step;
    } else if ((b1 == 0xD0 || b1 == 0xD1) && *str) {
      uint8_t b2 = (uint8_t)*str++;
      if (visible) {
        uint16_t cp = ((uint16_t)(b1 & 0x1F) << 6) | (b2 & 0x3F);
        int idx = -1;
        if (cp >= 0x0410 && cp <= 0x044F) idx = cp - 0x0410;
        else if (cp == 0x0401) idx = 64; // Ё
        else if (cp == 0x0451) idx = 65; // ё
        if (idx >= 0) drawCyrillicGlyph(gfx, curX, y, idx, color, bg, size);
        else gfx.drawChar(curX, y, '?', color, bg, size);
      }
      curX += step;
    } else if (b1 == 0xEF && (uint8_t)str[0] == 0xB8 && (uint8_t)str[1] == 0x8F) {
      // Пропускаем U+FE0F (селектор эмодзи после ❤)
      str += 2;
    } else if (b1 == 0xE2 && (uint8_t)str[0] == 0x80 && (uint8_t)str[1] == 0xA2) {
      // Буллит '•'
      str += 2;
      if (visible) {
        if (bg != color) gfx.fillRect(curX, y, step, 8 * size, bg);
        int16_t r = (size >= 2) ? size : 1;
        gfx.fillCircle(curX + (5 * size) / 2, y + (7 * size) / 2, r, color);
      }
      curX += step;
    } else if (b1 == 0xE2 && (uint8_t)str[0] == 0x80 && ((uint8_t)str[1] == 0x94 || (uint8_t)str[1] == 0x93)) {
      // Длинное/среднее тире '—' / '–'
      str += 2;
      if (visible) gfx.drawChar(curX, y, '-', color, bg, size);
      curX += step;
    } else if (b1 == 0xE2 && (((uint8_t)str[0] == 0x9D && (uint8_t)str[1] == 0xA4) ||
                              ((uint8_t)str[0] == 0x99 && (uint8_t)str[1] == 0xA5))) {
      // Символы сердечка '❤' и '♥'
      str += 2;
      if (visible) {
        uint16_t hc = (bg == color) ? 0xFA4E : 0xFA4E;
        drawInlineHeartGlyph(gfx, curX, y, hc, (bg == color) ? hc : bg, size);
      }
      curX += step;
    } else {
      // Прочие многобайтовые эмодзи (например 💕 4 байта 0xF0 ...) тоже рисуем милым сердечком!
      while ((*str & 0xC0) == 0x80) str++;
      if (b1 >= 0xF0) {
        if (visible) drawInlineHeartGlyph(gfx, curX, y, 0xFA4E, (bg == color) ? 0xFA4E : bg, size);
        curX += step;
      }
    }
  }
}

// Идеально центрированный вывод UTF-8 строки по ширине целевого экрана или GFXcanvas16
static __attribute__((noinline)) void drawCenteredUTF8(Adafruit_GFX &gfx, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size, int16_t targetWidth = -1) {
  int16_t w = (targetWidth > 0) ? targetWidth : gfx.width();
  uint16_t len = utf8CharCount(str);
  int16_t totalWidth = (len > 0) ? (len * 6 * size - size) : 0;
  int16_t x = (w - totalWidth) / 2;
  drawTextUTF8(gfx, x, y, str, color, bg, size);
}
