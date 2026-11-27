#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <Adafruit_GC9A01A.h>

// ============================================================================
// ФОРМАТ ПОЛНОЭКРАННЫХ ОТКРЫТОК ДЛЯ КРУГЛОГО ЭКРАНА 240x240: LoveCard v1 (.lvc)
// ============================================================================
// Структура файла (84+ байт, без внешних библиотек, нулевая нагрузка на RAM):
//   [0..3]   Магическая сигнатура "LVC1"
//   [4..5]   Ширина (240, uint16 LE)
//   [6..7]   Высота (240, uint16 LE)
//   [8..9]   Длина подписи в байтах capLen (uint16 LE, 0..255)
//   [10..69] uint32_t stripLens[15] (15 полос по 16 строк = 240x16 пикселей)
//   [70..]   UTF-8 подпись длиной capLen + 15 сжатых QOI565-полос подряд
// ============================================================================

static const int LVC_WIDTH       = 240;
static const int LVC_HEIGHT      = 240;
static const int LVC_STRIP_ROWS  = 16;
static const int LVC_STRIP_COUNT = 15; // 15 * 16 = 240
static const int LVC_HEADER_BASE = 70;

struct LvcMeta {
  bool valid;
  uint16_t width;
  uint16_t height;
  uint16_t capLen;
  uint32_t stripLens[LVC_STRIP_COUNT];
  uint32_t dataOffset;
  char caption[256];
};

static inline uint16_t lvcReadU16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t lvcReadU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline bool lvcParseMetaFromBuffer(const uint8_t* buf, size_t len, LvcMeta& meta) {
  meta.valid = false;
  meta.caption[0] = '\0';
  if (!buf || len < LVC_HEADER_BASE) return false;
  if (buf[0] != 'L' || buf[1] != 'V' || buf[2] != 'C' || buf[3] != '1') return false;

  meta.width  = lvcReadU16(buf + 4);
  meta.height = lvcReadU16(buf + 6);
  meta.capLen = lvcReadU16(buf + 8);
  if (meta.width != LVC_WIDTH || meta.height != LVC_HEIGHT || meta.capLen > 255) return false;

  for (int i = 0; i < LVC_STRIP_COUNT; i++) {
    meta.stripLens[i] = lvcReadU32(buf + 10 + i * 4);
    if (meta.stripLens[i] == 0 || meta.stripLens[i] > 24576) return false;
  }

  meta.dataOffset = LVC_HEADER_BASE + meta.capLen;
  if (len >= meta.dataOffset && meta.capLen > 0) {
    memcpy(meta.caption, buf + LVC_HEADER_BASE, meta.capLen);
    meta.caption[meta.capLen] = '\0';
  }
  meta.valid = true;
  return true;
}

inline bool lvcReadMetaFromFile(const char* path, LvcMeta& meta) {
  meta.valid = false;
  meta.caption[0] = '\0';
  if (!path || !LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint8_t hdr[LVC_HEADER_BASE];
  if (f.read(hdr, LVC_HEADER_BASE) != LVC_HEADER_BASE) {
    f.close();
    return false;
  }
  if (!lvcParseMetaFromBuffer(hdr, LVC_HEADER_BASE, meta)) {
    f.close();
    return false;
  }

  if (meta.capLen > 0) {
    if (f.read((uint8_t*)meta.caption, meta.capLen) != meta.capLen) {
      f.close();
      meta.valid = false;
      return false;
    }
    meta.caption[meta.capLen] = '\0';
  }
  f.close();
  return true;
}

// Декодер одной полосы QOI565 (240*16 = 3840 пикселей)
inline void decodeQoi565Strip(const uint8_t* src, size_t srcLen, uint16_t* dst, size_t numPixels) {
  uint16_t index[64];
  memset(index, 0, sizeof(index));
  uint16_t prev = 0x0000;
  size_t p = 0;
  size_t outIdx = 0;

  while (outIdx < numPixels) {
    if (p >= srcLen) {
      dst[outIdx++] = prev;
      continue;
    }

    uint8_t b1 = src[p++];
    if (b1 == 0xFE) {
      if (p + 1 < srcLen) {
        prev = ((uint16_t)src[p] << 8) | (uint16_t)src[p + 1];
        p += 2;
      }
    } else if ((b1 & 0xC0) == 0x40) {
      int run = (b1 & 0x3F) + 1;
      for (int r = 0; r < run && outIdx < numPixels; r++) {
        dst[outIdx++] = prev;
      }
      continue;
    } else if ((b1 & 0xC0) == 0x00) {
      prev = index[b1 & 0x3F];
    } else if ((b1 & 0xC0) == 0x80) {
      int pr = (prev >> 11) & 0x1F;
      int pg = (prev >> 5)  & 0x3F;
      int pb =  prev        & 0x1F;
      int dr = ((b1 >> 4) & 0x03) - 2;
      int dg = ((b1 >> 2) & 0x03) - 2;
      int db = ( b1       & 0x03) - 2;
      int nr = (pr + dr) & 0x1F;
      int ng = (pg + dg) & 0x3F;
      int nb = (pb + db) & 0x1F;
      prev = (uint16_t)((nr << 11) | (ng << 5) | nb);
    } else {
      // LUMA (0xC0..0xDF)
      if (p < srcLen) {
        uint8_t b2 = src[p++];
        int pr = (prev >> 11) & 0x1F;
        int pg = (prev >> 5)  & 0x3F;
        int pb =  prev        & 0x1F;
        int dg = (b1 & 0x1F) - 16;
        int dg2 = (dg >= 0) ? (dg / 2) : -((-dg) / 2);
        int dr = dg2 + (((b2 >> 4) & 0x0F) - 8);
        int db = dg2 + (( b2       & 0x0F) - 8);
        int nr = (pr + dr) & 0x1F;
        int ng = (pg + dg) & 0x3F;
        int nb = (pb + db) & 0x1F;
        prev = (uint16_t)((nr << 11) | (ng << 5) | nb);
      }
    }

    uint8_t r5 = (prev >> 11) & 0x1F;
    uint8_t g6 = (prev >> 5)  & 0x3F;
    uint8_t b5 =  prev        & 0x1F;
    index[(r5 * 3 + g6 * 5 + b5 * 7) & 63] = prev;
    dst[outIdx++] = prev;
  }
}

// Смещает сохранённые открытки 0..3 -> 1..4 и помещает /pending.lvc в /card0.lvc
inline void lvcPromotePendingCard(const char* pendingPath = "/pending.lvc") {
  if (!LittleFS.exists(pendingPath)) return;
  if (LittleFS.exists("/card4.lvc")) LittleFS.remove("/card4.lvc");
  for (int i = 3; i >= 0; i--) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "/card%d.lvc", i);
    snprintf(dst, sizeof(dst), "/card%d.lvc", i + 1);
    if (LittleFS.exists(src)) {
      LittleFS.rename(src, dst);
    }
  }
  LittleFS.rename(pendingPath, "/card0.lvc");
}

// Если пришло обычное текстовое сообщение без картинки, сдвигаем слоты файлов открыток
inline void lvcShiftEmptySlot0() {
  if (LittleFS.exists("/card4.lvc")) LittleFS.remove("/card4.lvc");
  for (int i = 3; i >= 0; i--) {
    char src[16], dst[16];
    snprintf(src, sizeof(src), "/card%d.lvc", i);
    snprintf(dst, sizeof(dst), "/card%d.lvc", i + 1);
    if (LittleFS.exists(src)) {
      LittleFS.rename(src, dst);
    }
  }
}

inline void lvcDrawStripFromFile(File& f, const LvcMeta& meta, int stripIdx, uint16_t* stripBuf,
                                 uint8_t* compBuf, size_t compBufSize,
                                 uint16_t ringColor, Adafruit_GC9A01A& tft) {
  if (stripIdx < 0 || stripIdx >= LVC_STRIP_COUNT) return;

  uint32_t offset = meta.dataOffset;
  for (int i = 0; i < stripIdx; i++) offset += meta.stripLens[i];

  uint32_t len = meta.stripLens[stripIdx];
  if (len == 0 || len > compBufSize) return;

  f.seek(offset);
  size_t rd = f.read(compBuf, len);
  if (rd == len) {
    decodeQoi565Strip(compBuf, len, stripBuf, LVC_WIDTH * LVC_STRIP_ROWS);

    int yBase = stripIdx * LVC_STRIP_ROWS;
    for (int r = 0; r < LVC_STRIP_ROWS; r++) {
      int y = yBase + r;
      int dy = y - 120;
      int dy2 = dy * dy;
      for (int x = 0; x < LVC_WIDTH; x++) {
        int dx = x - 120;
        int r2 = dx * dx + dy2;
        if (r2 > 14042) {
          stripBuf[r * LVC_WIDTH + x] = 0x0000;
        } else if (r2 >= 13572) {
          stripBuf[r * LVC_WIDTH + x] = ringColor;
        }
      }
    }
    tft.drawRGBBitmap(0, yBase, stripBuf, LVC_WIDTH, LVC_STRIP_ROWS);
  }
}
