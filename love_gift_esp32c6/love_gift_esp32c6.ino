/*
 * Станция отношений — Маленький хранитель нашей любви
 * Плата:   Seeed Studio XIAO ESP32-C6
 * Экран:   Круглый TFT GC9A01A 240x240 (SPI 40 МГц)
 * Кнопки:  D0 (GPIO 0) и D1 (GPIO 1), активный уровень HIGH (с антидребезгом)
 * Питание: Аккумулятор 18650
 *
 * Что умеет (v1.2.0):
 * 1. Запоминает пароль домашнего Wi-Fi в энергонезависимой Flash-памяти (NVS) —
 *    не сбрасывается при перезапуске, разряде батареи и автообновлениях.
 * 2. Плавные анимации переключения 3 страниц на круглом экране по Кнопке 2 (D1):
 *    - Страница 1: Таймер отношений с 21.10.2026 + переливающееся сердечко по кругу + бегущая строка.
 *    - Страница 2: Полноэкранные открытки-послания из интернета (фото, разные шрифты, эмодзи, текст)
 *                  сохраняются в LittleFS, с плавным раскрытием от центра экрана!
 *    - Страница 3: Большое 3D-сердечко + адрес локального сайта и версия прошивки.
 * 3. Приём открыток и сообщений через бесплатный облачный канал ntfy.sh
 *    + ответное сердечко тебе на телефон по Кнопке 1 (D0)!
 * 4. Автообновление прошивки с GitHub по воздуху (HTTPS OTA).
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "app_types.h"
#include "cyrillic_font.h"
#include "love_card.h"
#include "web_ui.h"

// --- ВЕРСИЯ ПРОШИВКИ И GITHUB АВТООБНОВЛЕНИЕ (OTA) ---
#define FIRMWARE_VERSION "1.3.5"

static const char* OTA_VERSION_URL =
  "https://raw.githubusercontent.com/danilka-revin/rust_xv/main/ota/version.json";

static const unsigned long OTA_FIRST_CHECK_DELAY_MS = 45000UL;
static const unsigned long OTA_CHECK_INTERVAL_MS    = 1800000UL; // 30 минут

// --- СЕКРЕТНЫЙ ОБЛАЧНЫЙ КАНАЛ СООБЩЕНИЙ NTFY.SH (РАБОТАЕТ ЧЕРЕЗ ИНТЕРНЕТ ИЗДАЛЕКА) ---
// Должен совпадать с ключом в вашем пульте sender/index.html!
static const char* NTFY_TOPIC = "love_station_xv_7d92a4c8e1";
static const unsigned long NTFY_POLL_INTERVAL_MS = 10000UL; // Проверка новых посланий каждые 10 сек

// --- ПИНЫ XIAO ESP32-C6 (ваша проверенная рабочая схема) ---
#define TFT_CS    21  // пин D3
#define TFT_DC    22  // пин D4
#define TFT_RST   23  // пин D5

#define BTN_1     0   // пин D0 (активный HIGH)
#define BTN_2     1   // пин D1 (активный HIGH)

// --- КОНСТАНТЫ ---
// 21 октября 2026 00:00:00 UTC = 1792540800
static const uint32_t REL_START_UTC_MIDNIGHT = 1792540800UL;
static const char* AP_SSID = "love";

static const char* DEFAULT_WIFI_SSID = "";
static const char* DEFAULT_WIFI_PASS = "";

static const uint8_t DNS_PORT = 53;
static const unsigned long BTN_PRESS_DEBOUNCE_MS   = 16;
static const unsigned long BTN_RELEASE_DEBOUNCE_MS = 70;
static const unsigned long PAGE_SWITCH_COOLDOWN_MS = 320;

// --- ЦВЕТА И ПЕРЕЛИВЫ RGB565 ---
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

static inline uint16_t dim565(uint16_t c, uint8_t alpha) {
  if (alpha >= 250) return c;
  if (alpha == 0) return 0x0000;
  uint32_t r = ((c >> 11) & 0x1F) * alpha / 255;
  uint32_t g = ((c >> 5)  & 0x3F) * alpha / 255;
  uint32_t b = ( c        & 0x1F) * alpha / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void evalShimmerRGB(float phase, uint8_t &outR, uint8_t &outG, uint8_t &outB) {
  static const uint8_t PALETTE[5][3] = {
    { 255, 120, 175 },
    { 255,  42, 102 },
    { 255,  92,  76 },
    { 255, 188, 118 },
    { 218,  96, 255 }
  };
  float p = phase - floorf(phase);
  float scaled = p * 5.0f;
  int idx0 = ((int)scaled) % 5;
  int idx1 = (idx0 + 1) % 5;
  float frac = scaled - floorf(scaled);
  float t = 0.5f - 0.5f * cosf(frac * PI);

  outR = (uint8_t)(PALETTE[idx0][0] + (PALETTE[idx1][0] - PALETTE[idx0][0]) * t);
  outG = (uint8_t)(PALETTE[idx0][1] + (PALETTE[idx1][1] - PALETTE[idx0][1]) * t);
  outB = (uint8_t)(PALETTE[idx0][2] + (PALETTE[idx1][2] - PALETTE[idx0][2]) * t);
}

static const uint16_t COL_BG         = 0x0000;
static const uint16_t COL_CARD_BG    = 0x1863;
static const uint16_t COL_RING_DIM   = 0x30A6;
static const uint16_t COL_PINK_SOFT  = 0xFDB8;
static const uint16_t COL_PINK_HOT   = 0xFA4E;
static const uint16_t COL_GOLD_SOFT  = 0xFE73;
static const uint16_t COL_MINT_OK    = 0x57EA;
static const uint16_t COL_MUTED      = 0xAD55;

// --- ЭКРАН И ОФФСКРИН-БУФЕРЫ (0% МЕРЦАНИЙ) ---
Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

GFXcanvas16 heartCanvas(112, 100);     // Большое 3D-сердечко / буфер распаковки полос (22.4 КБ)
GFXcanvas16 msgCanvas(172, 86);        // Поле текста на Стр.2 / блок дней / полоса 240x16 (29.6 КБ)
GFXcanvas16 pillCanvas(168, 30);       // Капсула ЧЧ:ММ:СС / прогресс OTA (10.1 КБ)
GFXcanvas16 tickerCanvas(152, 16);     // Бегущая строка / подсказка (4.9 КБ)
GFXcanvas16 miniHeartCanvas(28, 24);   // Верхнее мини-сердечко (1.3 КБ)
GFXcanvas16 orbitCanvas(18, 18);       // Бегущее переливающееся сердечко (0.6 КБ)

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// --- СТРАНИЦЫ КРУГЛОГО ЭКРАНА (определены в app_types.h) ---
ScreenMode currentScreen = SCREEN_AP_SETUP;
ScreenMode returnScreenAfterPulse = SCREEN_TIMER;
bool screenNeedsFullRedraw = true;

// --- ПЛАВНЫЕ АНИМАЦИИ ПЕРЕКЛЮЧЕНИЯ СТРАНИЦ ---
bool transActive = false;
int transPhase = 0;            // 0 = закрытие сияющего кольца, 1 = раскрытие новой страницы
ScreenMode transTarget = SCREEN_TIMER;
unsigned long transStartMs = 0;
int transPrevR = 118;
int transStripStep = -1;       // 0..7 для раскрытия 15 полос открытки от центра вверх и вниз
bool activeCardRendered = false;

// --- СОСТОЯНИЕ СЕТИ И ВРЕМЕНИ ---
String savedSsid = "";
String savedPass = "";
int tzOffsetHours = 3;
bool wifiConnected = false;
bool apActive = false;
bool pendingDisableAp = false;
unsigned long disableApAtMs = 0;
bool bannerActive = false;
unsigned long bannerUntilMs = 0;
unsigned long lastWifiRetryMs = 0;
bool timeSynced = false;
bool fsReady = false;

// --- ХРАНИЛИЩЕ ПОСЛЕДНИХ 5 ПОСЛАНИЙ / ОТКРЫТОК ---
static const int MAX_MSG_HISTORY = 5;

StoredMessage msgHistory[MAX_MSG_HISTORY];
int msgHistoryCount = 0;
char lastNtfyId[32] = "";
volatile bool unreadMessage = false;

volatile bool incomingMsgPendingApply = false;
char pendingMsgText[320] = "";
uint32_t pendingMsgEpoch = 0;
char pendingMsgId[32] = "";
bool pendingMsgHasCard = false;

// Вертикальный авто-скролл текстовых сообщений (если пришло без картинки)
static const int MAX_WRAPPED_LINES = 24;
char wrappedLines[MAX_WRAPPED_LINES][80];
int wrappedLineCount = 0;
uint8_t wrappedScale = 2;
bool msgNeedsRewrap = true;
float msgScrollY = 0.0f;
float msgScrollPauseSec = 1.8f;
int msgScrollDir = 1;

// Фоновые задачи облака (ntfy + GitHub OTA)
volatile bool ntfyPollRunning = false;
volatile bool ntfyReplyRunning = false;
unsigned long nextNtfyPollMs = 5000UL;
unsigned long lastReplySentMs = 0;

volatile bool otaTaskRunning = false;
volatile bool otaUpdateReady = false;
volatile bool otaForceRequested = false;
char otaPendingVersion[24] = "";
char otaPendingBinUrl[256] = "";
unsigned long nextOtaCheckMs = OTA_FIRST_CHECK_DELAY_MS;

// --- ПОГОДА (OPEN-METEO, СТРАНИЦА 4) ---
static const unsigned long WEATHER_POLL_INTERVAL_MS = 900000UL; // Обновление каждые 15 минут
String weatherCity = "Москва";
float  weatherLat  = 55.7558f;
float  weatherLon  = 37.6173f;
volatile bool weatherValid = false;
volatile int  weatherTempC = 0;
volatile int  weatherFeelsC = 0;
volatile int  weatherWindMs = 0;
volatile int  weatherCode = 0;
volatile bool weatherPollRunning = false;
volatile bool weatherNeedsRedraw = false;
unsigned long nextWeatherPollMs = 6000UL;

// --- ТАЙМИНГИ КАДРОВ ---
unsigned long lastFrameMs = 0;
int prevSecondDrawn = -1;
uint64_t prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
bool prevSyncStateDrawn = true;
int16_t prevOrbitBx = -999;
int16_t prevOrbitBy = -999;

char tickerText[192] = "";
int tickerLoopPx = 1;
float tickerScrollPx = 0.0f;

unsigned long pulseStartMs = 0;
bool pulseSentReply = false;
int lovePhraseIndex = 0;
const char* LOVE_PHRASES[] = {
  "ЛЮБЛЮ ТЕБЯ",
  "ВСЕГДА РЯДОМ",
  "ТЫ ЛУЧШАЯ",
  "НАШЕ ВРЕМЯ",
  "КРЕПКО ОБНИМАЮ",
  "МОЯ РАДОСТЬ"
};
const int LOVE_PHRASES_COUNT = sizeof(LOVE_PHRASES) / sizeof(LOVE_PHRASES[0]);

// --- АНТИДРЕБЕЗГ КНОПОК ---
DebouncedButton btn1 = { BTN_1, false, false, 0 };
DebouncedButton btn2 = { BTN_2, false, false, 0 };

unsigned long btn2PressStartMs = 0;
bool btn2LongHandled = false;
int lastHoldSecShown = 0;
unsigned long lastPageSwitchMs = 0;
unsigned long lastBtn1ActionMs = 0;

bool updateButton(DebouncedButton &b, unsigned long nowMs, bool &justPressed, bool &justReleased) {
  justPressed = false;
  justReleased = false;
  bool r = (digitalRead(b.pin) == HIGH);
  if (r != b.rawState) {
    b.rawState = r;
    b.lastChangeMs = nowMs;
  }
  unsigned long reqMs = b.rawState ? BTN_PRESS_DEBOUNCE_MS : BTN_RELEASE_DEBOUNCE_MS;
  if ((nowMs - b.lastChangeMs) >= reqMs && b.stableState != b.rawState) {
    b.stableState = b.rawState;
    if (b.stableState) justPressed = true;
    else justReleased = true;
  }
  return b.stableState;
}

// ============================================================================
// УТИЛИТЫ ВРЕМЕНИ, ФОРМАТИРОВАНИЯ И ПЕРЕНОСА СЛОВ (UTF-8)
// ============================================================================

const char* pluralUpper(uint64_t n, const char* one, const char* two, const char* five) {
  uint64_t n100 = n % 100ULL;
  uint64_t n10  = n % 10ULL;
  if (n100 > 10ULL && n100 < 20ULL) return five;
  if (n10 == 1ULL) return one;
  if (n10 >= 2ULL && n10 <= 4ULL) return two;
  return five;
}

void formatU64Grouped(uint64_t val, char* out, size_t outSize, bool withSpaces = true) {
  char rev[32];
  int rLen = 0;
  if (val == 0) {
    rev[rLen++] = '0';
  } else {
    int group = 0;
    while (val > 0 && rLen < 30) {
      if (withSpaces && group == 3) {
        rev[rLen++] = ' ';
        group = 0;
      }
      rev[rLen++] = (char)('0' + (val % 10ULL));
      val /= 10ULL;
      group++;
    }
  }
  size_t pos = 0;
  for (int i = rLen - 1; i >= 0 && pos + 1 < outSize; i--) {
    out[pos++] = rev[i];
  }
  out[pos] = '\0';
}

void buildTickerString(uint64_t totalDiffSec) {
  uint64_t totalHours = totalDiffSec / 3600ULL;
  uint64_t totalMins  = totalDiffSec / 60ULL;

  char hBuf[32], mBuf[32], sBuf[32];
  formatU64Grouped(totalHours,   hBuf, sizeof(hBuf), true);
  formatU64Grouped(totalMins,    mBuf, sizeof(mBuf), true);
  formatU64Grouped(totalDiffSec, sBuf, sizeof(sBuf), true);

  const char* hWord = pluralUpper(totalHours,   "ЧАС",     "ЧАСА",    "ЧАСОВ");
  const char* mWord = pluralUpper(totalMins,    "МИНУТА",  "МИНУТЫ",  "МИНУТ");
  const char* sWord = pluralUpper(totalDiffSec, "СЕКУНДА", "СЕКУНДЫ", "СЕКУНД");

  snprintf(tickerText, sizeof(tickerText),
           "ВСЕГО: %s %s  •  %s %s  •  %s %s  •  ",
           hBuf, hWord, mBuf, mWord, sBuf, sWord);

  int chars = (int)utf8CharCount(tickerText);
  tickerLoopPx = (chars > 0) ? (chars * 6) : 1;
  if (tickerScrollPx >= (float)tickerLoopPx) {
    tickerScrollPx = fmodf(tickerScrollPx, (float)tickerLoopPx);
  }
}

void configureNtp() {
  configTime(tzOffsetHours * 3600L, 0, "pool.ntp.org", "time.google.com", "ru.pool.ntp.org");
}

bool getCurrentExactUtc(uint32_t &outSec, uint32_t &outUsec) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if ((uint32_t)tv.tv_sec > 1700000000UL) {
    timeSynced = true;
    outSec  = (uint32_t)tv.tv_sec;
    outUsec = (uint32_t)tv.tv_usec;
    return true;
  }
  outSec = 0;
  outUsec = 0;
  return false;
}

uint32_t getCurrentUtcEpoch() {
  uint32_t s, u;
  return getCurrentExactUtc(s, u) ? s : 0;
}

void setSystemEpoch(uint32_t epochSec) {
  if (epochSec < 1700000000UL) return;
  struct timeval tv;
  tv.tv_sec = epochSec;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeSynced = true;
  prefs.putUInt("lastEpoch", epochSec);
}

void formatMsgTimestamp(uint32_t utcEpoch, char* out, size_t outSize) {
  if (utcEpoch < 1700000000UL) {
    snprintf(out, outSize, "Сохранено в памяти");
    return;
  }
  time_t localT = (time_t)utcEpoch + (time_t)(tzOffsetHours * 3600L);
  struct tm tmv;
  gmtime_r(&localT, &tmv);
  snprintf(out, outSize, "Получено %02d.%02d в %02d:%02d",
           tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min);
}

size_t nextUtf8GraphemeBytes(const char* s) {
  if (!s || !*s) return 0;
  size_t n = 1;
  while ((s[n] & 0xC0) == 0x80) n++;
  if ((uint8_t)s[n] == 0xEF && (uint8_t)s[n + 1] == 0xB8 && (uint8_t)s[n + 2] == 0x8F) {
    n += 3;
  }
  return n;
}

int wrapUtf8ToLines(const char* src, int maxCharsPerLine, char outLines[][80], int maxLines) {
  int lineIdx = 0;
  const char* p = src;

  while (*p && lineIdx < maxLines) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == '\r') { p++; continue; }
    if (*p == '\n') {
      outLines[lineIdx][0] = '\0';
      lineIdx++;
      p++;
      continue;
    }

    int curChars = 0;
    size_t curBytes = 0;
    int lastSpaceCharPos = -1;
    size_t lastSpaceBytePos = 0;
    const char* lastSpaceSrcPtr = nullptr;
    const char* scan = p;

    while (*scan && *scan != '\n' && *scan != '\r') {
      size_t gBytes = nextUtf8GraphemeBytes(scan);
      if (curChars + 1 > maxCharsPerLine || curBytes + gBytes >= 79) {
        if (lastSpaceCharPos > 0 && lastSpaceSrcPtr != nullptr) {
          curBytes = lastSpaceBytePos;
          scan = lastSpaceSrcPtr + 1;
        }
        break;
      }

      if (*scan == ' ') {
        lastSpaceCharPos = curChars;
        lastSpaceBytePos = curBytes;
        lastSpaceSrcPtr  = scan;
      }

      memcpy(&outLines[lineIdx][curBytes], scan, gBytes);
      curBytes += gBytes;
      curChars++;
      scan += gBytes;
    }

    while (curBytes > 0 && outLines[lineIdx][curBytes - 1] == ' ') {
      curBytes--;
    }
    outLines[lineIdx][curBytes] = '\0';
    lineIdx++;

    if (*scan == '\r') scan++;
    if (*scan == '\n') scan++;
    p = scan;
  }

  return lineIdx;
}

void prepareWrappedCurrentMessage() {
  const char* txt = (msgHistoryCount > 0 && msgHistory[0].text[0] != '\0')
                    ? msgHistory[0].text
                    : "—";

  int tryCount = wrapUtf8ToLines(txt, 14, wrappedLines, MAX_WRAPPED_LINES);
  if (tryCount <= 3 && utf8CharCount(txt) <= 40) {
    wrappedScale = 2;
    wrappedLineCount = tryCount;
  } else {
    wrappedScale = 1;
    wrappedLineCount = wrapUtf8ToLines(txt, 26, wrappedLines, MAX_WRAPPED_LINES);
  }

  msgScrollY = 0.0f;
  msgScrollPauseSec = 1.8f;
  msgScrollDir = 1;
  msgNeedsRewrap = false;
}

// ============================================================================
// ХРАНИЛИЩЕ ПОСЛАНИЙ + ОТКРЫТОК В NVS & LITTLEFS
// ============================================================================

bool hasActiveCard0() {
  return fsReady && msgHistoryCount > 0 && msgHistory[0].hasCard && LittleFS.exists("/card0.lvc");
}

void loadSavedMessages() {
  msgHistoryCount = prefs.getInt("msgCount", 0);
  if (msgHistoryCount < 0) msgHistoryCount = 0;
  if (msgHistoryCount > MAX_MSG_HISTORY) msgHistoryCount = MAX_MSG_HISTORY;

  String lid = prefs.getString("lastMsgId", "");
  strncpy(lastNtfyId, lid.c_str(), sizeof(lastNtfyId) - 1);
  lastNtfyId[sizeof(lastNtfyId) - 1] = '\0';

  for (int i = 0; i < msgHistoryCount; i++) {
    char kTxt[12], kTm[12], kCrd[12];
    snprintf(kTxt, sizeof(kTxt), "msg%d", i);
    snprintf(kTm,  sizeof(kTm),  "mtm%d", i);
    snprintf(kCrd, sizeof(kCrd), "mcr%d", i);
    String s = prefs.getString(kTxt, "");
    strncpy(msgHistory[i].text, s.c_str(), sizeof(msgHistory[i].text) - 1);
    msgHistory[i].text[sizeof(msgHistory[i].text) - 1] = '\0';
    msgHistory[i].epoch = prefs.getUInt(kTm, 0);
    msgHistory[i].hasCard = (prefs.getInt(kCrd, 0) == 1);

    if (msgHistory[i].hasCard && fsReady) {
      char path[16];
      snprintf(path, sizeof(path), "/card%d.lvc", i);
      if (!LittleFS.exists(path)) msgHistory[i].hasCard = false;
    }
  }

  msgNeedsRewrap = true;
}

void pushNewMessageToHistory(const char* text, uint32_t epoch, const char* msgId, bool withPendingCard = false) {
  if ((!text || text[0] == '\0') && !withPendingCard) return;

  if (withPendingCard && fsReady) {
    lvcPromotePendingCard("/pending.lvc");
  } else if (fsReady) {
    lvcShiftEmptySlot0();
  }

  for (int i = MAX_MSG_HISTORY - 1; i >= 1; i--) {
    msgHistory[i] = msgHistory[i - 1];
  }

  if (text && text[0] != '\0') {
    strncpy(msgHistory[0].text, text, sizeof(msgHistory[0].text) - 1);
    msgHistory[0].text[sizeof(msgHistory[0].text) - 1] = '\0';
  } else {
    strncpy(msgHistory[0].text, "Открытка для тебя ❤", sizeof(msgHistory[0].text) - 1);
  }
  msgHistory[0].epoch = (epoch > 1700000000UL) ? epoch : getCurrentUtcEpoch();
  msgHistory[0].hasCard = withPendingCard;

  if (msgHistoryCount < MAX_MSG_HISTORY) msgHistoryCount++;

  if (msgId && msgId[0] != '\0') {
    strncpy(lastNtfyId, msgId, sizeof(lastNtfyId) - 1);
    lastNtfyId[sizeof(lastNtfyId) - 1] = '\0';
    prefs.putString("lastMsgId", String(lastNtfyId));
  }

  prefs.putInt("msgCount", msgHistoryCount);
  for (int i = 0; i < msgHistoryCount; i++) {
    char kTxt[12], kTm[12], kCrd[12];
    snprintf(kTxt, sizeof(kTxt), "msg%d", i);
    snprintf(kTm,  sizeof(kTm),  "mtm%d", i);
    snprintf(kCrd, sizeof(kCrd), "mcr%d", i);
    prefs.putString(kTxt, String(msgHistory[i].text));
    prefs.putUInt(kTm, msgHistory[i].epoch);
    prefs.putInt(kCrd, msgHistory[i].hasCard ? 1 : 0);
  }

  unreadMessage = true;
  msgNeedsRewrap = true;
}

// ============================================================================
// JSON ПАРСЕР И NTFY.SH (ПОДДЕРЖКА КАК ТЕКСТА, ТАК И .LVC ОТКРЫТОК)
// ============================================================================

size_t appendCodepointUtf8(uint16_t cp, char* out, size_t pos, size_t maxLen) {
  if (cp < 0x80) {
    if (pos + 1 < maxLen) out[pos++] = (char)cp;
  } else if (cp < 0x800) {
    if (pos + 2 < maxLen) {
      out[pos++] = (char)(0xC0 | (cp >> 6));
      out[pos++] = (char)(0x80 | (cp & 0x3F));
    }
  } else {
    if (pos + 3 < maxLen) {
      out[pos++] = (char)(0xE0 | (cp >> 12));
      out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[pos++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  return pos;
}

int findJsonKeyColon(const String& json, const char* key, int startFrom = 0) {
  String pat = String("\"") + key + "\"";
  int searchFrom = startFrom;
  const char* raw = json.c_str();
  while (true) {
    int kPos = json.indexOf(pat, searchFrom);
    if (kPos < 0) return -1;
    bool validBefore = (kPos == 0);
    if (!validBefore) {
      int b = kPos - 1;
      while (b > 0 && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\n' || raw[b] == '\r')) b--;
      validBefore = (raw[b] == '{' || raw[b] == ',');
    }
    int afterKey = kPos + pat.length();
    while (raw[afterKey] == ' ' || raw[afterKey] == '\t' || raw[afterKey] == '\n' || raw[afterKey] == '\r') {
      afterKey++;
    }
    if (validBefore && raw[afterKey] == ':') {
      return afterKey;
    }
    searchFrom = kPos + pat.length();
  }
}

bool extractJsonStringUnescaped(const String& json, const char* key, char* outBuf, size_t outSize, int startFrom = 0) {
  int colon = findJsonKeyColon(json, key, startFrom);
  if (colon < 0) return false;
  int q = json.indexOf('"', colon + 1);
  if (q < 0) return false;

  const char* s = json.c_str() + q + 1;
  size_t pos = 0;
  while (*s && pos + 1 < outSize) {
    if (*s == '"') {
      outBuf[pos] = '\0';
      return true;
    }
    if (*s == '\\' && s[1]) {
      s++;
      if (*s == 'n') outBuf[pos++] = '\n';
      else if (*s == 'r') { /* skip */ }
      else if (*s == 't') outBuf[pos++] = ' ';
      else if (*s == '"' || *s == '\\' || *s == '/') outBuf[pos++] = *s;
      else if (*s == 'u' && s[1] && s[2] && s[3] && s[4]) {
        char hex[5] = { s[1], s[2], s[3], s[4], '\0' };
        uint16_t cp = (uint16_t)strtoul(hex, nullptr, 16);
        pos = appendCodepointUtf8(cp, outBuf, pos, outSize);
        s += 4;
      } else {
        outBuf[pos++] = *s;
      }
      s++;
    } else {
      outBuf[pos++] = *s++;
    }
  }
  while (pos > 0 && ((uint8_t)outBuf[pos - 1] & 0xC0) == 0x80) pos--;
  if (pos > 0 && ((uint8_t)outBuf[pos - 1] & 0x80) != 0) {
    uint8_t lead = (uint8_t)outBuf[pos - 1];
    if ((lead & 0xE0) == 0xC0 || (lead & 0xF0) == 0xE0 || (lead & 0xF8) == 0xF0) pos--;
  }
  outBuf[pos] = '\0';
  return (pos > 0);
}

uint32_t extractJsonUintField(const String& json, const char* key) {
  int colon = findJsonKeyColon(json, key);
  if (colon < 0) return 0;
  const char* p = json.c_str() + colon + 1;
  while (*p == ' ') p++;
  return (uint32_t)strtoul(p, nullptr, 10);
}

bool parseSimpleFloat(const char* p, float &outVal) {
  if (!p) return false;
  while (*p == ' ' || *p == '\t') p++;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  else if (*p == '+') { p++; }

  bool anyDigit = false;
  float val = 0.0f;
  while (*p >= '0' && *p <= '9') {
    anyDigit = true;
    val = val * 10.0f + (float)(*p - '0');
    p++;
  }
  if (*p == '.') {
    p++;
    float frac = 0.1f;
    while (*p >= '0' && *p <= '9') {
      anyDigit = true;
      val += (float)(*p - '0') * frac;
      frac *= 0.1f;
      p++;
    }
  }
  if (!anyDigit) return false;
  outVal = neg ? -val : val;
  return true;
}

bool extractJsonFloatField(const String& json, const char* key, float &outVal, int startFrom = 0) {
  int colon = findJsonKeyColon(json, key, startFrom);
  if (colon < 0) return false;
  return parseSimpleFloat(json.c_str() + colon + 1, outVal);
}

class LvcFilePrint : public Stream {
public:
  File &file;
  size_t totalWritten = 0;
  size_t maxBytes;
  bool overflow = false;
  LvcFilePrint(File &f, size_t maxB) : file(f), maxBytes(maxB) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t size) override {
    if (overflow || totalWritten + size > maxBytes) {
      overflow = true;
      return 0;
    }
    size_t wr = file.write(buf, size);
    totalWritten += wr;
    return wr;
  }
};

// Скачивает файл открытки .lvc потоково прямо в LittleFS (/pending.lvc) — всего 1 КБ RAM!
bool downloadLvcAttachmentToFlash(const char* url, char* outCaption, size_t outCapSize) {
  if (!fsReady || !url || url[0] == '\0') return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(6);

  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(4500);
  https.setTimeout(8000);
  https.setReuse(false);

  if (!https.begin(client, String(url))) return false;
  int code = https.GET();
  int totalLen = https.getSize();
  bool ok = false;

  if (code == HTTP_CODE_OK && (totalLen == -1 || (totalLen >= LVC_HEADER_BASE && totalLen <= 250000))) {
    File f = LittleFS.open("/pending.lvc", "w");
    if (f) {
      LvcFilePrint fp(f, 250000);
      https.writeToStream(&fp);
      f.close();

      LvcMeta meta;
      if (!fp.overflow && fp.totalWritten >= LVC_HEADER_BASE && lvcReadMetaFromFile("/pending.lvc", meta)) {
        if (meta.caption[0] != '\0') {
          strncpy(outCaption, meta.caption, outCapSize - 1);
          outCaption[outCapSize - 1] = '\0';
        }
        ok = true;
      } else {
        LittleFS.remove("/pending.lvc");
      }
    }
  }

  https.end();
  return ok;
}

class BoundedStringPrint : public Stream {
public:
  String &out;
  size_t maxBytes;
  BoundedStringPrint(String &s, size_t maxB) : out(s), maxBytes(maxB) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t size) override {
    if (out.length() < maxBytes) {
      size_t space = maxBytes - out.length();
      size_t take = (size < space) ? size : space;
      for (size_t i = 0; i < take; i++) {
        out += (char)buf[i];
      }
    }
    return size;
  }
};

class NtfyLineStream : public Stream {
public:
  char* lineBuf;
  size_t lineCap;
  size_t lineLen = 0;
  char* newestId;
  size_t idCap;
  char* newestText;
  size_t textCap;
  char* newestAttachUrl;
  size_t attachCap;
  uint32_t &newestTime;

  NtfyLineStream(char* lb, size_t lc,
                 char* nId, size_t idC,
                 char* nTxt, size_t txtC,
                 char* nAtt, size_t attC,
                 uint32_t &nTime)
    : lineBuf(lb), lineCap(lc),
      newestId(nId), idCap(idC),
      newestText(nTxt), textCap(txtC),
      newestAttachUrl(nAtt), attachCap(attC),
      newestTime(nTime) {}

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  void processCurrentLine() {
    if (lineLen < 10) {
      lineLen = 0;
      return;
    }
    lineBuf[lineLen] = '\0';
    String line(lineBuf);
    lineLen = 0;
    line.trim();
    if (line.length() < 10) return;

    char ev[24] = "";
    if (!extractJsonStringUnescaped(line, "event", ev, sizeof(ev))) return;
    if (strcmp(ev, "message") != 0) return;

    char mid[32] = "";
    char mtxt[320] = "";
    int attPos = findJsonKeyColon(line, "attachment");
    bool hasMsg = extractJsonStringUnescaped(line, "message", mtxt, sizeof(mtxt));
    if (extractJsonStringUnescaped(line, "id", mid, sizeof(mid)) && (hasMsg || attPos > 0)) {
      if (strcmp(mid, lastNtfyId) != 0) {
        strncpy(newestId, mid, idCap - 1);
        newestId[idCap - 1] = '\0';
        strncpy(newestText, mtxt, textCap - 1);
        newestText[textCap - 1] = '\0';
        newestTime = extractJsonUintField(line, "time");
        newestAttachUrl[0] = '\0';

        if (attPos > 0) {
          extractJsonStringUnescaped(line, "url", newestAttachUrl, attachCap, attPos);
        }
      }
    }
  }

  size_t write(uint8_t b) override {
    if (b == '\n') {
      processCurrentLine();
    } else if (b != '\r' && lineLen + 1 < lineCap) {
      lineBuf[lineLen++] = (char)b;
    }
    return 1;
  }

  size_t write(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      write(buf[i]);
    }
    return size;
  }
};

void ntfyPollTask(void* /*param*/) {
  static char lineBuf[2048];
  static char newestText[320];
  static char newestId[32];
  static char newestAttachUrl[256];
  newestText[0] = '\0';
  newestId[0] = '\0';
  newestAttachUrl[0] = '\0';
  uint32_t newestTime = 0;
  int httpCode = -1;

  {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5);

    HTTPClient https;
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    https.setConnectTimeout(4500);
    https.setTimeout(6000);
    https.setReuse(false);

    String sinceArg = (lastNtfyId[0] != '\0') ? String(lastNtfyId) : String("48h");
    String url = String("https://ntfy.sh/") + NTFY_TOPIC + "/json?poll=1&since=" + sinceArg;

    if (https.begin(client, url)) {
      httpCode = https.GET();
      if (httpCode == HTTP_CODE_OK) {
        NtfyLineStream parser(lineBuf, sizeof(lineBuf),
                              newestId, sizeof(newestId),
                              newestText, sizeof(newestText),
                              newestAttachUrl, sizeof(newestAttachUrl),
                              newestTime);
        https.writeToStream(&parser);
        parser.processCurrentLine();
      }
      https.end();
    }
  }

  if (httpCode == HTTP_CODE_OK && newestId[0] != '\0' && !incomingMsgPendingApply) {
    bool gotCard = false;
    if (newestAttachUrl[0] != '\0') {
      gotCard = downloadLvcAttachmentToFlash(newestAttachUrl, newestText, sizeof(newestText));
    }
    strncpy(pendingMsgId, newestId, sizeof(pendingMsgId) - 1);
    pendingMsgId[sizeof(pendingMsgId) - 1] = '\0';
    strncpy(pendingMsgText, newestText, sizeof(pendingMsgText) - 1);
    pendingMsgText[sizeof(pendingMsgText) - 1] = '\0';
    pendingMsgEpoch = newestTime;
    pendingMsgHasCard = gotCard;
    incomingMsgPendingApply = true;
  }

  ntfyPollRunning = false;
  vTaskDelete(nullptr);
}

void startBackgroundNtfyPoll() {
  if (ntfyPollRunning || weatherPollRunning || otaTaskRunning || otaUpdateReady) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getFreeHeap() < 75000) return;
  ntfyPollRunning = true;
  if (xTaskCreate(ntfyPollTask, "ntfy_in", 20480, nullptr, 1, nullptr) != pdPASS) {
    ntfyPollRunning = false;
  }
}

void ntfyReplyTask(void* /*param*/) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient https;
  https.setConnectTimeout(4500);
  https.setTimeout(5000);
  https.setReuse(false);

  String url = String("https://ntfy.sh/") + NTFY_TOPIC + "_reply";
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "text/plain; charset=utf-8");
    https.POST("Она нажала кнопку на станции и прислала тебе сердечко!");
    https.end();
  }

  ntfyReplyRunning = false;
  vTaskDelete(nullptr);
}

void sendCloudReplyHeart() {
  unsigned long now = millis();
  if (ntfyReplyRunning || (now - lastReplySentMs < 5000UL)) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getFreeHeap() < 75000) return;

  lastReplySentMs = now;
  ntfyReplyRunning = true;
  if (xTaskCreate(ntfyReplyTask, "ntfy_out", 20480, nullptr, 1, nullptr) != pdPASS) {
    ntfyReplyRunning = false;
  }
}

// ============================================================================
// ПОГОДА OPEN-METEO (СТРАНИЦА 4 — ВСЕГДА «ГУЛЯБЕЛЬНО ❤»)
// ============================================================================

const char* weatherCodeToRu(int code) {
  if (code == 0) return "Ясно и солнечно";
  if (code == 1 || code == 2) return "Переменная облачность";
  if (code == 3) return "Пасмурно";
  if (code == 45 || code == 48) return "Лёгкий туман";
  if (code >= 51 && code <= 57) return "Морось";
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Дождик";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Снег";
  if (code >= 95) return "Гроза";
  return "Свежий воздух";
}

void weatherPollTask(void* /*param*/) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(4500);
  https.setTimeout(5000);
  https.setReuse(false);

  long latE4 = (long)roundf(weatherLat * 10000.0f);
  long lonE4 = (long)roundf(weatherLon * 10000.0f);
  bool latNeg = (latE4 < 0); if (latNeg) latE4 = -latE4;
  bool lonNeg = (lonE4 < 0); if (lonNeg) lonE4 = -lonE4;

  char urlBuf[200];
  snprintf(urlBuf, sizeof(urlBuf),
           "https://api.open-meteo.com/v1/forecast?latitude=%s%ld.%04ld&longitude=%s%ld.%04ld"
           "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m&wind_speed_unit=ms",
           latNeg ? "-" : "", latE4 / 10000L, latE4 % 10000L,
           lonNeg ? "-" : "", lonE4 / 10000L, lonE4 % 10000L);

  if (https.begin(client, String(urlBuf))) {
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      String body;
      BoundedStringPrint bp(body, 4096);
      https.writeToStream(&bp);
      int curPos = findJsonKeyColon(body, "current");
      if (curPos >= 0) {
        float t = 0.0f, app = 0.0f, wcode = 0.0f, wind = 0.0f;
        if (extractJsonFloatField(body, "temperature_2m", t, curPos)) {
          extractJsonFloatField(body, "apparent_temperature", app, curPos);
          extractJsonFloatField(body, "weather_code", wcode, curPos);
          extractJsonFloatField(body, "wind_speed_10m", wind, curPos);
          weatherTempC  = (int)roundf(t);
          weatherFeelsC = (int)roundf(app);
          weatherCode   = (int)roundf(wcode);
          weatherWindMs = (int)roundf(wind);
          weatherValid  = true;
          weatherNeedsRedraw = true;
        }
      }
    }
    https.end();
  }

  weatherPollRunning = false;
  vTaskDelete(nullptr);
}

void startBackgroundWeatherPoll() {
  if (weatherPollRunning || ntfyPollRunning || otaTaskRunning || otaUpdateReady) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getFreeHeap() < 75000) return;

  weatherPollRunning = true;
  if (xTaskCreate(weatherPollTask, "wx_poll", 20480, nullptr, 1, nullptr) != pdPASS) {
    weatherPollRunning = false;
  }
}

// ============================================================================
// БУФЕРИЗОВАННАЯ ГРАФИКА СЕРДЕЧЕК И СТРАНИЦ
// ============================================================================

float getHeartbeatScale(unsigned long ms) {
  float phase = (ms % 1400) / 1400.0f;
  if (phase < 0.12f) {
    return 0.92f + 0.15f * sinf((phase / 0.12f) * PI);
  } else if (phase >= 0.18f && phase < 0.32f) {
    return 0.92f + 0.09f * sinf(((phase - 0.18f) / 0.14f) * PI);
  }
  return 0.92f;
}

void renderBigHeartFrame(int16_t x0, int16_t y0, unsigned long ms, bool shimmerMode = false, float extraScale = 1.0f) {
  const int W = 112;
  const int H = 100;
  heartCanvas.fillScreen(COL_BG);

  float scale = getHeartbeatScale(ms) * extraScale;
  if (scale < 0.12f) scale = 0.12f;
  float invScale = 1.0f / scale;
  float basePhase = ms * 0.00028f;

  uint8_t baseR = 255, baseG = 42, baseB = 98;
  if (shimmerMode) {
    evalShimmerRGB(basePhase, baseR, baseG, baseB);
  }

  for (int py = 0; py < H; py++) {
    float ym = (1.22f - (py / (float)(H - 1)) * 2.40f) * invScale;
    float ym2 = ym * ym;
    float ym3 = ym2 * ym;

    for (int px = 0; px < W; px++) {
      float xm = (((px / (float)(W - 1)) * 2.56f) - 1.28f) * invScale;
      float xm2 = xm * xm;
      float a = xm2 + ym2 - 1.0f;
      if (a * a * a - xm2 * ym3 <= 0.0f) {
        float shade = 0.68f + 0.22f * ym - 0.14f * xm;
        if (shade < 0.35f) shade = 0.35f;
        if (shade > 1.0f) shade = 1.0f;

        float hx = xm + 0.42f;
        float hy = ym - 0.46f;
        float hlDist = hx * hx + hy * hy;

        uint8_t r, g, b;
        if (hlDist < 0.045f) {
          r = 255; g = 228; b = 238;
        } else if (hlDist < 0.11f) {
          r = 255; g = 150; b = 185;
        } else {
          r = (uint8_t)(baseR * shade);
          g = (uint8_t)(baseG * shade + 14.0f);
          b = (uint8_t)(baseB * shade + 22.0f);
        }
        heartCanvas.drawPixel(px, py, rgb565(r, g, b));
      }
    }
  }

  if (extraScale > 0.75f) {
    float twinkle = 0.5f + 0.5f * sinf(ms * 0.006f);
    if (twinkle > 0.48f) {
      uint16_t spCol = (twinkle > 0.78f) ? 0xFFFF : COL_PINK_SOFT;
      heartCanvas.drawFastHLine(10, 18, 5, spCol);
      heartCanvas.drawFastVLine(12, 16, 5, spCol);
      heartCanvas.drawFastHLine(96, 24, 5, spCol);
      heartCanvas.drawFastVLine(98, 22, 5, spCol);
    }
  }

  tft.drawRGBBitmap(x0, y0, heartCanvas.getBuffer(), W, H);
}

void renderMiniHeartBuffered(int16_t x0, int16_t y0, unsigned long ms, bool shimmer = false) {
  const int W = 28;
  const int H = 24;
  miniHeartCanvas.fillScreen(COL_BG);

  float scale = getHeartbeatScale(ms);
  float invScale = 1.0f / scale;

  uint8_t sr = 255, sg = 73, sb = 119;
  if (shimmer) evalShimmerRGB(ms * 0.0004f, sr, sg, sb);
  uint16_t mainCol = shimmer ? rgb565(sr, sg, sb) : COL_PINK_HOT;

  for (int py = 0; py < H; py++) {
    float ym = (1.24f - (py / (float)(H - 1)) * 2.48f) * invScale;
    float ym2 = ym * ym;
    float ym3 = ym2 * ym;
    for (int px = 0; px < W; px++) {
      float xm = (((px / (float)(W - 1)) * 2.60f) - 1.30f) * invScale;
      float xm2 = xm * xm;
      float a = xm2 + ym2 - 1.0f;
      if (a * a * a - xm2 * ym3 <= 0.0f) {
        uint16_t c = (xm < -0.25f && ym > 0.25f) ? COL_PINK_SOFT : mainCol;
        miniHeartCanvas.drawPixel(px, py, c);
      }
    }
  }

  tft.drawRGBBitmap(x0, y0, miniHeartCanvas.getBuffer(), W, H);
}

void clearOrbitPatchPreservingRing(int16_t bx, int16_t by) {
  const int S = 18;
  uint16_t* buf = orbitCanvas.getBuffer();
  for (int py = 0; py < S; py++) {
    int dy = (by + py) - 120;
    for (int px = 0; px < S; px++) {
      int dx = (bx + px) - 120;
      int r2 = dx * dx + dy * dy;
      buf[py * S + px] = (r2 >= 13572 && r2 <= 14042) ? COL_RING_DIM : COL_BG;
    }
  }
  tft.drawRGBBitmap(bx, by, buf, S, S);
}

void renderOrbitHeartBuffered(float exactSecond0to60, unsigned long nowMs) {
  const int S = 18;
  const float R_ORBIT = 110.0f;

  float angleRad = (exactSecond0to60 * 6.0f - 90.0f) * DEG_TO_RAD;
  float fx = 120.0f + cosf(angleRad) * R_ORBIT;
  float fy = 120.0f + sinf(angleRad) * R_ORBIT;

  int16_t bx = (int16_t)roundf(fx) - (S / 2);
  int16_t by = (int16_t)roundf(fy) - (S / 2);

  if (prevOrbitBx > -500 && (abs(bx - prevOrbitBx) > 2 || abs(by - prevOrbitBy) > 2)) {
    clearOrbitPatchPreservingRing(prevOrbitBx, prevOrbitBy);
  }
  prevOrbitBx = bx;
  prevOrbitBy = by;

  float cxLocal = fx - (float)bx;
  float cyLocal = fy - (float)by;

  float pulse = 1.0f + 0.06f * sinf(nowMs * 0.007f);
  float invRadius = 1.0f / (5.1f * pulse);
  float basePhase = nowMs * 0.00032f;

  uint16_t* buf = orbitCanvas.getBuffer();

  for (int py = 0; py < S; py++) {
    int screenDy = (by + py) - 120;
    float ym = (cyLocal - ((float)py + 0.5f)) * invRadius + 0.10f;
    float ym2 = ym * ym;
    float ym3 = ym2 * ym;

    for (int px = 0; px < S; px++) {
      int screenDx = (bx + px) - 120;
      int r2 = screenDx * screenDx + screenDy * screenDy;
      uint16_t bgCol = (r2 >= 13572 && r2 <= 14042) ? COL_RING_DIM : COL_BG;

      float xm = (((float)px + 0.5f) - cxLocal) * invRadius;
      float xm2 = xm * xm;
      float a = xm2 + ym2 - 1.0f;
      float val = a * a * a - xm2 * ym3;

      if (val <= 0.12f) {
        uint8_t r, g, b;
        evalShimmerRGB(basePhase + 0.14f * (xm - ym), r, g, b);

        float hx = xm + 0.38f;
        float hy = ym - 0.42f;
        if (hx * hx + hy * hy < 0.12f) {
          r = (uint8_t)((r + 255) / 2);
          g = (uint8_t)((g + 245) / 2);
          b = (uint8_t)((b + 250) / 2);
        }

        if (val <= -0.14f) {
          buf[py * S + px] = rgb565(r, g, b);
        } else {
          float coverage = (0.12f - val) / 0.26f;
          if (coverage < 0.0f) coverage = 0.0f;
          if (coverage > 1.0f) coverage = 1.0f;
          uint8_t br = (uint8_t)(r * coverage);
          uint8_t bg = (uint8_t)(g * coverage);
          uint8_t bb = (uint8_t)(b * coverage);
          buf[py * S + px] = (coverage > 0.15f) ? rgb565(br, bg, bb) : bgCol;
        }
      } else {
        buf[py * S + px] = bgCol;
      }
    }
  }

  tft.drawRGBBitmap(bx, by, buf, S, S);
}

void renderDaysBlockBuffered(uint64_t days, int16_t xSlideOffset = 0) {
  const int W = 168;
  const int H = 56;
  msgCanvas.fillScreen(COL_BG);

  char numBuf[32];
  if (days < 10000ULL) {
    formatU64Grouped(days, numBuf, sizeof(numBuf), false);
  } else {
    formatU64Grouped(days, numBuf, sizeof(numBuf), true);
  }

  uint16_t len = utf8CharCount(numBuf);
  uint8_t numScale = 4;
  int16_t numY = 1;

  if (len <= 7) {
    numScale = 4;
    numY = 1;
  } else if (len <= 9) {
    numScale = 3;
    numY = 7;
  } else {
    numScale = 2;
    numY = 11;
  }

  int16_t nw = (int16_t)(len * 6 * numScale);
  int16_t nx = (W - nw) / 2 + xSlideOffset;
  drawTextUTF8(msgCanvas, nx, numY, numBuf, 0xFFFF, COL_BG, numScale);

  const char* dWord = pluralUpper(days, "ДЕНЬ", "ДНЯ", "ДНЕЙ");
  int16_t dw = (int16_t)(utf8CharCount(dWord) * 6 * 2);
  int16_t dx = (W - dw) / 2 + xSlideOffset;
  drawTextUTF8(msgCanvas, dx, 39, dWord, COL_PINK_HOT, COL_BG, 2);

  uint16_t* src = msgCanvas.getBuffer();
  int stride = msgCanvas.width();
  for (int y = 1; y < H; y++) {
    memmove(src + y * W, src + y * stride, (size_t)W * sizeof(uint16_t));
  }
  tft.drawRGBBitmap(36, 67, src, W, H);
}

void renderTimePillBuffered(const char* text, uint16_t textColor, int16_t xSlideOffset = 0) {
  pillCanvas.fillScreen(COL_BG);
  if (xSlideOffset == 0) {
    pillCanvas.fillRoundRect(0, 0, 168, 30, 12, COL_CARD_BG);
    pillCanvas.drawRoundRect(0, 0, 168, 30, 12, COL_RING_DIM);
    drawCenteredUTF8(pillCanvas, 7, text, textColor, COL_CARD_BG, 2);
  } else {
    int16_t rx = xSlideOffset;
    pillCanvas.fillRoundRect(rx, 0, 168, 30, 12, COL_CARD_BG);
    pillCanvas.drawRoundRect(rx, 0, 168, 30, 12, COL_RING_DIM);
    int16_t tw = (int16_t)(utf8CharCount(text) * 12);
    drawTextUTF8(pillCanvas, rx + (168 - tw) / 2, 7, text, textColor, COL_CARD_BG, 2);
  }
  tft.drawRGBBitmap(36, 129, pillCanvas.getBuffer(), 168, 30);
}

void renderTickerBuffered(float dtSec) {
  if (tickerText[0] == '\0') return;

  tickerScrollPx += 45.0f * dtSec;
  if (tickerScrollPx >= (float)tickerLoopPx) {
    tickerScrollPx -= (float)tickerLoopPx;
  }

  const int W = 152;
  const int H = 16;
  const int FADE = 14;

  tickerCanvas.fillScreen(COL_BG);
  int16_t xStart = -(int16_t)tickerScrollPx;

  drawTextUTF8(tickerCanvas, xStart, 4, tickerText, COL_PINK_SOFT, COL_BG, 1);
  drawTextUTF8(tickerCanvas, xStart + tickerLoopPx, 4, tickerText, COL_PINK_SOFT, COL_BG, 1);

  uint16_t* buf = tickerCanvas.getBuffer();
  for (int x = 0; x < FADE; x++) {
    uint8_t alpha = (uint8_t)((x * 255) / FADE);
    int rx = W - 1 - x;
    for (int y = 0; y < H; y++) {
      int idxL = y * W + x;
      int idxR = y * W + rx;
      if (buf[idxL]) buf[idxL] = dim565(buf[idxL], alpha);
      if (buf[idxR]) buf[idxR] = dim565(buf[idxR], alpha);
    }
  }

  tft.drawRGBBitmap(44, 166, buf, W, H);
}

void renderMessageCardBuffered(float dtSec) {
  if (msgNeedsRewrap) {
    prepareWrappedCurrentMessage();
  }

  const int W = 172;
  const int H = 86;
  msgCanvas.fillScreen(COL_BG);
  msgCanvas.fillRoundRect(0, 0, W, H, 14, COL_CARD_BG);

  int lineStep = (wrappedScale == 2) ? 22 : 13;
  int totalTextH = wrappedLineCount * lineStep;
  int innerPadY = 8;
  int visibleH = H - innerPadY * 2;

  int16_t startY = innerPadY;
  bool scrolling = false;

  if (totalTextH <= visibleH) {
    startY = (H - totalTextH) / 2 + 2;
  } else {
    scrolling = true;
    float maxScroll = (float)(totalTextH - visibleH);
    if (msgScrollPauseSec > 0.0f) {
      msgScrollPauseSec -= dtSec;
    } else {
      msgScrollY += (float)msgScrollDir * 18.0f * dtSec;
      if (msgScrollY >= maxScroll) {
        msgScrollY = maxScroll;
        msgScrollDir = -1;
        msgScrollPauseSec = 2.0f;
      } else if (msgScrollY <= 0.0f) {
        msgScrollY = 0.0f;
        msgScrollDir = 1;
        msgScrollPauseSec = 2.0f;
      }
    }
    startY = innerPadY - (int16_t)roundf(msgScrollY);
  }

  for (int i = 0; i < wrappedLineCount; i++) {
    int16_t ly = startY + i * lineStep;
    if (ly + (8 * wrappedScale) < 2 || ly > H - 2) continue;
    drawCenteredUTF8(msgCanvas, ly, wrappedLines[i], 0xFFFF, 0xFFFF, wrappedScale, W);
  }

  if (scrolling) {
    uint16_t* buf = msgCanvas.getBuffer();
    for (int fy = 1; fy < 10; fy++) {
      uint8_t a = (uint8_t)((fy * 255) / 10);
      int by = H - 1 - fy;
      for (int x = 2; x < W - 2; x++) {
        if (buf[fy * W + x] != COL_CARD_BG) {
          buf[fy * W + x] = dim565(buf[fy * W + x], a);
          if (buf[fy * W + x] == 0) buf[fy * W + x] = COL_CARD_BG;
        }
        if (buf[by * W + x] != COL_CARD_BG) {
          buf[by * W + x] = dim565(buf[by * W + x], a);
          if (buf[by * W + x] == 0) buf[by * W + x] = COL_CARD_BG;
        }
      }
    }
  }

  msgCanvas.drawRoundRect(0, 0, W, H, 14, unreadMessage ? COL_PINK_HOT : COL_RING_DIM);
  tft.drawRGBBitmap(34, 62, msgCanvas.getBuffer(), W, H);
}

void renderHoldHintBuffered(const char* msg) {
  tickerCanvas.fillScreen(COL_BG);
  if (msg && msg[0] != '\0') {
    drawCenteredUTF8(tickerCanvas, 4, msg, COL_GOLD_SOFT, COL_BG, 1);
  }
  tft.drawRGBBitmap(44, 188, tickerCanvas.getBuffer(), 152, 16);
}

void drawPageDots(int page1to4, bool onPhotoCard = false) {
  int16_t y = onPhotoCard ? 225 : 207;
  if (onPhotoCard) {
    tft.fillRoundRect(92, y - 5, 56, 11, 5, COL_CARD_BG);
  }
  for (int i = 1; i <= 4; i++) {
    int16_t x = 102 + (i - 1) * 12;
    if (i == page1to4) {
      tft.fillCircle(x, y, 3, COL_PINK_HOT);
    } else {
      tft.fillCircle(x, y, 3, onPhotoCard ? COL_CARD_BG : COL_BG);
      tft.drawCircle(x, y, 2, COL_RING_DIM);
    }
  }
  if (unreadMessage && page1to4 != 2) {
    tft.fillCircle(114, y, 3, COL_GOLD_SOFT);
  }
}

// Рисует все 15 полос открытки /card0.lvc на экран (используя msgCanvas + heartCanvas без лишней RAM)
void drawFullCard0Instant() {
  LvcMeta meta;
  if (!lvcReadMetaFromFile("/card0.lvc", meta)) return;
  File f = LittleFS.open("/card0.lvc", "r");
  if (!f) return;

  uint16_t* stripBuf = msgCanvas.getBuffer();
  uint8_t*  compBuf  = (uint8_t*)heartCanvas.getBuffer();
  const size_t compCap = 112 * 100 * 2;

  for (int s = 0; s < LVC_STRIP_COUNT; s++) {
    lvcDrawStripFromFile(f, meta, s, stripBuf, compBuf, compCap, COL_RING_DIM, tft);
  }
  f.close();
  drawPageDots(2, true);
  activeCardRendered = true;
}

// ============================================================================
// СТАТИЧЕСКИЕ КАРКАСЫ ЭКРАНОВ И ПЛАВНЫЕ ПЕРЕХОДЫ
// ============================================================================

void drawDecorativeRings() {
  tft.drawCircle(120, 120, 118, COL_RING_DIM);
  tft.drawCircle(120, 120, 117, COL_RING_DIM);
}

void playBootAnimation() {
  tft.fillScreen(COL_BG);
  for (int r = 8; r <= 116; r += 8) {
    tft.drawCircle(120, 120, r, COL_PINK_HOT);
    if (r > 20) tft.drawCircle(120, 120, r - 16, COL_BG);
    delay(12);
  }
  tft.fillScreen(COL_BG);
}

void drawApSetupStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "СТАНЦИЯ ОТНОШЕНИЙ", COL_PINK_SOFT, COL_BG, 1);
  drawCenteredUTF8(tft, 145, "Подключи Wi-Fi:", COL_MUTED, COL_BG, 1);
  drawCenteredUTF8(tft, 160, AP_SSID, COL_MINT_OK, COL_BG, 2);
  drawCenteredUTF8(tft, 184, "Сайт: 192.168.4.1", COL_GOLD_SOFT, COL_BG, 1);
}

void drawTimerStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 47, "МЫ ВМЕСТЕ", COL_PINK_SOFT, COL_BG, 2);
  drawPageDots(1, false);

  prevSecondDrawn = -1;
  prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
  prevSyncStateDrawn = true;
  prevOrbitBx = -999;
  prevOrbitBy = -999;
}

void drawMessagesStatic() {
  activeCardRendered = false;
  if (hasActiveCard0()) {
    drawFullCard0Instant();
    return;
  }

  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  if (unreadMessage) {
    drawCenteredUTF8(tft, 45, "НОВОЕ ПОСЛАНИЕ", COL_GOLD_SOFT, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 45, "ПОСЛАНИЕ ДЛЯ ТЕБЯ", COL_PINK_SOFT, COL_BG, 1);
  }

  if (msgHistoryCount > 0) {
    char tmStr[40];
    formatMsgTimestamp(msgHistory[0].epoch, tmStr, sizeof(tmStr));
    drawCenteredUTF8(tft, 156, tmStr, COL_MUTED, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 156, "—", COL_MUTED, COL_BG, 1);
  }
  drawCenteredUTF8(tft, 173, "Кн.1 - послать ♥ в ответ", COL_PINK_SOFT, COL_BG, 1);

  drawPageDots(2, false);
  msgNeedsRewrap = true;
}

void drawHeartIpStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "СТАНЦИЯ ОТНОШЕНИЙ", COL_PINK_SOFT, COL_BG, 1);

  if (wifiConnected) {
    drawCenteredUTF8(tft, 143, "Адрес страницы:", COL_MUTED, COL_BG, 1);
    String ipStr = WiFi.localIP().toString();
    uint8_t ipScale = (ipStr.length() <= 13) ? 2 : 1;
    int16_t ipY = (ipScale == 2) ? 157 : 161;
    drawCenteredUTF8(tft, ipY, ipStr.c_str(), COL_GOLD_SOFT, COL_BG, ipScale);
    char verLine[32];
    snprintf(verLine, sizeof(verLine), "Wi-Fi ок • v%s", FIRMWARE_VERSION);
    drawCenteredUTF8(tft, 180, verLine, COL_MINT_OK, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 150, "Поиск домашней сети...", COL_GOLD_SOFT, COL_BG, 1);
    drawCenteredUTF8(tft, 168, savedSsid.c_str(), COL_PINK_SOFT, COL_BG, 1);
  }

  drawPageDots(3, false);
}

// Анимированная иконка погоды 56x44 (рисуем через msgCanvas без лишней RAM!)
void renderWeatherIconBuffered(unsigned long nowMs) {
  const int W = 56;
  const int H = 44;
  msgCanvas.fillScreen(COL_BG);

  int code = weatherValid ? weatherCode : 0;
  bool hasSun   = (code == 0 || code == 1 || code == 2);
  bool hasCloud = (code >= 1);
  bool hasRain  = ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95);
  bool hasSnow  = ((code >= 71 && code <= 77) || code == 85 || code == 86);
  bool hasStorm = (code >= 95);
  bool hasFog   = (code == 45 || code == 48);

  if (hasSun) {
    int16_t sx = hasCloud ? 36 : 28;
    int16_t sy = hasCloud ? 15 : 21;
    int16_t sr = hasCloud ? 9 : 11;
    msgCanvas.fillCircle(sx, sy, sr, COL_GOLD_SOFT);
    float baseAng = nowMs * 0.0012f;
    for (int i = 0; i < 8; i++) {
      float a = baseAng + i * (PI / 4.0f);
      int16_t rx1 = sx + (int16_t)roundf(cosf(a) * (sr + 3));
      int16_t ry1 = sy + (int16_t)roundf(sinf(a) * (sr + 3));
      int16_t rx2 = sx + (int16_t)roundf(cosf(a) * (sr + 6));
      int16_t ry2 = sy + (int16_t)roundf(sinf(a) * (sr + 6));
      msgCanvas.drawLine(rx1, ry1, rx2, ry2, COL_GOLD_SOFT);
    }
  }

  if (hasCloud) {
    uint16_t cloudCol = hasStorm ? rgb565(175, 165, 200) : rgb565(235, 232, 248);
    int16_t cy = (hasRain || hasSnow || hasStorm) ? 18 : 23;
    msgCanvas.fillCircle(19, cy, 8, cloudCol);
    msgCanvas.fillCircle(29, cy - 3, 10, cloudCol);
    msgCanvas.fillCircle(38, cy + 1, 7, cloudCol);
    msgCanvas.fillRoundRect(12, cy, 32, 10, 5, cloudCol);
  }

  if (hasFog) {
    uint16_t fogCol = rgb565(180, 175, 205);
    int16_t drift = (int16_t)roundf(sinf(nowMs * 0.002f) * 3.0f);
    msgCanvas.fillRoundRect(10 + drift, 32, 36, 3, 1, fogCol);
    msgCanvas.fillRoundRect(14 - drift, 37, 28, 3, 1, COL_MUTED);
  }

  if (hasRain) {
    uint16_t dropCol = rgb565(96, 185, 255);
    int phase = (int)((nowMs / 75) % 12);
    for (int d = 0; d < 3; d++) {
      int16_t dx = 18 + d * 10;
      int16_t dy = 29 + ((phase + d * 4) % 12);
      msgCanvas.drawFastVLine(dx, dy, 4, dropCol);
      msgCanvas.drawFastVLine(dx + 1, dy, 4, dropCol);
    }
  }

  if (hasSnow) {
    int phase = (int)((nowMs / 100) % 12);
    for (int d = 0; d < 3; d++) {
      int16_t dx = 18 + d * 10 + (int16_t)roundf(sinf(nowMs * 0.004f + d) * 2.0f);
      int16_t dy = 29 + ((phase + d * 4) % 12);
      msgCanvas.fillCircle(dx, dy, 1, 0xFFFF);
    }
  }

  if (hasStorm && ((nowMs / 250) % 4 == 0)) {
    msgCanvas.drawLine(29, 27, 24, 34, COL_GOLD_SOFT);
    msgCanvas.drawLine(24, 34, 30, 34, COL_GOLD_SOFT);
    msgCanvas.drawLine(30, 34, 25, 41, COL_GOLD_SOFT);
  }

  uint16_t* src = msgCanvas.getBuffer();
  int stride = msgCanvas.width();
  for (int y = 1; y < H; y++) {
    memmove(src + y * W, src + y * stride, (size_t)W * sizeof(uint16_t));
  }
  tft.drawRGBBitmap(92, 34, src, W, H);
}

void renderWeatherWalkablePill(unsigned long nowMs, int16_t yOffset = 0) {
  pillCanvas.fillScreen(COL_BG);
  uint8_t sr, sg, sb;
  evalShimmerRGB(nowMs * 0.00035f, sr, sg, sb);
  uint16_t borderCol = rgb565(sr, sg, sb);

  pillCanvas.fillRoundRect(0, 0, 168, 30, 13, COL_CARD_BG);
  pillCanvas.drawRoundRect(0, 0, 168, 30, 13, borderCol);
  drawCenteredUTF8(pillCanvas, 7, "ГУЛЯБЕЛЬНО ❤", COL_MINT_OK, COL_CARD_BG, 2);
  tft.drawRGBBitmap(36, 144 + yOffset, pillCanvas.getBuffer(), 168, 30);
}

void drawWeatherStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();

  char titleBuf[48];
  snprintf(titleBuf, sizeof(titleBuf), "ПОГОДА • %s", weatherCity.c_str());
  drawCenteredUTF8(tft, 20, titleBuf, COL_PINK_SOFT, COL_BG, 1);

  if (weatherValid) {
    char tempBuf[16];
    snprintf(tempBuf, sizeof(tempBuf), "%s%d°C", (weatherTempC > 0 ? "+" : ""), (int)weatherTempC);
    drawCenteredUTF8(tft, 83, tempBuf, 0xFFFF, COL_BG, 3);
    drawCenteredUTF8(tft, 113, weatherCodeToRu(weatherCode), COL_GOLD_SOFT, COL_BG, 1);

    char detBuf[48];
    snprintf(detBuf, sizeof(detBuf), "Ощущается %s%d° • %d м/с",
             (weatherFeelsC > 0 ? "+" : ""), (int)weatherFeelsC, (int)weatherWindMs);
    drawCenteredUTF8(tft, 127, detBuf, COL_MUTED, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 83, "—", 0xFFFF, COL_BG, 3);
    drawCenteredUTF8(tft, 117, "—", COL_MUTED, COL_BG, 1);
  }

  renderWeatherWalkablePill(millis(), 0);
  drawPageDots(4, false);
  weatherNeedsRedraw = false;
}

void drawPulsePhraseBuffered() {
  pillCanvas.fillScreen(COL_BG);
  drawCenteredUTF8(pillCanvas, 7, LOVE_PHRASES[lovePhraseIndex], 0xFFFF, COL_BG, 2);
  tft.drawRGBBitmap(36, 146, pillCanvas.getBuffer(), 168, 30);
}

void drawPulseStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "СТАНЦИЯ ОТНОШЕНИЙ", COL_PINK_SOFT, COL_BG, 1);
  drawPulsePhraseBuffered();
  if (pulseSentReply) {
    drawCenteredUTF8(tft, 182, "Ответное ♥ отправлено!", COL_MINT_OK, COL_BG, 1);
  }
}

// Стирает кольцо без зазоров (через точные горизонтальные хорды) и рисует сияющий обод
void drawIrisClosingRing(int rOuter, int rInner, unsigned long nowMs) {
  if (rInner < 0) rInner = 0;
  if (rOuter > 118) rOuter = 118;
  uint8_t sr, sg, sb;
  evalShimmerRGB(nowMs * 0.0012f, sr, sg, sb);
  uint16_t haloCol = rgb565(sr, sg, sb);

  int rOuter2 = rOuter * rOuter;
  int rInner2 = rInner * rInner;
  int rHalo2  = (rInner > 3) ? ((rInner - 3) * (rInner - 3)) : 0;

  tft.startWrite();
  for (int y = 120 - rOuter; y <= 120 + rOuter; y++) {
    if (y < 0 || y >= 240) continue;
    int dy = y - 120;
    int dy2 = dy * dy;
    if (dy2 > rOuter2) continue;

    int xOut = (int)sqrtf((float)(rOuter2 - dy2));
    int xIn  = (dy2 < rInner2) ? (int)sqrtf((float)(rInner2 - dy2)) : 0;

    if (xOut >= xIn) {
      tft.writeFastHLine(120 - xOut, y, xOut - xIn + 1, COL_BG);
      tft.writeFastHLine(120 + xIn,  y, xOut - xIn + 1, COL_BG);
    }

    if (rInner > 3 && dy2 < rInner2) {
      int xH = (dy2 < rHalo2) ? (int)sqrtf((float)(rHalo2 - dy2)) : 0;
      if (xIn > xH) {
        tft.writeFastHLine(120 - xIn, y, xIn - xH + 1, haloCol);
        tft.writeFastHLine(120 + xH,  y, xIn - xH + 1, haloCol);
      }
    }
  }
  tft.endWrite();
}

void beginPageTransition(ScreenMode target) {
  transActive = true;
  transPhase = 0;
  transTarget = target;
  transStartMs = millis();
  transPrevR = 118;
  transStripStep = -1;
}

void stepPageTransition(unsigned long nowMs) {
  unsigned long dt = nowMs - transStartMs;

  if (transPhase == 0) {
    // Фаза 0 (145 мс): сияющее кольцо плавно сжимается к центру экрана
    float p = dt / 145.0f;
    if (p > 1.0f) p = 1.0f;
    float ease = p * p * (3.0f - 2.0f * p);
    int curR = (int)roundf(118.0f * (1.0f - ease));

    if (curR < transPrevR) {
      drawIrisClosingRing(transPrevR, curR, nowMs);
      transPrevR = curR;
    }

    if (p >= 1.0f) {
      tft.fillCircle(120, 120, 8, COL_BG);
      currentScreen = transTarget;
      transPhase = 1;
      transStartMs = nowMs;
      transStripStep = -1;
      activeCardRendered = false;

      if (currentScreen == SCREEN_TIMER) {
        drawTimerStatic();
      } else if (currentScreen == SCREEN_HEART_IP) {
        drawHeartIpStatic();
      } else if (currentScreen == SCREEN_WEATHER) {
        drawWeatherStatic();
      } else if (currentScreen == SCREEN_PULSE) {
        drawPulseStatic();
      } else if (currentScreen == SCREEN_MESSAGES && !hasActiveCard0()) {
        drawMessagesStatic();
      }
    }
    return;
  }

  // Фаза 1: эффектное появление выбранной страницы
  if (currentScreen == SCREEN_MESSAGES && hasActiveCard0()) {
    // Двухстворчатое раскрытие 240x240 открытки от экватора вверх и вниз с сияющей полосой!
    int targetStep = (int)(dt / 26); // 8 шагов * 26 мс ≈ 208 мс
    if (targetStep > 7) targetStep = 7;

    if (targetStep > transStripStep) {
      LvcMeta meta;
      if (lvcReadMetaFromFile("/card0.lvc", meta)) {
        File f = LittleFS.open("/card0.lvc", "r");
        if (f) {
          uint16_t* stripBuf = msgCanvas.getBuffer();
          uint8_t*  compBuf  = (uint8_t*)heartCanvas.getBuffer();
          const size_t compCap = 112 * 100 * 2;

          uint8_t sr, sg, sb;
          evalShimmerRGB(nowMs * 0.001f, sr, sg, sb);
          uint16_t beamCol = rgb565(sr, sg, sb);

          while (transStripStep < targetStep) {
            transStripStep++;
            int k = transStripStep;
            if (k == 0) {
              lvcDrawStripFromFile(f, meta, 7, stripBuf, compBuf, compCap, COL_RING_DIM, tft);
            } else {
              lvcDrawStripFromFile(f, meta, 7 - k, stripBuf, compBuf, compCap, COL_RING_DIM, tft);
              lvcDrawStripFromFile(f, meta, 7 + k, stripBuf, compBuf, compCap, COL_RING_DIM, tft);
            }
            if (k < 7) {
              int yTop = (7 - k) * LVC_STRIP_ROWS - 2;
              int yBot = (7 + k + 1) * LVC_STRIP_ROWS;
              if (yTop >= 4 && yTop < 236) tft.drawFastHLine(24, yTop, 192, beamCol);
              if (yBot >= 4 && yBot < 236) tft.drawFastHLine(24, yBot, 192, beamCol);
            }
          }
          f.close();
        }
      }
      if (targetStep >= 7) {
        drawPageDots(2, true);
        activeCardRendered = true;
        transActive = false;
      }
    }
    return;
  }

  float p = dt / 230.0f;
  if (p > 1.0f) p = 1.0f;
  float easeOut = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);

  if (currentScreen == SCREEN_TIMER) {
    renderMiniHeartBuffered(106, 20, nowMs, false);
    int16_t slide = (int16_t)roundf(62.0f * (1.0f - easeOut));

    uint32_t nowUtc = 0, nowUsec = 0;
    if (getCurrentExactUtc(nowUtc, nowUsec)) {
      int64_t startUtc = (int64_t)REL_START_UTC_MIDNIGHT - (int64_t)tzOffsetHours * 3600LL;
      int64_t diffSigned = (int64_t)nowUtc - startUtc;
      uint64_t diff = (diffSigned > 0) ? (uint64_t)diffSigned : 0ULL;
      uint64_t days = diff / 86400ULL;
      uint32_t rem  = (uint32_t)(diff % 86400ULL);
      char timeBuf[24];
      snprintf(timeBuf, sizeof(timeBuf), "%02lu : %02lu : %02lu",
               (unsigned long)(rem / 3600UL), (unsigned long)((rem % 3600UL) / 60UL), (unsigned long)(rem % 60UL));

      renderDaysBlockBuffered(days, slide);
      renderTimePillBuffered(timeBuf, 0xFFFF, -slide);
    } else {
      renderTimePillBuffered("-- : -- : --", COL_PINK_SOFT, -slide);
    }

    if (p >= 1.0f) {
      prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
      prevSecondDrawn = -1;
      transActive = false;
    }
  } else if (currentScreen == SCREEN_HEART_IP || currentScreen == SCREEN_PULSE) {
    // Пружинистое появление 3D-сердечка (0.22 -> 1.13 -> 1.0)
    float elastic = (p < 0.72f)
      ? (0.22f + 0.91f * sinf((p / 0.72f) * (PI * 0.5f)))
      : (1.13f - 0.13f * ((p - 0.72f) / 0.28f));
    bool shimmer = (currentScreen == SCREEN_PULSE);
    int16_t hy = (currentScreen == SCREEN_PULSE) ? 34 : 36;
    renderBigHeartFrame(64, hy, nowMs, shimmer, elastic);

    if (p >= 1.0f) {
      transActive = false;
    }
  } else if (currentScreen == SCREEN_WEATHER) {
    renderWeatherIconBuffered(nowMs);
    renderWeatherWalkablePill(nowMs, 0);
    if (p >= 1.0f) transActive = false;
  } else {
    // Текстовое сообщение Стр. 2 без картинки
    renderMiniHeartBuffered(106, 16, nowMs, true);
    renderMessageCardBuffered(0.03f);
    if (p >= 1.0f) transActive = false;
  }
}

void triggerLovePulse(bool sendReplyToSender = true) {
  if (sendReplyToSender) {
    sendCloudReplyHeart();
    pulseSentReply = (WiFi.status() == WL_CONNECTED);
  } else {
    pulseSentReply = false;
  }

  if (unreadMessage) {
    unreadMessage = false;
  }

  if (currentScreen == SCREEN_PULSE && !transActive) {
    lovePhraseIndex = (lovePhraseIndex + 1) % LOVE_PHRASES_COUNT;
    pulseStartMs = millis();
    drawPulsePhraseBuffered();
    return;
  }
  returnScreenAfterPulse = (currentScreen == SCREEN_PULSE) ? transTarget : currentScreen;
  if (returnScreenAfterPulse == SCREEN_PULSE) returnScreenAfterPulse = SCREEN_TIMER;
  pulseStartMs = millis();
  beginPageTransition(SCREEN_PULSE);
}

void updateTimerDynamic(unsigned long nowMs, float dtSec) {
  renderMiniHeartBuffered(106, 20, nowMs, false);

  uint32_t nowUtc = 0, nowUsec = 0;
  if (!getCurrentExactUtc(nowUtc, nowUsec)) {
    if (prevSyncStateDrawn) {
      prevSyncStateDrawn = false;
      msgCanvas.fillScreen(COL_BG);
      drawCenteredUTF8(msgCanvas, 12, "Синхронизация", COL_GOLD_SOFT, COL_BG, 2, 168);
      drawCenteredUTF8(msgCanvas, 36, "точного времени...", COL_MUTED, COL_BG, 1, 168);
      uint16_t* src = msgCanvas.getBuffer();
      int stride = msgCanvas.width();
      for (int y = 1; y < 56; y++) {
        memmove(src + y * 168, src + y * stride, 168 * sizeof(uint16_t));
      }
      tft.drawRGBBitmap(36, 67, src, 168, 56);
      renderTimePillBuffered("-- : -- : --", COL_PINK_SOFT);
      snprintf(tickerText, sizeof(tickerText), "ПОДКЛЮЧАЕМСЯ К СЕРВЕРУ ТОЧНОГО ВРЕМЕНИ  •  ");
      tickerLoopPx = (int)utf8CharCount(tickerText) * 6;
    }
    float waitOrbitSec = fmodf(nowMs / 250.0f, 60.0f);
    renderOrbitHeartBuffered(waitOrbitSec, nowMs);
    renderTickerBuffered(dtSec);
    return;
  }

  if (!prevSyncStateDrawn) {
    prevSyncStateDrawn = true;
    prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
    prevSecondDrawn = -1;
  }

  int64_t startUtc = (int64_t)REL_START_UTC_MIDNIGHT - (int64_t)tzOffsetHours * 3600LL;
  int64_t diffSigned = (int64_t)nowUtc - startUtc;
  uint64_t diff = (diffSigned > 0) ? (uint64_t)diffSigned : 0ULL;

  uint64_t days = diff / 86400ULL;
  uint32_t rem  = (uint32_t)(diff % 86400ULL);
  uint32_t hrs  = rem / 3600UL;
  uint32_t mins = (rem % 3600UL) / 60UL;
  uint32_t secs = rem % 60UL;

  float exactSec = (float)secs + ((float)nowUsec / 1000000.0f);
  renderOrbitHeartBuffered(exactSec, nowMs);

  if (days != prevDaysDrawn) {
    prevDaysDrawn = days;
    renderDaysBlockBuffered(days);
  }

  if ((int)secs != prevSecondDrawn) {
    prevSecondDrawn = (int)secs;

    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "%02lu : %02lu : %02lu",
             (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs);
    renderTimePillBuffered(timeBuf, 0xFFFF);

    buildTickerString(diff);
  }

  renderTickerBuffered(dtSec);
}

void showConnectedBanner(const String& ip) {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  renderBigHeartFrame(64, 30, millis());
  drawCenteredUTF8(tft, 140, "ПАРОЛЬ СОХРАНЁН!", COL_MINT_OK, COL_BG, 1);
  drawCenteredUTF8(tft, 158, ip.c_str(), COL_GOLD_SOFT, COL_BG, 2);
  drawCenteredUTF8(tft, 184, "Открой в браузере", COL_MUTED, COL_BG, 1);
  bannerActive = true;
  bannerUntilMs = millis() + 2600UL;
  transActive = false;
}

// ============================================================================
// АВТООБНОВЛЕНИЕ С GITHUB (HTTPS OTA)
// ============================================================================

bool isNewerVersion(const char* remoteVer, const char* localVer) {
  if (*remoteVer == 'v' || *remoteVer == 'V') remoteVer++;
  if (*localVer  == 'v' || *localVer  == 'V') localVer++;

  for (int part = 0; part < 4; part++) {
    long rNum = 0, lNum = 0;
    while (*remoteVer >= '0' && *remoteVer <= '9') {
      rNum = rNum * 10 + (*remoteVer - '0');
      remoteVer++;
    }
    while (*localVer >= '0' && *localVer <= '9') {
      lNum = lNum * 10 + (*localVer - '0');
      localVer++;
    }
    if (rNum > lNum) return true;
    if (rNum < lNum) return false;
    if (*remoteVer == '.') remoteVer++;
    if (*localVer  == '.') localVer++;
    if (*remoteVer == '\0' && *localVer == '\0') break;
  }
  return false;
}

void otaCheckTask(void* /*param*/) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(4500);
  https.setTimeout(5000);
  https.setReuse(false);

  String url = String(OTA_VERSION_URL) + "?t=" + String(millis());
  if (https.begin(client, url)) {
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      String payload;
      BoundedStringPrint bp(payload, 2048);
      https.writeToStream(&bp);
      char remVer[24] = "";
      char remBin[256] = "";
      if (extractJsonStringUnescaped(payload, "version", remVer, sizeof(remVer)) &&
          extractJsonStringUnescaped(payload, "bin_url", remBin, sizeof(remBin))) {
        if (isNewerVersion(remVer, FIRMWARE_VERSION)) {
          strncpy(otaPendingVersion, remVer, sizeof(otaPendingVersion) - 1);
          otaPendingVersion[sizeof(otaPendingVersion) - 1] = '\0';
          strncpy(otaPendingBinUrl, remBin, sizeof(otaPendingBinUrl) - 1);
          otaPendingBinUrl[sizeof(otaPendingBinUrl) - 1] = '\0';
          otaUpdateReady = true;
        }
      }
    }
    https.end();
  }

  otaTaskRunning = false;
  vTaskDelete(nullptr);
}

void startBackgroundOtaCheck() {
  if (otaTaskRunning || otaUpdateReady || ntfyPollRunning || weatherPollRunning) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (ESP.getFreeHeap() < 75000) return;

  otaForceRequested = false;
  otaTaskRunning = true;
  if (xTaskCreate(otaCheckTask, "ota_chk", 20480, nullptr, 1, nullptr) != pdPASS) {
    otaTaskRunning = false;
  }
}

void renderOtaProgressBarBuffered(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  pillCanvas.fillScreen(COL_BG);
  pillCanvas.fillRoundRect(0, 0, 168, 30, 12, COL_CARD_BG);
  pillCanvas.drawRoundRect(0, 0, 168, 30, 12, COL_RING_DIM);

  int fillW = (160 * percent) / 100;
  if (fillW > 0) {
    pillCanvas.fillRoundRect(4, 4, fillW, 22, 9, COL_PINK_HOT);
  }

  char pctBuf[16];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", percent);
  drawCenteredUTF8(pillCanvas, 8, pctBuf, 0xFFFF, 0xFFFF, 2);
  tft.drawRGBBitmap(36, 150, pillCanvas.getBuffer(), 168, 30);
}

void performOtaUpdate() {
  otaUpdateReady = false;
  transActive = false;

  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "ОБНОВЛЕНИЕ СИСТЕМЫ", COL_PINK_SOFT, COL_BG, 1);

  char verMsg[40];
  snprintf(verMsg, sizeof(verMsg), "Версия v%s", otaPendingVersion);
  drawCenteredUTF8(tft, 134, verMsg, COL_GOLD_SOFT, COL_BG, 1);
  renderOtaProgressBarBuffered(0);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(6000);
  https.setTimeout(15000);
  https.setReuse(false);

  bool success = false;

  if (https.begin(client, String(otaPendingBinUrl))) {
    int code = https.GET();
    int totalLen = https.getSize();

    if (code == HTTP_CODE_OK && (totalLen > 100000 || totalLen == -1)) {
      size_t updateSize = (totalLen > 0) ? (size_t)totalLen : UPDATE_SIZE_UNKNOWN;
      if (Update.begin(updateSize)) {
        WiFiClient* stream = https.getStreamPtr();
        uint8_t buff[1024];
        size_t written = 0;
        bool firstChunk = true;
        bool aborted = false;
        unsigned long lastUiMs = 0;
        unsigned long lastDataMs = millis();

        while ((https.connected() || stream->available() > 0) &&
               (totalLen == -1 || (int)written < totalLen)) {
          size_t avail = stream->available();
          if (avail > 0) {
            size_t toRead = (avail > sizeof(buff)) ? sizeof(buff) : avail;
            int readBytes = stream->readBytes(buff, toRead);
            if (readBytes > 0) {
              if (firstChunk) {
                firstChunk = false;
                if (buff[0] != 0xE9) {
                  aborted = true;
                  break;
                }
              }
              if (Update.write(buff, (size_t)readBytes) != (size_t)readBytes) {
                aborted = true;
                break;
              }
              written += (size_t)readBytes;
              lastDataMs = millis();

              unsigned long now = millis();
              if (now - lastUiMs >= 60) {
                lastUiMs = now;
                renderBigHeartFrame(64, 32, now, true);
                int pct = (totalLen > 0) ? (int)((written * 100ULL) / (size_t)totalLen) : 50;
                renderOtaProgressBarBuffered(pct);
              }
            }
          } else {
            if (millis() - lastDataMs > 15000UL) {
              aborted = true;
              break;
            }
            delay(5);
          }
          yield();
        }

        if (!aborted && Update.end(true)) {
          success = true;
        } else {
          Update.abort();
        }
      }
    }
    https.end();
  }

  if (success) {
    renderOtaProgressBarBuffered(100);
    drawCenteredUTF8(tft, 188, "ГОТОВО! ПЕРЕЗАПУСК...", COL_MINT_OK, COL_BG, 1);
    delay(1200);
    ESP.restart();
  } else {
    drawCenteredUTF8(tft, 188, "Не удалось обновить", COL_PINK_HOT, COL_BG, 1);
    delay(1800);
    screenNeedsFullRedraw = true;
  }
}

// ============================================================================
// WI-FI И ВСТРОЕННЫЙ ВЕБ-СЕРВЕР
// ============================================================================

String jsonEscape(const char* s) {
  String out;
  if (!s) return out;
  while (*s) {
    char c = *s++;
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') { /* skip */ }
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void startApPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  delay(80);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  WiFi.scanNetworks(true);
  apActive = true;
  transActive = false;
  currentScreen = SCREEN_AP_SETUP;
  screenNeedsFullRedraw = true;
}

bool connectToSavedWifi(unsigned long timeoutMs) {
  if (savedSsid.length() == 0) return false;

  WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    renderBigHeartFrame(64, 36, millis());
    delay(30);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    wifiConnected = true;
    configureNtp();
    nextOtaCheckMs = millis() + OTA_FIRST_CHECK_DELAY_MS;
    nextNtfyPollMs = millis() + 10000UL;
    nextWeatherPollMs = millis() + 14000UL;
    return true;
  }

  wifiConnected = false;
  return false;
}

void sendWebUiGzip() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEB_HTML_GZ, WEB_HTML_GZ_LEN);
}

void setupWebRoutes() {
  server.on("/", HTTP_GET, []() {
    sendWebUiGzip();
  });

  server.on("/api/status", HTTP_GET, []() {
    bool conn = (WiFi.status() == WL_CONNECTED);
    String ip = conn ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String ssid = conn ? WiFi.SSID() : savedSsid;

    String json = "{";
    json += "\"connected\":" + String(conn ? "true" : "false") + ",";
    json += "\"ssid\":\"" + jsonEscape(ssid.c_str()) + "\",";
    json += "\"saved_ssid\":\"" + jsonEscape(savedSsid.c_str()) + "\",";
    json += "\"has_saved_pass\":" + String(savedPass.length() > 0 ? "true" : "false") + ",";
    json += "\"ip\":\"" + ip + "\",";
    json += "\"tz_offset\":" + String(tzOffsetHours) + ",";
    json += "\"weather_city\":\"" + jsonEscape(weatherCity.c_str()) + "\",";
    json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\"";
    json += "}";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/api/weather", HTTP_GET, []() {
    String json = "{";
    json += "\"valid\":" + String(weatherValid ? "true" : "false") + ",";
    json += "\"city\":\"" + jsonEscape(weatherCity.c_str()) + "\",";
    json += "\"temp\":" + String((int)weatherTempC) + ",";
    json += "\"feels\":" + String((int)weatherFeelsC) + ",";
    json += "\"wind\":" + String((int)weatherWindMs) + ",";
    json += "\"code\":" + String((int)weatherCode) + ",";
    json += "\"desc\":\"" + jsonEscape(weatherCodeToRu(weatherCode)) + "\",";
    json += "\"walkable\":true";
    json += "}";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/api/weather_city", HTTP_POST, []() {
    String c = server.arg("city");
    c.trim();
    if (c.length() > 0) {
      weatherCity = c;
      prefs.putString("wCity", weatherCity);
    }
    if (server.hasArg("lat") && server.hasArg("lon")) {
      float latV = weatherLat, lonV = weatherLon;
      if (parseSimpleFloat(server.arg("lat").c_str(), latV) &&
          parseSimpleFloat(server.arg("lon").c_str(), lonV)) {
        weatherLat = latV;
        weatherLon = lonV;
        prefs.putFloat("wLat", weatherLat);
        prefs.putFloat("wLon", weatherLon);
      }
    }
    weatherValid = false;
    nextWeatherPollMs = millis() + 200UL;
    if (currentScreen == SCREEN_WEATHER) screenNeedsFullRedraw = true;
    server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  });

  server.on("/api/messages", HTTP_GET, []() {
    String json = "{";
    json += "\"unread\":" + String(unreadMessage ? "true" : "false") + ",";
    json += "\"latest_text\":\"" + jsonEscape(msgHistoryCount > 0 ? msgHistory[0].text : "—") + "\",";
    json += "\"latest_time\":" + String(msgHistoryCount > 0 ? msgHistory[0].epoch : 0) + ",";
    json += "\"latest_has_card\":" + String((msgHistoryCount > 0 && msgHistory[0].hasCard) ? "true" : "false") + ",";
    json += "\"history\":[";
    for (int i = 0; i < msgHistoryCount; i++) {
      if (i > 0) json += ",";
      json += "{\"text\":\"" + jsonEscape(msgHistory[i].text) + "\",";
      json += "\"time\":" + String(msgHistory[i].epoch) + ",";
      json += "\"has_card\":" + String(msgHistory[i].hasCard ? "true" : "false") + "}";
    }
    json += "]}";
    server.send(200, "application/json; charset=utf-8", json);
  });

  // Отдаёт бинарную открытку /card<idx>.lvc на сайт станции для отрисовки 1-в-1!
  server.on("/api/card", HTTP_GET, []() {
    int idx = server.hasArg("idx") ? server.arg("idx").toInt() : 0;
    if (!fsReady || idx < 0 || idx >= MAX_MSG_HISTORY) {
      server.send(404, "text/plain", "not found");
      return;
    }
    char path[16];
    snprintf(path, sizeof(path), "/card%d.lvc", idx);
    if (!LittleFS.exists(path)) {
      server.send(404, "text/plain", "no card");
      return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
      server.send(500, "text/plain", "open err");
      return;
    }
    server.streamFile(f, "application/octet-stream");
    f.close();
  });

  server.on("/api/mark_read", HTTP_POST, []() {
    if (unreadMessage) {
      unreadMessage = false;
      if (currentScreen == SCREEN_MESSAGES && !hasActiveCard0()) {
        screenNeedsFullRedraw = true;
      } else if (currentScreen == SCREEN_TIMER || currentScreen == SCREEN_HEART_IP || currentScreen == SCREEN_WEATHER) {
        int pg = (currentScreen == SCREEN_TIMER) ? 1 : ((currentScreen == SCREEN_HEART_IP) ? 3 : 4);
        drawPageDots(pg, false);
      }
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/scan", HTTP_GET, []() {
    if (server.hasArg("refresh")) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true);
      server.send(200, "application/json; charset=utf-8", "{\"scanning\":true}");
      return;
    }

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true);
      server.send(200, "application/json; charset=utf-8", "{\"scanning\":true}");
      return;
    }
    if (n == WIFI_SCAN_RUNNING) {
      server.send(200, "application/json; charset=utf-8", "{\"scanning\":true}");
      return;
    }

    String json = "[";
    bool first = true;
    for (int i = 0; i < n && i < 15; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      if (!first) json += ",";
      first = false;
      bool openNet = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      json += "{\"ssid\":\"" + jsonEscape(ssid.c_str()) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"open\":" + String(openNet ? "true" : "false") + "}";
    }
    json += "]";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/api/sync_time", HTTP_POST, []() {
    if (server.hasArg("epoch")) {
      uint32_t ep = (uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10);
      setSystemEpoch(ep);
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/pulse", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    triggerLovePulse(true);
  });

  server.on("/api/ota_check", HTTP_POST, []() {
    otaForceRequested = true;
    startBackgroundOtaCheck();
    server.send(200, "application/json; charset=utf-8",
                "{\"ok\":true,\"message\":\"Проверяем GitHub... Если доступна новая версия, станция обновится автоматически.\"}");
  });

  server.on("/api/connect", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    newSsid.trim();
    String newPass = server.arg("pass");
    int newTz = server.hasArg("tz") ? server.arg("tz").toInt() : tzOffsetHours;

    if (server.hasArg("epoch")) {
      uint32_t ep = (uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10);
      setSystemEpoch(ep);
    }

    if (newTz != tzOffsetHours) {
      tzOffsetHours = newTz;
      prefs.putInt("tz", tzOffsetHours);
      configureNtp();
      prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
      prevSecondDrawn = -1;
    }

    if (newSsid.length() == 0 || (WiFi.status() == WL_CONNECTED && newSsid == savedSsid && newPass.length() == 0)) {
      if (WiFi.status() == WL_CONNECTED) {
        String homeIp = WiFi.localIP().toString();
        server.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"ip\":\"" + homeIp + "\"}");
        return;
      }
      if (newSsid.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false}");
        return;
      }
    }

    String passToTry = (newSsid == savedSsid && newPass.length() == 0) ? savedPass : newPass;

    WiFi.scanDelete();
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(newSsid.c_str(), passToTry.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 11000) {
      renderBigHeartFrame(64, 38, millis());
      delay(30);
    }

    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      savedSsid = newSsid;
      savedPass = passToTry;
      prefs.putString("ssid", savedSsid);
      prefs.putString("pass", savedPass);

      wifiConnected = true;
      configureNtp();
      nextOtaCheckMs = millis() + OTA_FIRST_CHECK_DELAY_MS;
      nextNtfyPollMs = millis() + 10000UL;
      nextWeatherPollMs = millis() + 14000UL;
      String homeIp = WiFi.localIP().toString();

      String resp = "{\"ok\":true,\"ip\":\"" + homeIp + "\"}";
      server.send(200, "application/json; charset=utf-8", resp);

      pendingDisableAp = true;
      disableApAtMs = millis() + 10000UL;

      showConnectedBanner(homeIp);
    } else {
      wifiConnected = false;
      server.send(200, "application/json; charset=utf-8", "{\"ok\":false}");
    }
  });

  auto captiveRedirect = []() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  };
  server.on("/generate_204", HTTP_GET, captiveRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, captiveRedirect);
  server.on("/ncsi.txt", HTTP_GET, captiveRedirect);
  server.on("/connecttest.txt", HTTP_GET, captiveRedirect);
  server.onNotFound([]() {
    if (apActive) {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    } else {
      sendWebUiGzip();
    }
  });

  server.begin();
}

// ============================================================================
// ОБРАБОТКА КНОПОК (СКРОЛЛ 3 СТРАНИЦ С АНИМАЦИЕЙ КНОПКОЙ 2 + ОТВЕТ КНОПКОЙ 1)
// ============================================================================

void resetWifiSettings() {
  prefs.remove("ssid");
  prefs.remove("pass");
  savedSsid = "";
  savedPass = "";
  wifiConnected = false;
  WiFi.disconnect(true);
  delay(150);
  startApPortal();
}

void scrollNextPage() {
  ScreenMode cur = transActive ? transTarget : currentScreen;
  ScreenMode nextMode = SCREEN_TIMER;

  if (cur == SCREEN_PULSE) {
    nextMode = SCREEN_TIMER;
  } else if (cur == SCREEN_TIMER) {
    nextMode = SCREEN_MESSAGES;
    unreadMessage = false;
  } else if (cur == SCREEN_MESSAGES) {
    unreadMessage = false;
    nextMode = SCREEN_HEART_IP;
  } else if (cur == SCREEN_HEART_IP) {
    nextMode = SCREEN_WEATHER;
  } else {
    nextMode = SCREEN_TIMER;
  }

  beginPageTransition(nextMode);
}

bool btn1HandledCurrentPress = false;
bool btn2HandledCurrentPress = false;

void handleButtons(unsigned long nowMs) {
  bool b1Pressed = false, b1Released = false;
  bool b2Pressed = false, b2Released = false;

  updateButton(btn1, nowMs, b1Pressed, b1Released);
  bool b2Held = updateButton(btn2, nowMs, b2Pressed, b2Released);

  if (b1Pressed) {
    btn1HandledCurrentPress = true;
    if ((nowMs - lastBtn1ActionMs) >= PAGE_SWITCH_COOLDOWN_MS) {
      lastBtn1ActionMs = nowMs;
      triggerLovePulse(true);
    }
  } else if (b1Released) {
    btn1HandledCurrentPress = false;
  }

  if (b2Pressed) {
    btn2PressStartMs = nowMs;
    btn2LongHandled = false;
    lastHoldSecShown = 0;
    btn2HandledCurrentPress = true;
    if (!apActive && !bannerActive && (nowMs - lastPageSwitchMs) >= PAGE_SWITCH_COOLDOWN_MS) {
      lastPageSwitchMs = nowMs;
      scrollNextPage();
    }
  } else if (b2Held && !btn2LongHandled) {
    unsigned long held = nowMs - btn2PressStartMs;
    if (held >= 1200 && held < 4000) {
      int secLeft = 4 - (int)(held / 1000);
      if (secLeft != lastHoldSecShown) {
        lastHoldSecShown = secLeft;
        char buf[32];
        snprintf(buf, sizeof(buf), "Сброс Wi-Fi: %d...", secLeft);
        renderHoldHintBuffered(buf);
      }
    } else if (held >= 4000) {
      btn2LongHandled = true;
      tft.fillScreen(COL_BG);
      drawDecorativeRings();
      drawCenteredUTF8(tft, 112, "СБРОС WI-FI...", COL_PINK_HOT, COL_BG, 2);
      delay(600);
      resetWifiSettings();
    }
  } else if (b2Released) {
    btn2HandledCurrentPress = false;
    if (!btn2LongHandled && lastHoldSecShown > 0) {
      renderHoldHintBuffered("");
      lastHoldSecShown = 0;
    }
  }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  pinMode(BTN_1, INPUT_PULLUP);
  pinMode(BTN_2, INPUT_PULLUP);

  btn1.rawState = btn1.stableState = (digitalRead(BTN_1) == HIGH);
  btn2.rawState = btn2.stableState = (digitalRead(BTN_2) == HIGH);
  btn1.lastChangeMs = btn2.lastChangeMs = millis();

  SPI.begin(19, 20, 18, TFT_CS);
  tft.begin(40000000);
  tft.setRotation(0);

  playBootAnimation();

  fsReady = LittleFS.begin(true);

  prefs.begin("lovebox", false);
  savedSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  savedPass = prefs.getString("pass", DEFAULT_WIFI_PASS);
  tzOffsetHours = prefs.getInt("tz", 3);
  weatherCity = prefs.getString("wCity", "Москва");
  weatherLat  = prefs.getFloat("wLat", 55.7558f);
  weatherLon  = prefs.getFloat("wLon", 37.6173f);
  uint32_t lastSavedEpoch = prefs.getUInt("lastEpoch", 0);
  if (lastSavedEpoch > 1700000000UL) {
    setSystemEpoch(lastSavedEpoch);
  }
  loadSavedMessages();

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  if (savedSsid.length() > 0) {
    tft.fillScreen(COL_BG);
    drawDecorativeRings();
    drawCenteredUTF8(tft, 20, "СТАНЦИЯ ОТНОШЕНИЙ", COL_PINK_SOFT, COL_BG, 1);
    drawCenteredUTF8(tft, 152, "Подключение к Wi-Fi...", COL_MUTED, COL_BG, 1);
    drawCenteredUTF8(tft, 170, savedSsid.c_str(), COL_GOLD_SOFT, COL_BG, 1);

    if (connectToSavedWifi(10000)) {
      apActive = false;
      currentScreen = SCREEN_TIMER;
      screenNeedsFullRedraw = true;
    } else {
      startApPortal();
      lastWifiRetryMs = millis();
    }
  } else {
    startApPortal();
  }

  setupWebRoutes();
  lastFrameMs = millis();
}

void loop() {
  unsigned long nowMs = millis();

  if (apActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();

  if (pendingDisableAp && nowMs >= disableApAtMs) {
    pendingDisableAp = false;
    dnsServer.stop();
    WiFi.enableAP(false);
    WiFi.setSleep(false);
    apActive = false;
  }

  if (savedSsid.length() > 0) {
    bool nowConn = (WiFi.status() == WL_CONNECTED);

    if (apActive && !pendingDisableAp) {
      if (nowConn) {
        WiFi.setSleep(false);
        wifiConnected = true;
        configureNtp();
        nextOtaCheckMs = nowMs + OTA_FIRST_CHECK_DELAY_MS;
        nextNtfyPollMs = nowMs + 10000UL;
        nextWeatherPollMs = nowMs + 14000UL;
        dnsServer.stop();
        WiFi.enableAP(false);
        apActive = false;
        beginPageTransition(SCREEN_TIMER);
      } else if (nowMs - lastWifiRetryMs > 15000UL) {
        lastWifiRetryMs = nowMs;
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
    } else if (!apActive) {
      if (nowConn != wifiConnected) {
        wifiConnected = nowConn;
        if (nowConn) WiFi.setSleep(false);
        if (currentScreen == SCREEN_HEART_IP && !transActive) screenNeedsFullRedraw = true;
      }
      if (!nowConn && (nowMs - lastWifiRetryMs > 15000UL)) {
        lastWifiRetryMs = nowMs;
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
      }
      if (nowConn && !bannerActive) {
        if ((long)(nowMs - nextNtfyPollMs) >= 0) {
          nextNtfyPollMs = nowMs + NTFY_POLL_INTERVAL_MS;
          startBackgroundNtfyPoll();
        }
        if ((long)(nowMs - nextWeatherPollMs) >= 0) {
          nextWeatherPollMs = nowMs + WEATHER_POLL_INTERVAL_MS;
          startBackgroundWeatherPoll();
        }
        if (otaForceRequested || (long)(nowMs - nextOtaCheckMs) >= 0) {
          nextOtaCheckMs = nowMs + OTA_CHECK_INTERVAL_MS;
          startBackgroundOtaCheck();
        }
      }
    }
  }

  if (weatherNeedsRedraw && currentScreen == SCREEN_WEATHER && !transActive && !bannerActive) {
    weatherNeedsRedraw = false;
    screenNeedsFullRedraw = true;
  }

  if (incomingMsgPendingApply) {
    pushNewMessageToHistory(pendingMsgText, pendingMsgEpoch, pendingMsgId, pendingMsgHasCard);
    incomingMsgPendingApply = false;
    if (!apActive && !bannerActive) {
      beginPageTransition(SCREEN_MESSAGES);
    }
  }

  if (otaUpdateReady && !bannerActive) {
    performOtaUpdate();
    lastFrameMs = millis();
    return;
  }

  handleButtons(nowMs);

  if (bannerActive) {
    if ((long)(nowMs - bannerUntilMs) >= 0) {
      bannerActive = false;
      beginPageTransition(SCREEN_TIMER);
    } else {
      unsigned long elapsedMs = nowMs - lastFrameMs;
      if (elapsedMs >= 30) {
        lastFrameMs = nowMs;
        renderBigHeartFrame(64, 30, nowMs, false);
      }
      delay(2);
      return;
    }
  }

  if (currentScreen == SCREEN_PULSE && !transActive && (nowMs - pulseStartMs >= 2400)) {
    lovePhraseIndex = (lovePhraseIndex + 1) % LOVE_PHRASES_COUNT;
    ScreenMode back = apActive ? SCREEN_AP_SETUP : returnScreenAfterPulse;
    if (back == SCREEN_PULSE) back = SCREEN_TIMER;
    if (back == SCREEN_AP_SETUP) {
      currentScreen = SCREEN_AP_SETUP;
      screenNeedsFullRedraw = true;
    } else {
      beginPageTransition(back);
    }
  }

  // Если идёт анимация переключения страниц — рисуем её плавно без блокировки кнопок/Wi-Fi
  if (transActive) {
    stepPageTransition(nowMs);
    lastFrameMs = nowMs;
    delay(2);
    return;
  }

  if (screenNeedsFullRedraw) {
    screenNeedsFullRedraw = false;
    if (currentScreen == SCREEN_AP_SETUP) {
      drawApSetupStatic();
    } else if (currentScreen == SCREEN_MESSAGES) {
      drawMessagesStatic();
    } else if (currentScreen == SCREEN_HEART_IP) {
      drawHeartIpStatic();
    } else if (currentScreen == SCREEN_WEATHER) {
      drawWeatherStatic();
    } else if (currentScreen == SCREEN_PULSE) {
      drawPulseStatic();
    } else {
      drawTimerStatic();
    }
  }

  unsigned long elapsedMs = nowMs - lastFrameMs;
  if (elapsedMs >= 30) {
    float dtSec = elapsedMs / 1000.0f;
    if (dtSec > 0.1f) dtSec = 0.03f;
    lastFrameMs = nowMs;

    if (currentScreen == SCREEN_AP_SETUP || currentScreen == SCREEN_HEART_IP) {
      renderBigHeartFrame(64, 36, nowMs, false);
    } else if (currentScreen == SCREEN_PULSE) {
      renderBigHeartFrame(64, 34, nowMs, true);
    } else if (currentScreen == SCREEN_WEATHER) {
      renderWeatherIconBuffered(nowMs);
      renderWeatherWalkablePill(nowMs, 0);
      renderMiniHeartBuffered(106, 178, nowMs, true);
    } else if (currentScreen == SCREEN_MESSAGES) {
      if (!hasActiveCard0()) {
        renderMiniHeartBuffered(106, 16, nowMs, true);
        renderMessageCardBuffered(dtSec);
      } else if (!activeCardRendered) {
        drawFullCard0Instant();
      }
    } else if (currentScreen == SCREEN_TIMER) {
      updateTimerDynamic(nowMs, dtSec);
    }
  }

  static unsigned long lastEpochSaveMs = 0;
  if (timeSynced && (nowMs - lastEpochSaveMs > 600000UL)) {
    lastEpochSaveMs = nowMs;
    uint32_t ep = getCurrentUtcEpoch();
    if (ep > 0) prefs.putUInt("lastEpoch", ep);
  }

  // На одноядерном ESP32-C6 delay(2) отдаёт время IDLE0 для освобождения завершённых FreeRTOS-задач и сброса WDT
  delay(2);
}
