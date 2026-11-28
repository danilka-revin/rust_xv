// Станция отношений (Seeed Studio XIAO ESP32-C6 + GC9A01A 240x240)
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
#include "esp_timer.h"

#include "app_types.h"
#include "cyrillic_font.h"
#include "love_card.h"
#include "web_ui.h"

// --- ВЕРСИЯ ПРОШИВКИ И GITHUB АВТООБНОВЛЕНИЕ (OTA ПО ПОСЛЕДНЕМУ КОММИТУ MAIN) ---
#define FIRMWARE_VERSION "1.3.8"

static const char* GITHUB_REPO   = "danilka-revin/rust_xv";
static const char* GITHUB_BRANCH = "main";

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
static const unsigned long BTN_PRESS_DEBOUNCE_MS   = 15;
static const unsigned long BTN_RELEASE_DEBOUNCE_MS = 60;
static const unsigned long PAGE_SWITCH_COOLDOWN_MS = 280;
static const unsigned long BTN1_HEART_HOLD_MS      = 10000UL;
static const unsigned long BTN1_HOLD_UI_DELAY_MS   = 160UL;

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

static constexpr float PI_F = 3.14159265f;
static constexpr float DEG2RAD_F = 0.017453292f;

static void copyCStr(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static inline void makeIdxKey(char* out, const char* pfx3, int idx) {
  memcpy(out, pfx3, 3);
  out[3] = (char)('0' + idx);
  out[4] = '\0';
}

static uint32_t isqrt32(uint32_t x) {
  uint32_t res = 0, bit = 1UL << 30;
  while (bit > x) bit >>= 2;
  while (bit != 0) {
    if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
    else res >>= 1;
    bit >>= 2;
  }
  return res;
}

static inline int iroundf(float v) {
  return (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
}

static const int16_t PROGMEM SIN_Q15[17] = {
  0, 3212, 6393, 9512, 12540, 15447, 18205, 20788, 23170,
  25330, 27246, 28899, 30274, 31357, 32138, 32610, 32767
};

static float fastSinF(float x) {
  float q = x * 0.63661977f;
  int32_t iq = (q >= 0.0f) ? (int32_t)q : ((int32_t)q - 1);
  float frac = q - (float)iq;
  int quad = (int)(iq & 3);
  bool neg = (quad >= 2);
  if (quad & 1) frac = 1.0f - frac;
  float fidx = frac * 16.0f;
  int idx = (int)fidx;
  if (idx >= 16) return neg ? -1.0f : 1.0f;
  float rem = fidx - (float)idx;
  float v0 = (float)(int16_t)pgm_read_word(&SIN_Q15[idx]);
  float v1 = (float)(int16_t)pgm_read_word(&SIN_Q15[idx + 1]);
  float res = (v0 + (v1 - v0) * rem) * (1.0f / 32767.0f);
  return neg ? -res : res;
}

static inline void evalShimmerRGB(float phase, uint8_t &outR, uint8_t &outG, uint8_t &outB) {
  static const uint8_t PALETTE[5][3] = {
    { 255, 120, 175 },
    { 255,  42, 102 },
    { 255,  92,  76 },
    { 255, 188, 118 },
    { 218,  96, 255 }
  };
  float p = phase - (float)(int)phase;
  if (p < 0.0f) p += 1.0f;
  float scaled = p * 5.0f;
  int idx0 = ((int)scaled) % 5;
  int idx1 = (idx0 + 1) % 5;
  float frac = scaled - (float)(int)scaled;
  float t = frac * frac * (3.0f - 2.0f * frac);

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

// --- ЭКРАН И ОФФСКРИН-БУФЕРЫ (0% МЕРЦАНИЙ, БЕЗ БЕЛОГО ШУМА ПРИ СТАРТЕ) ---
class CleanGC9A01A : public Adafruit_GC9A01A {
public:
  CleanGC9A01A(int8_t cs, int8_t dc, int8_t rst) : Adafruit_GC9A01A(cs, dc, rst) {}

  void beginNoSnow(uint32_t freq) {
    invertOnCommand  = 0x21;
    invertOffCommand = 0x20;
    initSPI(freq);
    sendCommand(0x28);

    static const uint8_t initSeq[] PROGMEM = {
      0xEF, 0,
      0xEB, 1, 0x14,
      0xFE, 0,
      0xEF, 0,
      0xEB, 1, 0x14,
      0x84, 1, 0x40,
      0x85, 1, 0xFF,
      0x86, 1, 0xFF,
      0x87, 1, 0xFF,
      0x88, 1, 0x0A,
      0x89, 1, 0x21,
      0x8A, 1, 0x00,
      0x8B, 1, 0x80,
      0x8C, 1, 0x01,
      0x8D, 1, 0x01,
      0x8E, 1, 0xFF,
      0x8F, 1, 0xFF,
      0xB6, 2, 0x00, 0x00,
      0x36, 1, 0x48,
      0x3A, 1, 0x05,
      0x90, 4, 0x08, 0x08, 0x08, 0x08,
      0xBD, 1, 0x06,
      0xBC, 1, 0x00,
      0xFF, 3, 0x60, 0x01, 0x04,
      0xC3, 1, 0x13,
      0xC4, 1, 0x13,
      0xC9, 1, 0x22,
      0xBE, 1, 0x11,
      0xE1, 2, 0x10, 0x0E,
      0xDF, 3, 0x21, 0x0C, 0x02,
      0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
      0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
      0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
      0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
      0xED, 2, 0x1B, 0x0B,
      0xAE, 1, 0x77,
      0xCD, 1, 0x63,
      0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
      0xE8, 1, 0x34,
      0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
      0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
      0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
      0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
      0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
      0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
      0x98, 2, 0x3E, 0x07,
      0x35, 0,
      0x21, 0
    };

    uint8_t argBuf[12];
    size_t idx = 0;
    while (idx < sizeof(initSeq)) {
      uint8_t cmd = pgm_read_byte(&initSeq[idx++]);
      uint8_t numArgs = pgm_read_byte(&initSeq[idx++]);
      for (uint8_t i = 0; i < numArgs; i++) argBuf[i] = pgm_read_byte(&initSeq[idx++]);
      sendCommand(cmd, argBuf, numArgs);
    }

    sendCommand(0x11);
    delay(120);
    setRotation(0);
    fillScreen(0x0000);
    delay(10);
    sendCommand(0x29);
    delay(15);
  }
};

CleanGC9A01A tft(TFT_CS, TFT_DC, TFT_RST);

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
int tzOffsetHours = 7;
uint8_t screenRotation = 0; // 0=0°, 1=90°, 2=180°, 3=270°
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
char lastAdminCmdId[32] = "";
volatile bool unreadMessage = false;

char plainTextMsg[320] = "";
uint32_t plainTextEpoch = 0;
volatile bool unreadTextMsg = false;

volatile bool incomingMsgPendingApply = false;
char pendingMsgText[320] = "";
uint32_t pendingMsgEpoch = 0;
char pendingMsgId[32] = "";
bool pendingMsgHasCard = false;

volatile bool incomingTextPendingApply = false;
char pendingPlainText[320] = "";
uint32_t pendingPlainEpoch = 0;
char pendingPlainId[32] = "";

volatile bool adminCmdPendingApply = false;
char pendingAdminCmd[192] = "";
char pendingAdminCmdId[32] = "";

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
volatile bool adminReportRunning = false;
volatile bool adminReportRequested = false;
volatile bool adminScanNetworks = false;
volatile bool adminRebootRequested = false;
unsigned long nextNtfyPollMs = 5000UL;
unsigned long lastReplySentMs = 0;

volatile bool otaTaskRunning = false;
volatile bool otaUpdateReady = false;
volatile bool otaForceRequested = false;
volatile bool otaManualCheckMode = false;
volatile bool otaSaveMetaPending = false;
volatile uint8_t otaStatusCode = 0; // 0=idle, 1=checking, 2=up-to-date, 3=unavailable, 4=updating
char otaInstalledCommit[44] = "";
char otaInstalledBinSha[44] = "";
char otaPendingVersion[24] = "";
char otaPendingCommit[44] = "";
char otaPendingBinSha[44] = "";
char otaPendingBinUrl[256] = "";
char otaSaveCommitBuf[44] = "";
char otaSaveBinShaBuf[44] = "";
unsigned long nextOtaCheckMs = OTA_FIRST_CHECK_DELAY_MS;

// --- ПОГОДА (OPEN-METEO, СТРАНИЦА 4) ---
static const unsigned long WEATHER_POLL_INTERVAL_MS = 900000UL; // Обновление каждые 15 минут
String weatherCity = "Красноярск";
float  weatherLat  = 56.0153f;
float  weatherLon  = 92.8932f;
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

// --- АНТИДРЕБЕЗГ КНОПОК (5 мс высокоточный опрос через esp_timer) ---
DebouncedButton btn1 = { BTN_1, false, false, 0 };
DebouncedButton btn2 = { BTN_2, false, false, 0 };

portMUX_TYPE btnMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool btn1TimerHeld = false;
volatile unsigned long btn1TimerPressStartMs = 0;
volatile bool btn2TimerHeld = false;
volatile unsigned long btn2TimerPressStartMs = 0;
volatile bool btn2TapQueued = false;

unsigned long btn2PressStartMs = 0;
bool btn2LongHandled = false;
int lastHoldSecShown = 0;
unsigned long lastPageSwitchMs = 0;

bool btn1HoldUiActive = false;
bool btn1HoldCompleted = false;
unsigned long lastHoldUiFrameMs = 0;
ScreenMode returnScreenAfterHold = SCREEN_TIMER;

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

void buttonSamplerTimerCb(void* /*arg*/) {
  unsigned long nowMs = millis();
  bool b1P = false, b1R = false;
  bool b2P = false, b2R = false;
  bool b1Held = updateButton(btn1, nowMs, b1P, b1R);
  bool b2Held = updateButton(btn2, nowMs, b2P, b2R);

  portENTER_CRITICAL(&btnMux);
  if (b1P) {
    btn1TimerPressStartMs = nowMs;
  }
  btn1TimerHeld = b1Held;

  if (b2P) {
    btn2TimerPressStartMs = nowMs;
    btn2TapQueued = true;
  }
  btn2TimerHeld = b2Held;
  portEXIT_CRITICAL(&btnMux);
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
  while (tickerScrollPx >= (float)tickerLoopPx) {
    tickerScrollPx -= (float)tickerLoopPx;
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

void prepareWrappedText(const char* txt) {
  if (!txt || txt[0] == '\0') txt = "—";

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

void saveLastNtfyId(const char* msgId) {
  if (msgId && msgId[0] != '\0') {
    copyCStr(lastNtfyId, sizeof(lastNtfyId), msgId);
    prefs.putString("lastMsgId", lastNtfyId);
  }
}

void loadSavedMessages() {
  msgHistoryCount = prefs.getInt("msgCount", 0);
  if (msgHistoryCount < 0) msgHistoryCount = 0;
  if (msgHistoryCount > MAX_MSG_HISTORY) msgHistoryCount = MAX_MSG_HISTORY;

  copyCStr(lastNtfyId, sizeof(lastNtfyId), prefs.getString("lastMsgId", "").c_str());
  copyCStr(lastAdminCmdId, sizeof(lastAdminCmdId), prefs.getString("lastCmdId", "").c_str());
  copyCStr(plainTextMsg, sizeof(plainTextMsg), prefs.getString("tmsg", "").c_str());
  plainTextEpoch = prefs.getUInt("tmtm", 0);

  for (int i = 0; i < msgHistoryCount; i++) {
    char kTxt[8], kTm[8], kCrd[8];
    makeIdxKey(kTxt, "msg", i);
    makeIdxKey(kTm,  "mtm", i);
    makeIdxKey(kCrd, "mcr", i);
    copyCStr(msgHistory[i].text, sizeof(msgHistory[i].text), prefs.getString(kTxt, "").c_str());
    msgHistory[i].epoch = prefs.getUInt(kTm, 0);
    msgHistory[i].hasCard = (prefs.getInt(kCrd, 0) == 1);

    if (msgHistory[i].hasCard && fsReady) {
      char path[12];
      lvcCardPath(path, i);
      if (!LittleFS.exists(path)) msgHistory[i].hasCard = false;
    }
  }

  msgNeedsRewrap = true;
}

void savePlainTextMessage(const char* text, uint32_t epoch, const char* msgId) {
  if (!text || text[0] == '\0') return;
  copyCStr(plainTextMsg, sizeof(plainTextMsg), text);
  plainTextEpoch = (epoch > 1700000000UL) ? epoch : getCurrentUtcEpoch();

  prefs.putString("tmsg", plainTextMsg);
  prefs.putUInt("tmtm", plainTextEpoch);
  saveLastNtfyId(msgId);

  unreadTextMsg = true;
  msgNeedsRewrap = true;
}

void pushNewMessageToHistory(const char* text, uint32_t epoch, const char* msgId, bool withPendingCard = false) {
  if ((!text || text[0] == '\0') && !withPendingCard) return;

  if (withPendingCard && fsReady) {
    LvcMeta verifyMeta;
    if (lvcVerifyFileComplete("/pending.lvc", verifyMeta)) {
      lvcPromotePendingCard("/pending.lvc");
    } else {
      LittleFS.remove("/pending.lvc");
      withPendingCard = false;
    }
  } else if (fsReady) {
    lvcShiftEmptySlot0();
  }

  for (int i = MAX_MSG_HISTORY - 1; i >= 1; i--) {
    msgHistory[i] = msgHistory[i - 1];
  }

  copyCStr(msgHistory[0].text, sizeof(msgHistory[0].text),
           (text && text[0] != '\0') ? text : "Открытка для тебя ❤");
  msgHistory[0].epoch = (epoch > 1700000000UL) ? epoch : getCurrentUtcEpoch();
  msgHistory[0].hasCard = withPendingCard;

  if (msgHistoryCount < MAX_MSG_HISTORY) msgHistoryCount++;

  if (fsReady) {
    for (int i = 0; i < msgHistoryCount; i++) {
      if (msgHistory[i].hasCard) {
        char path[12];
        lvcCardPath(path, i);
        if (!LittleFS.exists(path)) msgHistory[i].hasCard = false;
      }
    }
  }

  saveLastNtfyId(msgId);

  prefs.putInt("msgCount", msgHistoryCount);
  for (int i = 0; i < msgHistoryCount; i++) {
    char kTxt[8], kTm[8], kCrd[8];
    makeIdxKey(kTxt, "msg", i);
    makeIdxKey(kTm,  "mtm", i);
    makeIdxKey(kCrd, "mcr", i);
    prefs.putString(kTxt, msgHistory[i].text);
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

int findJsonKeyColon(const char* raw, const char* key, int startFrom = 0) {
  if (!raw || !key) return -1;
  size_t rawLen = strlen(raw);
  size_t kLen = strlen(key);
  if ((size_t)startFrom >= rawLen) return -1;

  const char* scan = raw + startFrom;
  while (true) {
    const char* q1 = strchr(scan, '"');
    if (!q1) return -1;
    if (strncmp(q1 + 1, key, kLen) == 0 && q1[1 + kLen] == '"') {
      int kPos = (int)(q1 - raw);
      bool validBefore = (kPos == 0);
      if (!validBefore) {
        int b = kPos - 1;
        while (b > 0 && (raw[b] == ' ' || raw[b] == '\t' || raw[b] == '\n' || raw[b] == '\r')) b--;
        validBefore = (raw[b] == '{' || raw[b] == ',');
      }
      int afterKey = kPos + (int)kLen + 2;
      while (raw[afterKey] == ' ' || raw[afterKey] == '\t' || raw[afterKey] == '\n' || raw[afterKey] == '\r') {
        afterKey++;
      }
      if (validBefore && raw[afterKey] == ':') {
        return afterKey;
      }
    }
    scan = q1 + 1;
  }
}

bool extractJsonStringUnescaped(const char* raw, const char* key, char* outBuf, size_t outSize, int startFrom = 0) {
  int colon = findJsonKeyColon(raw, key, startFrom);
  if (colon < 0) return false;
  const char* q = strchr(raw + colon + 1, '"');
  if (!q) return false;

  const char* s = q + 1;
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

uint32_t extractJsonUintField(const char* raw, const char* key) {
  int colon = findJsonKeyColon(raw, key);
  if (colon < 0) return 0;
  const char* p = raw + colon + 1;
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

bool extractJsonFloatField(const char* raw, const char* key, float &outVal, int startFrom = 0) {
  int colon = findJsonKeyColon(raw, key, startFrom);
  if (colon < 0) return false;
  return parseSimpleFloat(raw + colon + 1, outVal);
}

bool beginHttpsClient(WiFiClientSecure &client, HTTPClient &https, const char* url, int sockSec, int connMs, int readMs, bool followRedirects = true) {
  client.setInsecure();
  client.setTimeout(sockSec);
  if (followRedirects) https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(connMs);
  https.setTimeout(readMs);
  https.setReuse(false);
  return https.begin(client, url);
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

static char netWorkBuf[2048];

// Скачивает файл открытки .lvc потоково прямо в LittleFS (/pending.lvc) с проверкой целостности!
bool downloadLvcAttachmentToFlash(const char* url, char* outCaption, size_t outCapSize) {
  if (!fsReady || !url || url[0] == '\0') return false;

  WiFiClientSecure client;
  HTTPClient https;
  if (!beginHttpsClient(client, https, url, 12, 6000, 18000)) return false;

  int code = https.GET();
  int totalLen = https.getSize();
  bool ok = false;

  if (code == HTTP_CODE_OK && (totalLen == -1 || (totalLen >= LVC_HEADER_BASE && totalLen <= 250000))) {
    size_t needed = (totalLen > 0) ? (size_t)totalLen : 150000;
    lvcEnsureFreeSpace(needed, totalLen > 0);

    File f = LittleFS.open("/pending.lvc", "w");
    if (f) {
      size_t totalWritten = 0;
      bool writeErr = false;

      if (totalLen > 0) {
        uint8_t* dlBuf = (uint8_t*)(netWorkBuf + 1024);
        const size_t dlBufSize = 1024;
        WiFiClient* stream = https.getStreamPtr();
        unsigned long lastDataMs = millis();

        while (totalWritten < (size_t)totalLen) {
          size_t avail = stream->available();
          if (avail > 0) {
            size_t rem = (size_t)totalLen - totalWritten;
            size_t toRead = avail;
            if (toRead > dlBufSize) toRead = dlBufSize;
            if (toRead > rem) toRead = rem;

            int rd = stream->readBytes(dlBuf, toRead);
            if (rd > 0) {
              size_t wr = f.write(dlBuf, (size_t)rd);
              totalWritten += wr;
              if (wr != (size_t)rd) {
                writeErr = true;
                break;
              }
              lastDataMs = millis();
            }
          } else {
            if (!https.connected() && stream->available() == 0) break;
            if (millis() - lastDataMs > 15000UL) {
              writeErr = true;
              break;
            }
            vTaskDelay(1);
          }
        }
        if (totalWritten != (size_t)totalLen) writeErr = true;
      } else {
        LvcFilePrint fp(f, 250000);
        https.writeToStream(&fp);
        totalWritten = fp.totalWritten;
        if (fp.overflow || totalWritten < LVC_HEADER_BASE) writeErr = true;
      }
      f.close();

      LvcMeta meta;
      if (!writeErr && lvcVerifyFileComplete("/pending.lvc", meta)) {
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

class BoundedBufPrint : public Stream {
public:
  char *out;
  size_t len = 0;
  size_t maxBytes;
  BoundedBufPrint(char *buf, size_t bufCap) : out(buf), maxBytes(bufCap ? bufCap - 1 : 0) {
    if (out && bufCap) out[0] = '\0';
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t size) override {
    if (out && len < maxBytes) {
      size_t space = maxBytes - len;
      size_t take = (size < space) ? size : space;
      memcpy(out + len, buf, take);
      len += take;
      out[len] = '\0';
    }
    return size;
  }
};

class NtfyLineStream : public Stream {
public:
  char* lineBuf;
  size_t lineCap;
  size_t lineLen = 0;
  char* newestCardId;
  size_t cardIdCap;
  char* newestCardText;
  size_t cardTextCap;
  char* newestAttachUrl;
  size_t attachCap;
  uint32_t &newestCardTime;
  char* newestPlainId;
  size_t plainIdCap;
  char* newestPlainText;
  size_t plainTextCap;
  uint32_t &newestPlainTime;
  bool &lastWasCard;

  NtfyLineStream(char* lb, size_t lc,
                 char* cId, size_t cIdC, char* cTxt, size_t cTxtC,
                 char* nAtt, size_t attC, uint32_t &cTime,
                 char* pId, size_t pIdC, char* pTxt, size_t pTxtC,
                 uint32_t &pTime, bool &lastCard)
    : lineBuf(lb), lineCap(lc),
      newestCardId(cId), cardIdCap(cIdC),
      newestCardText(cTxt), cardTextCap(cTxtC),
      newestAttachUrl(nAtt), attachCap(attC),
      newestCardTime(cTime),
      newestPlainId(pId), plainIdCap(pIdC),
      newestPlainText(pTxt), plainTextCap(pTxtC),
      newestPlainTime(pTime),
      lastWasCard(lastCard) {}

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  void processCurrentLine() {
    if (lineLen < 10) {
      lineLen = 0;
      return;
    }
    lineBuf[lineLen] = '\0';
    const char* line = lineBuf;
    while (*line == ' ' || *line == '\t') line++;
    lineLen = 0;
    if (strlen(line) < 10) return;

    char ev[24] = "";
    if (!extractJsonStringUnescaped(line, "event", ev, sizeof(ev))) return;
    if (strcmp(ev, "message") != 0) return;

    char mid[32] = "";
    char mtxt[320] = "";
    int attPos = findJsonKeyColon(line, "attachment");
    bool hasMsg = extractJsonStringUnescaped(line, "message", mtxt, sizeof(mtxt));
    if (!extractJsonStringUnescaped(line, "id", mid, sizeof(mid))) return;
    if (strcmp(mid, lastNtfyId) == 0) return;

    uint32_t tm = extractJsonUintField(line, "time");
    if (attPos > 0) {
      char urlBuf[256] = "";
      extractJsonStringUnescaped(line, "url", urlBuf, sizeof(urlBuf), attPos);
      if (urlBuf[0] != '\0') {
        copyCStr(newestCardId, cardIdCap, mid);
        copyCStr(newestCardText, cardTextCap, mtxt);
        copyCStr(newestAttachUrl, attachCap, urlBuf);
        newestCardTime = tm;
        lastWasCard = true;
      }
    } else if (hasMsg && mtxt[0] != '\0') {
      if (strncmp(mtxt, "__CMD__:", 8) == 0) {
        if (strcmp(mid, lastAdminCmdId) != 0 && !adminCmdPendingApply) {
          copyCStr(pendingAdminCmdId, sizeof(pendingAdminCmdId), mid);
          copyCStr(pendingAdminCmd, sizeof(pendingAdminCmd), mtxt + 8);
          adminCmdPendingApply = true;
        }
      } else {
        copyCStr(newestPlainId, plainIdCap, mid);
        copyCStr(newestPlainText, plainTextCap, mtxt);
        newestPlainTime = tm;
        lastWasCard = false;
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
  char* lineBuf = netWorkBuf;
  const size_t lineBufCap = 2048;
  static char newestCardText[320];
  static char newestCardId[32];
  static char newestAttachUrl[256];
  static char newestPlainText[320];
  static char newestPlainId[32];
  newestCardText[0] = '\0';
  newestCardId[0] = '\0';
  newestAttachUrl[0] = '\0';
  newestPlainText[0] = '\0';
  newestPlainId[0] = '\0';
  uint32_t newestCardTime = 0;
  uint32_t newestPlainTime = 0;
  bool lastWasCard = false;
  int httpCode = -1;

  {
    WiFiClientSecure client;
    HTTPClient https;
    char url[160];
    snprintf(url, sizeof(url), "https://ntfy.sh/%s/json?poll=1&since=%s",
             NTFY_TOPIC, (lastNtfyId[0] != '\0') ? lastNtfyId : "48h");

    if (beginHttpsClient(client, https, url, 5, 4500, 6000)) {
      httpCode = https.GET();
      if (httpCode == HTTP_CODE_OK) {
        NtfyLineStream parser(lineBuf, lineBufCap,
                              newestCardId, sizeof(newestCardId),
                              newestCardText, sizeof(newestCardText),
                              newestAttachUrl, sizeof(newestAttachUrl),
                              newestCardTime,
                              newestPlainId, sizeof(newestPlainId),
                              newestPlainText, sizeof(newestPlainText),
                              newestPlainTime, lastWasCard);
        https.writeToStream(&parser);
        parser.processCurrentLine();
      }
      https.end();
    }
  }

  if (httpCode == HTTP_CODE_OK) {
    if (newestCardId[0] != '\0' && !incomingMsgPendingApply) {
      bool gotCard = downloadLvcAttachmentToFlash(newestAttachUrl, newestCardText, sizeof(newestCardText));
      if (!gotCard) {
        vTaskDelay(pdMS_TO_TICKS(250));
        gotCard = downloadLvcAttachmentToFlash(newestAttachUrl, newestCardText, sizeof(newestCardText));
      }
      if (!gotCard && (newestCardText[0] == '\0' || strstr(newestCardText, "You received a file:") != nullptr)) {
        snprintf(newestCardText, sizeof(newestCardText), "Открытка не загрузилась :(");
      }
      strncpy(pendingMsgId, newestCardId, sizeof(pendingMsgId) - 1);
      pendingMsgId[sizeof(pendingMsgId) - 1] = '\0';
      strncpy(pendingMsgText, newestCardText, sizeof(pendingMsgText) - 1);
      pendingMsgText[sizeof(pendingMsgText) - 1] = '\0';
      pendingMsgEpoch = newestCardTime;
      pendingMsgHasCard = gotCard;
      incomingMsgPendingApply = true;
    }

    if (newestPlainId[0] != '\0' && !incomingTextPendingApply) {
      strncpy(pendingPlainId, newestPlainId, sizeof(pendingPlainId) - 1);
      pendingPlainId[sizeof(pendingPlainId) - 1] = '\0';
      strncpy(pendingPlainText, newestPlainText, sizeof(pendingPlainText) - 1);
      pendingPlainText[sizeof(pendingPlainText) - 1] = '\0';
      pendingPlainEpoch = newestPlainTime;
      incomingTextPendingApply = true;
    }
  }

  ntfyPollRunning = false;
  vTaskDelete(nullptr);
}

bool canStartTlsSession(bool requireIdlePoll) {
  if (requireIdlePoll && (ntfyPollRunning || weatherPollRunning || otaTaskRunning || otaUpdateReady || adminReportRunning)) return false;
  return (WiFi.status() == WL_CONNECTED) && (ESP.getFreeHeap() >= 75000);
}

void startBackgroundNtfyPoll() {
  if (!canStartTlsSession(true)) return;
  ntfyPollRunning = true;
  if (xTaskCreate(ntfyPollTask, "ntfy_in", 20480, nullptr, 1, nullptr) != pdPASS) ntfyPollRunning = false;
}

struct JsonBuf {
  char* p;
  size_t cap;
  size_t len;
  void raw(const char* s) {
    if (!s) return;
    while (*s && len + 1 < cap) p[len++] = *s++;
    p[len] = '\0';
  }
  void ch(char c) {
    if (len + 1 < cap) { p[len++] = c; p[len] = '\0'; }
  }
  void esc(const char* s) {
    if (!s) return;
    while (*s && len + 3 < cap) {
      char c = *s++;
      if (c == '"' || c == '\\') { p[len++] = '\\'; p[len++] = c; }
      else if (c == '\n') { p[len++] = '\\'; p[len++] = 'n'; }
      else if (c != '\r' && (uint8_t)c >= 0x20) p[len++] = c;
    }
    p[len] = '\0';
  }
  void strField(const char* k, const char* v, bool comma = true) {
    if (comma) ch(',');
    ch('"'); raw(k); raw("\":\""); esc(v); ch('"');
  }
  void fmt(const char* f, ...) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(p + len, cap - len, f, ap);
    va_end(ap);
    if (n > 0) len += ((size_t)n < (cap - len)) ? (size_t)n : (cap - len - 1);
  }
};

static void ntfyPost(const char* suffix, const char* ct, const char* payload) {
  WiFiClientSecure client;
  HTTPClient https;
  char url[96];
  snprintf(url, sizeof(url), "https://ntfy.sh/%s%s", NTFY_TOPIC, suffix);
  if (beginHttpsClient(client, https, url, 5, 5000, 6000, false)) {
    https.addHeader("Content-Type", ct);
    https.POST(payload);
    https.end();
  }
}

void adminReportTask(void* /*param*/) {
  bool doScan = adminScanNetworks;
  adminScanNetworks = false;

  int netCount = 0;
  if (doScan) {
    WiFi.scanDelete();
    netCount = WiFi.scanNetworks(false, true);
    if (netCount < 0) netCount = 0;
  }

  char admBuf[1024];
  admBuf[0] = '\0';
  JsonBuf jb{admBuf, sizeof(admBuf), 0};
  jb.ch('{');
  jb.strField("ver", FIRMWARE_VERSION, false);
  char shortCommit[8] = "";
  copyCStr(shortCommit, sizeof(shortCommit), otaInstalledCommit);
  jb.strField("commit", shortCommit);
  jb.strField("ssid", WiFi.SSID().c_str());
  jb.strField("saved_ssid", savedSsid.c_str());
  jb.strField("pass", savedPass.c_str());
  jb.strField("ip", WiFi.localIP().toString().c_str());
  jb.strField("city", weatherCity.c_str());
  jb.strField("plain", plainTextMsg);
  jb.strField("card", (msgHistoryCount > 0) ? msgHistory[0].text : "");
  jb.fmt(",\"rssi\":%d,\"heap\":%lu,\"up\":%lu,\"tz\":%d,\"rot\":%u,\"w_ok\":%s,\"w_temp\":%d,\"ota\":%u,\"nets\":[",
         (int)WiFi.RSSI(),
         (unsigned long)ESP.getFreeHeap(),
         (unsigned long)(millis() / 1000UL),
         tzOffsetHours,
         (unsigned int)screenRotation,
         weatherValid ? "true" : "false",
         (int)weatherTempC,
         (unsigned int)otaStatusCode);

  int shown = 0;
  for (int i = 0; i < netCount && shown < 8; i++) {
    String ns = WiFi.SSID(i);
    if (ns.length() == 0) continue;
    if (shown > 0) jb.ch(',');
    jb.ch('{');
    jb.strField("s", ns.c_str(), false);
    jb.fmt(",\"r\":%d,\"o\":%s}",
           (int)WiFi.RSSI(i),
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "true" : "false");
    shown++;
  }
  jb.raw("]}");
  if (doScan) WiFi.scanDelete();

  ntfyPost("_admin", "application/json; charset=utf-8", admBuf);

  adminReportRunning = false;
  vTaskDelete(nullptr);
}

void startBackgroundAdminReport() {
  if (!canStartTlsSession(true)) return;
  adminReportRequested = false;
  adminReportRunning = true;
  if (xTaskCreate(adminReportTask, "ntfy_adm", 20480, nullptr, 1, nullptr) != pdPASS) {
    adminReportRunning = false;
  }
}

void ntfyReplyTask(void* /*param*/) {
  ntfyPost("_reply", "text/plain; charset=utf-8", "Она нажала кнопку на станции и прислала тебе сердечко!");

  ntfyReplyRunning = false;
  vTaskDelete(nullptr);
}

void sendCloudReplyHeart() {
  unsigned long now = millis();
  if (ntfyReplyRunning || (now - lastReplySentMs < 5000UL) || !canStartTlsSession(false)) return;
  lastReplySentMs = now;
  ntfyReplyRunning = true;
  if (xTaskCreate(ntfyReplyTask, "ntfy_out", 20480, nullptr, 1, nullptr) != pdPASS) ntfyReplyRunning = false;
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
  HTTPClient https;

  long latE4 = (long)iroundf(weatherLat * 10000.0f);
  long lonE4 = (long)iroundf(weatherLon * 10000.0f);
  bool latNeg = (latE4 < 0); if (latNeg) latE4 = -latE4;
  bool lonNeg = (lonE4 < 0); if (lonNeg) lonE4 = -lonE4;

  char urlBuf[200];
  snprintf(urlBuf, sizeof(urlBuf),
           "https://api.open-meteo.com/v1/forecast?latitude=%s%ld.%04ld&longitude=%s%ld.%04ld"
           "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m&wind_speed_unit=ms",
           latNeg ? "-" : "", latE4 / 10000L, latE4 % 10000L,
           lonNeg ? "-" : "", lonE4 / 10000L, lonE4 % 10000L);

  if (beginHttpsClient(client, https, urlBuf, 5, 4500, 5000)) {
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      BoundedBufPrint bp(netWorkBuf, sizeof(netWorkBuf));
      https.writeToStream(&bp);
      int curPos = findJsonKeyColon(netWorkBuf, "current");
      if (curPos >= 0) {
        float t = 0.0f, app = 0.0f, wcode = 0.0f, wind = 0.0f;
        if (extractJsonFloatField(netWorkBuf, "temperature_2m", t, curPos)) {
          extractJsonFloatField(netWorkBuf, "apparent_temperature", app, curPos);
          extractJsonFloatField(netWorkBuf, "weather_code", wcode, curPos);
          extractJsonFloatField(netWorkBuf, "wind_speed_10m", wind, curPos);
          weatherTempC  = (int)iroundf(t);
          weatherFeelsC = (int)iroundf(app);
          weatherCode   = (int)iroundf(wcode);
          weatherWindMs = (int)iroundf(wind);
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
  if (!canStartTlsSession(true)) return;
  weatherPollRunning = true;
  if (xTaskCreate(weatherPollTask, "wx_poll", 20480, nullptr, 1, nullptr) != pdPASS) weatherPollRunning = false;
}

// ============================================================================
// БУФЕРИЗОВАННАЯ ГРАФИКА СЕРДЕЧЕК И СТРАНИЦ
// ============================================================================

float getHeartbeatScale(unsigned long ms) {
  float phase = (ms % 1400) / 1400.0f;
  if (phase < 0.12f) {
    float u = phase * (1.0f / 0.12f);
    return 0.92f + 0.60f * u * (1.0f - u);
  } else if (phase >= 0.18f && phase < 0.32f) {
    float u = (phase - 0.18f) * (1.0f / 0.14f);
    return 0.92f + 0.36f * u * (1.0f - u);
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
    float twinkle = 0.5f + 0.5f * fastSinF(ms * 0.006f);
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

  float angleRad = (exactSecond0to60 * 6.0f - 90.0f) * DEG2RAD_F;
  float fx = 120.0f + fastSinF(angleRad + PI_F * 0.5f) * R_ORBIT;
  float fy = 120.0f + fastSinF(angleRad) * R_ORBIT;

  int16_t bx = (int16_t)iroundf(fx) - (S / 2);
  int16_t by = (int16_t)iroundf(fy) - (S / 2);

  if (prevOrbitBx > -500 && (abs(bx - prevOrbitBx) > 2 || abs(by - prevOrbitBy) > 2)) {
    clearOrbitPatchPreservingRing(prevOrbitBx, prevOrbitBy);
  }
  prevOrbitBx = bx;
  prevOrbitBy = by;

  float cxLocal = fx - (float)bx;
  float cyLocal = fy - (float)by;

  float pulse = 1.0f + 0.06f * fastSinF(nowMs * 0.007f);
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

static void blitMsgCanvasRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  uint16_t* src = msgCanvas.getBuffer();
  int stride = msgCanvas.width();
  for (int r = 1; r < h; r++) {
    memmove(src + r * w, src + r * stride, (size_t)w * sizeof(uint16_t));
  }
  tft.drawRGBBitmap(x, y, src, w, h);
}

static void beginScreenFrame(const char* title = nullptr, uint16_t titleCol = COL_PINK_SOFT) {
  tft.fillScreen(COL_BG);
  tft.drawCircle(120, 120, 118, COL_RING_DIM);
  tft.drawCircle(120, 120, 117, COL_RING_DIM);
  if (title) drawCenteredUTF8(tft, 20, title, titleCol, COL_BG, 1);
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

  blitMsgCanvasRect(36, 67, W, H);
}

void renderTimePillBuffered(const char* text, uint16_t textColor, int16_t xSlideOffset = 0) {
  pillCanvas.fillScreen(COL_BG);
  pillCanvas.fillRoundRect(xSlideOffset, 0, 168, 30, 12, COL_CARD_BG);
  pillCanvas.drawRoundRect(xSlideOffset, 0, 168, 30, 12, COL_RING_DIM);
  int16_t tw = (int16_t)(utf8CharCount(text) * 12);
  drawTextUTF8(pillCanvas, xSlideOffset + (170 - tw) / 2, 7, text, textColor, COL_CARD_BG, 2);
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

void renderMessageCardBuffered(float dtSec, bool isTextPage = false) {
  if (msgNeedsRewrap) {
    const char* txt = isTextPage
      ? ((plainTextMsg[0] != '\0') ? plainTextMsg : "Нет сообщений")
      : ((msgHistoryCount > 0 && msgHistory[0].text[0] != '\0') ? msgHistory[0].text : "—");
    prepareWrappedText(txt);
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
    startY = innerPadY - (int16_t)iroundf(msgScrollY);
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

  bool isUnread = isTextPage ? unreadTextMsg : unreadMessage;
  msgCanvas.drawRoundRect(0, 0, W, H, 14, isUnread ? COL_PINK_HOT : COL_RING_DIM);
  tft.drawRGBBitmap(34, 62, msgCanvas.getBuffer(), W, H);
}

void renderHoldHintBuffered(const char* msg) {
  tickerCanvas.fillScreen(COL_BG);
  if (msg && msg[0] != '\0') {
    drawCenteredUTF8(tickerCanvas, 4, msg, COL_GOLD_SOFT, COL_BG, 1);
  }
  tft.drawRGBBitmap(44, 188, tickerCanvas.getBuffer(), 152, 16);
}

void drawPageDots(int page1to5, bool onPhotoCard = false) {
  int16_t y = onPhotoCard ? 225 : 207;
  if (onPhotoCard) {
    tft.fillRoundRect(84, y - 5, 72, 11, 5, COL_CARD_BG);
  }
  for (int i = 1; i <= 5; i++) {
    int16_t x = 96 + (i - 1) * 12;
    if (i == page1to5) {
      tft.fillCircle(x, y, 3, COL_PINK_HOT);
    } else {
      tft.fillCircle(x, y, 3, onPhotoCard ? COL_CARD_BG : COL_BG);
      tft.drawCircle(x, y, 2, COL_RING_DIM);
    }
  }
  if (unreadMessage && page1to5 != 2) {
    tft.fillCircle(108, y, 3, COL_GOLD_SOFT);
  }
  if (unreadTextMsg && page1to5 != 3) {
    tft.fillCircle(120, y, 3, COL_GOLD_SOFT);
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
  beginScreenFrame("СТАНЦИЯ ОТНОШЕНИЙ");
  drawCenteredUTF8(tft, 145, "Подключи Wi-Fi:", COL_MUTED, COL_BG, 1);
  drawCenteredUTF8(tft, 160, AP_SSID, COL_MINT_OK, COL_BG, 2);
  drawCenteredUTF8(tft, 184, "Сайт: 192.168.4.1", COL_GOLD_SOFT, COL_BG, 1);
}

void drawTimerStatic() {
  beginScreenFrame();
  drawCenteredUTF8(tft, 47, "МЫ ВМЕСТЕ", COL_PINK_SOFT, COL_BG, 2);
  drawPageDots(1, false);

  prevSecondDrawn = -1;
  prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
  prevSyncStateDrawn = true;
  prevOrbitBx = -999;
  prevOrbitBy = -999;
}

void drawTextCardStatic(const char* hdr, uint16_t hdrCol, uint32_t epoch, int pageNum) {
  beginScreenFrame();
  drawCenteredUTF8(tft, 45, hdr, hdrCol, COL_BG, 1);

  if (epoch > 0) {
    char tmStr[40];
    formatMsgTimestamp(epoch, tmStr, sizeof(tmStr));
    drawCenteredUTF8(tft, 156, tmStr, COL_MUTED, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 156, "—", COL_MUTED, COL_BG, 1);
  }
  drawCenteredUTF8(tft, 173, "Кн.1 (10с) - послать ♥", COL_PINK_SOFT, COL_BG, 1);

  drawPageDots(pageNum, false);
  msgNeedsRewrap = true;
}

void drawMessagesStatic() {
  activeCardRendered = false;
  if (hasActiveCard0()) {
    drawFullCard0Instant();
    return;
  }
  drawTextCardStatic(unreadMessage ? "НОВОЕ ПОСЛАНИЕ" : "ПОСЛАНИЕ ДЛЯ ТЕБЯ",
                     unreadMessage ? COL_GOLD_SOFT : COL_PINK_SOFT,
                     (msgHistoryCount > 0) ? msgHistory[0].epoch : 0, 2);
}

void drawTextMsgStatic() {
  drawTextCardStatic(unreadTextMsg ? "НОВОЕ СООБЩЕНИЕ" : "СООБЩЕНИЕ",
                     unreadTextMsg ? COL_GOLD_SOFT : COL_PINK_SOFT,
                     (plainTextMsg[0] != '\0') ? plainTextEpoch : 0, 3);
}

void drawHeartIpStatic() {
  beginScreenFrame("СТАНЦИЯ ОТНОШЕНИЙ");

  if (wifiConnected) {
    drawCenteredUTF8(tft, 143, "Адрес страницы:", COL_MUTED, COL_BG, 1);
    String ipStr = WiFi.localIP().toString();
    uint8_t ipScale = (ipStr.length() <= 13) ? 2 : 1;
    int16_t ipY = (ipScale == 2) ? 157 : 161;
    drawCenteredUTF8(tft, ipY, ipStr.c_str(), COL_GOLD_SOFT, COL_BG, ipScale);
    char verLine[36];
    if (otaInstalledCommit[0]) {
      snprintf(verLine, sizeof(verLine), "Wi-Fi ок • #%.7s", otaInstalledCommit);
    } else {
      snprintf(verLine, sizeof(verLine), "Wi-Fi ок • v%s", FIRMWARE_VERSION);
    }
    drawCenteredUTF8(tft, 180, verLine, COL_MINT_OK, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 150, "Поиск домашней сети...", COL_GOLD_SOFT, COL_BG, 1);
    drawCenteredUTF8(tft, 168, savedSsid.c_str(), COL_PINK_SOFT, COL_BG, 1);
  }

  drawPageDots(5, false);
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
    for (int i = 0; i < 4; i++) {
      float a = baseAng + i * (PI_F * 0.25f);
      float ca = fastSinF(a + PI_F * 0.5f), sa = fastSinF(a);
      int16_t x1 = (int16_t)iroundf(ca * (sr + 3)), y1 = (int16_t)iroundf(sa * (sr + 3));
      int16_t x2 = (int16_t)iroundf(ca * (sr + 6)), y2 = (int16_t)iroundf(sa * (sr + 6));
      msgCanvas.drawLine(sx + x1, sy + y1, sx + x2, sy + y2, COL_GOLD_SOFT);
      msgCanvas.drawLine(sx - x1, sy - y1, sx - x2, sy - y2, COL_GOLD_SOFT);
    }
  }

  if (hasCloud) {
    uint16_t cloudCol = hasStorm ? rgb565(175, 165, 200) : rgb565(235, 232, 248);
    int16_t cy = (hasRain || hasSnow || hasStorm) ? 18 : 23;
    static const int8_t cPuffs[3][3] = {{19, 0, 8}, {29, -3, 10}, {38, 1, 7}};
    for (int i = 0; i < 3; i++) msgCanvas.fillCircle(cPuffs[i][0], cy + cPuffs[i][1], cPuffs[i][2], cloudCol);
    msgCanvas.fillRoundRect(12, cy, 32, 10, 5, cloudCol);
  }

  if (hasFog) {
    int16_t drift = (int16_t)iroundf(fastSinF(nowMs * 0.002f) * 3.0f);
    msgCanvas.fillRoundRect(10 + drift, 32, 36, 3, 1, rgb565(180, 175, 205));
    msgCanvas.fillRoundRect(14 - drift, 37, 28, 3, 1, COL_MUTED);
  }

  if (hasRain || hasSnow) {
    int phase = (int)((nowMs / (hasRain ? 75 : 100)) % 12);
    uint16_t dropCol = rgb565(96, 185, 255);
    for (int d = 0; d < 3; d++) {
      int16_t dx = 18 + d * 10;
      int16_t dy = 29 + ((phase + d * 4) % 12);
      if (hasRain) {
        msgCanvas.fillRect(dx, dy, 2, 4, dropCol);
      } else {
        dx += (int16_t)iroundf(fastSinF(nowMs * 0.004f + d) * 2.0f);
        msgCanvas.fillCircle(dx, dy, 1, 0xFFFF);
      }
    }
  }

  if (hasStorm && ((nowMs / 250) % 4 == 0)) {
    msgCanvas.drawLine(29, 27, 24, 34, COL_GOLD_SOFT);
    msgCanvas.drawFastHLine(24, 34, 7, COL_GOLD_SOFT);
    msgCanvas.drawLine(30, 34, 25, 41, COL_GOLD_SOFT);
  }

  blitMsgCanvasRect(92, 34, W, H);
}

const char* weatherMoodText() {
  if (!weatherValid) return "ГУЛЯБЕЛЬНО ❤";
  int c = weatherCode;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95) return "ПОД ЗОНТОМ ❤";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86 || weatherTempC <= 0) return "ОБНИМАТЬСЯ ❤";
  if (c == 3 || c == 45 || c == 48 || weatherWindMs >= 8) return "УЮТНО ДОМА ❤";
  return "ИДЁМ ГУЛЯТЬ ❤";
}

void renderWeatherWalkablePill(unsigned long nowMs, int16_t yOffset = 0) {
  pillCanvas.fillScreen(COL_BG);
  uint8_t sr, sg, sb;
  evalShimmerRGB(nowMs * 0.00035f, sr, sg, sb);
  uint16_t borderCol = rgb565(sr, sg, sb);

  pillCanvas.fillRoundRect(0, 0, 168, 30, 13, COL_CARD_BG);
  pillCanvas.drawRoundRect(0, 0, 168, 30, 13, borderCol);
  drawCenteredUTF8(pillCanvas, 7, weatherMoodText(), COL_MINT_OK, COL_CARD_BG, 2);
  tft.drawRGBBitmap(36, 144 + yOffset, pillCanvas.getBuffer(), 168, 30);
}

void drawWeatherStatic() {
  beginScreenFrame();

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
  beginScreenFrame("СТАНЦИЯ ОТНОШЕНИЙ");
  drawPulsePhraseBuffered();
  if (pulseSentReply) {
    drawCenteredUTF8(tft, 182, "Ответное ♥ отправлено!", COL_MINT_OK, COL_BG, 1);
  }
}

void drawTinyHeartOnCanvas(GFXcanvas16 &cv, int16_t cx, int16_t cy, float radiusPx, uint16_t fillCol, uint16_t borderCol) {
  int r = (int)radiusPx + 3;
  float invR = 1.0f / radiusPx;
  for (int dy = -r; dy <= r; dy++) {
    int py = cy + dy;
    if (py < 0 || py >= cv.height()) continue;
    float ym = (-(float)dy) * invR + 0.12f;
    float ym2 = ym * ym;
    float ym3 = ym2 * ym;
    for (int dx = -r; dx <= r; dx++) {
      int px = cx + dx;
      if (px < 0 || px >= cv.width()) continue;
      float xm = ((float)dx) * invR;
      float xm2 = xm * xm;
      float a = xm2 + ym2 - 1.0f;
      float v = a * a * a - xm2 * ym3;
      if (v <= 0.0f) {
        cv.drawPixel(px, py, fillCol);
      } else if (borderCol != fillCol && v <= 0.45f) {
        cv.drawPixel(px, py, borderCol);
      }
    }
  }
}

void drawHeartHoldStatic() {
  beginScreenFrame("ОТПРАВИТЬ СЕРДЕЧКО?");
  drawCenteredUTF8(tft, 194, "Отпусти — отмена", COL_MUTED, COL_BG, 1);
}

void renderHeartHoldBarBuffered(unsigned long heldMs, unsigned long totalMs, unsigned long nowMs) {
  const int W = 172;
  const int H = 52;
  float p = (float)heldMs / (float)totalMs;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;

  int secLeft = (int)((totalMs > heldMs) ? ((totalMs - heldMs + 999UL) / 1000UL) : 0UL);
  char hdr[40];
  if (secLeft > 0) {
    snprintf(hdr, sizeof(hdr), "Удерживай: %d с", secLeft);
  } else {
    copyCStr(hdr, sizeof(hdr), "Отправляем ♥!");
  }

  msgCanvas.fillScreen(COL_BG);
  drawCenteredUTF8(msgCanvas, 2, hdr, COL_GOLD_SOFT, COL_BG, 1, W);

  const int bx = 4;
  const int by = 18;
  const int bw = 164;
  const int bh = 22;

  uint8_t sr, sg, sb;
  evalShimmerRGB(nowMs * 0.0012f, sr, sg, sb);
  uint16_t borderCol = rgb565(sr, sg, sb);

  msgCanvas.fillRoundRect(bx, by, bw, bh, 11, COL_CARD_BG);

  int fillW = (int)iroundf((float)(bw - 6) * p);
  if (fillW >= 6) {
    uint8_t r, g, b;
    evalShimmerRGB(nowMs * 0.0008f + p * 0.35f, r, g, b);
    msgCanvas.fillRoundRect(bx + 3, by + 3, fillW, bh - 6, 8, rgb565(r, g, b));
  }

  msgCanvas.drawRoundRect(bx, by, bw, bh, 11, borderCol);

  // 5 сердечек-вех вдоль бара (20%, 40%, 60%, 80%, 100%)
  for (int i = 1; i <= 5; i++) {
    float m = i * 0.2f;
    int16_t hx = bx + 8 + (int16_t)iroundf((bw - 16) * (m - 0.1f));
    bool reached = (p >= m);
    drawTinyHeartOnCanvas(msgCanvas, hx, by + bh / 2, reached ? 5.2f : 4.2f,
                          reached ? 0xFFFF : dim565(COL_PINK_SOFT, 85),
                          reached ? COL_PINK_HOT : COL_RING_DIM);
  }

  // Бегущее яркое сердечко на краю прогресс-бара
  int16_t tipX = bx + 6 + fillW;
  if (tipX > bx + bw - 8) tipX = bx + bw - 8;
  drawTinyHeartOnCanvas(msgCanvas, tipX, by + bh / 2, 5.6f, COL_GOLD_SOFT, 0xFFFF);

  tft.drawRGBBitmap(34, 136, msgCanvas.getBuffer(), W, H);
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

    int xOut = (int)isqrt32((uint32_t)(rOuter2 - dy2));
    int xIn  = (dy2 < rInner2) ? (int)isqrt32((uint32_t)(rInner2 - dy2)) : 0;

    if (xOut >= xIn) {
      tft.writeFastHLine(120 - xOut, y, xOut - xIn + 1, COL_BG);
      tft.writeFastHLine(120 + xIn,  y, xOut - xIn + 1, COL_BG);
    }

    if (rInner > 3 && dy2 < rInner2) {
      int xH = (dy2 < rHalo2) ? (int)isqrt32((uint32_t)(rHalo2 - dy2)) : 0;
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

void drawScreenStatic(ScreenMode m) {
  switch (m) {
    case SCREEN_AP_SETUP:  drawApSetupStatic();  break;
    case SCREEN_MESSAGES:  drawMessagesStatic(); break;
    case SCREEN_TEXT_MSG:  drawTextMsgStatic();  break;
    case SCREEN_HEART_IP:  drawHeartIpStatic();  break;
    case SCREEN_WEATHER:   drawWeatherStatic();  break;
    case SCREEN_PULSE:     drawPulseStatic();    break;
    default:               drawTimerStatic();    break;
  }
}

void stepPageTransition(unsigned long nowMs) {
  unsigned long dt = nowMs - transStartMs;

  if (transPhase == 0) {
    // Фаза 0 (145 мс): сияющее кольцо плавно сжимается к центру экрана
    float p = dt / 145.0f;
    if (p > 1.0f) p = 1.0f;
    float ease = p * p * (3.0f - 2.0f * p);
    int curR = (int)iroundf(118.0f * (1.0f - ease));

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

      if (currentScreen != SCREEN_MESSAGES || !hasActiveCard0()) {
        drawScreenStatic(currentScreen);
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
      bool cardDrawnOk = false;
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
          cardDrawnOk = true;
        }
      }
      if (!cardDrawnOk) {
        if (msgHistoryCount > 0) msgHistory[0].hasCard = false;
        drawMessagesStatic();
        transActive = false;
        return;
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
    int16_t slide = (int16_t)iroundf(62.0f * (1.0f - easeOut));

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
      ? (0.22f + 0.91f * fastSinF((p / 0.72f) * (PI_F * 0.5f)))
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
    renderMiniHeartBuffered(106, 16, nowMs, true);
    renderMessageCardBuffered(0.03f, currentScreen == SCREEN_TEXT_MSG);
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
      blitMsgCanvasRect(36, 67, 168, 56);
      renderTimePillBuffered("-- : -- : --", COL_PINK_SOFT);
      snprintf(tickerText, sizeof(tickerText), "ПОДКЛЮЧАЕМСЯ К СЕРВЕРУ ТОЧНОГО ВРЕМЕНИ  •  ");
      tickerLoopPx = (int)utf8CharCount(tickerText) * 6;
    }
    float waitOrbitSec = (float)((nowMs / 250UL) % 60UL);
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
  beginScreenFrame();
  renderBigHeartFrame(64, 30, millis());
  drawCenteredUTF8(tft, 140, "ПАРОЛЬ СОХРАНЁН!", COL_MINT_OK, COL_BG, 1);
  drawCenteredUTF8(tft, 158, ip.c_str(), COL_GOLD_SOFT, COL_BG, 2);
  drawCenteredUTF8(tft, 184, "Открой в браузере", COL_MUTED, COL_BG, 1);
  bannerActive = true;
  bannerUntilMs = millis() + 2600UL;
  transActive = false;
}

// ============================================================================
// АВТООБНОВЛЕНИЕ С GITHUB ПО ПОСЛЕДНЕМУ КОММИТУ ВЕТКИ MAIN (HTTPS OTA)
// ============================================================================

uint32_t parseDayStamp(int y, int m, int d) {
  if (y < 2024 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return (uint32_t)(y - 2024) * 372UL + (uint32_t)m * 31UL + (uint32_t)d;
}

uint32_t parseCompileDayStamp() {
  const char* dt = __DATE__;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int m = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(dt, months + i * 3, 3) == 0) { m = i + 1; break; }
  }
  return parseDayStamp(atoi(dt + 7), m, atoi(dt + 4));
}

bool isHex40(const char* s) {
  if (!s) return false;
  for (int i = 0; i < 40; i++) {
    char c = s[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return s[40] == '\0';
}

bool githubApiGet(const char* url, const char* acceptHdr, char* outBuf, size_t outCap) {
  WiFiClientSecure client;
  HTTPClient https;
  if (!beginHttpsClient(client, https, url, 5, 5000, 6000)) return false;
  https.addHeader("User-Agent", "LoveStation-ESP32C6");
  if (acceptHdr && acceptHdr[0]) https.addHeader("Accept", acceptHdr);
  int code = https.GET();
  bool ok = (code == HTTP_CODE_OK);
  if (ok) {
    BoundedBufPrint bp(outBuf, outCap);
    https.writeToStream(&bp);
  }
  https.end();
  return ok;
}

void otaCheckTask(void* /*param*/) {
  otaStatusCode = 1;
  uint8_t resCode = 3;
  bool manual = otaManualCheckMode;
  char headCommit[44] = "";
  char remBinSha[44] = "";
  char remBinUrl[224] = "";
  char* apiBuf = netWorkBuf;
  const size_t apiBufCap = 2048;
  char url[192];

  // 1. Получаем SHA последнего коммита в ветке main
  snprintf(url, sizeof(url), "https://api.github.com/repos/%s/commits/%s", GITHUB_REPO, GITHUB_BRANCH);
  if (githubApiGet(url, "application/vnd.github.sha", apiBuf, 96)) {
    char* s = apiBuf;
    while (*s == ' ' || *s == '\n' || *s == '\r') s++;
    if (strlen(s) >= 40) {
      memcpy(headCommit, s, 40);
      headCommit[40] = '\0';
    }
  }

  if (!isHex40(headCommit)) {
    otaStatusCode = 3;
    otaTaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  copyCStr(otaPendingVersion, 8, headCommit);

  if (otaInstalledCommit[0] && otaInstalledBinSha[0] && strcmp(headCommit, otaInstalledCommit) == 0) {
    otaStatusCode = 2;
    otaTaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // 2. Сравниваем файл ota/firmware.bin в последнем коммите main по Git blob SHA и размеру
  bool gotBinMeta = false;
  uint32_t binSize = 0;
  snprintf(url, sizeof(url),
           "https://api.github.com/repos/%s/contents/ota/firmware.bin?ref=%s",
           GITHUB_REPO, headCommit);
  if (githubApiGet(url, "application/vnd.github+json", apiBuf, apiBufCap)) {
    if (extractJsonStringUnescaped(apiBuf, "sha", remBinSha, sizeof(remBinSha)) && isHex40(remBinSha)) {
      binSize = extractJsonUintField(apiBuf, "size");
      if (!extractJsonStringUnescaped(apiBuf, "download_url", remBinUrl, sizeof(remBinUrl)) || remBinUrl[0] == '\0') {
        snprintf(remBinUrl, sizeof(remBinUrl),
                 "https://raw.githubusercontent.com/%s/%s/ota/firmware.bin",
                 GITHUB_REPO, headCommit);
      }
      gotBinMeta = (binSize > 100000UL);
    }
  }

  if (!gotBinMeta) {
    otaStatusCode = 3;
    otaTaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  bool needUpdate = false;
  bool saveMetaOnly = false;

  if (otaInstalledBinSha[0]) {
    if (strcmp(remBinSha, otaInstalledBinSha) == 0) {
      saveMetaOnly = true;
      resCode = 2;
    } else {
      needUpdate = true;
    }
  } else {
    uint32_t binDay = 0;
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/commits?sha=%s&path=ota/firmware.bin&per_page=1",
             GITHUB_REPO, headCommit);
    if (githubApiGet(url, "application/vnd.github+json", apiBuf, 1024)) {
      char dateBuf[32] = "";
      if (extractJsonStringUnescaped(apiBuf, "date", dateBuf, sizeof(dateBuf)) && strlen(dateBuf) >= 10) {
        binDay = parseDayStamp(atoi(dateBuf), atoi(dateBuf + 5), atoi(dateBuf + 8));
      }
    }
    if (manual || binDay == 0 || (binDay + 1UL) >= parseCompileDayStamp()) {
      needUpdate = true;
    } else {
      saveMetaOnly = true;
      resCode = 2;
    }
  }

  if (saveMetaOnly) {
    copyCStr(otaSaveCommitBuf, sizeof(otaSaveCommitBuf), headCommit);
    copyCStr(otaSaveBinShaBuf, sizeof(otaSaveBinShaBuf), remBinSha);
    otaSaveMetaPending = true;
  }

  if (needUpdate) {
    copyCStr(otaPendingVersion, 8, headCommit);
    copyCStr(otaPendingCommit, sizeof(otaPendingCommit), headCommit);
    copyCStr(otaPendingBinSha, sizeof(otaPendingBinSha), remBinSha);
    copyCStr(otaPendingBinUrl, sizeof(otaPendingBinUrl), remBinUrl);
    otaUpdateReady = true;
    resCode = 4;
  }

  otaStatusCode = resCode;
  otaTaskRunning = false;
  vTaskDelete(nullptr);
}

void startBackgroundOtaCheck() {
  if (!canStartTlsSession(true)) return;
  otaManualCheckMode = otaForceRequested;
  otaForceRequested = false;
  otaStatusCode = 1;
  otaTaskRunning = true;
  if (xTaskCreate(otaCheckTask, "ota_chk", 20480, nullptr, 1, nullptr) != pdPASS) {
    otaStatusCode = 3;
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

  beginScreenFrame("ОБНОВЛЕНИЕ СИСТЕМЫ");

  char verMsg[40];
  snprintf(verMsg, sizeof(verMsg), "Коммит #%s", otaPendingVersion);
  drawCenteredUTF8(tft, 134, verMsg, COL_GOLD_SOFT, COL_BG, 1);
  renderOtaProgressBarBuffered(0);

  WiFiClientSecure client;
  HTTPClient https;
  bool success = false;

  if (beginHttpsClient(client, https, otaPendingBinUrl, 15, 6000, 15000)) {
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
    copyCStr(otaInstalledCommit, sizeof(otaInstalledCommit), otaPendingCommit);
    copyCStr(otaInstalledBinSha, sizeof(otaInstalledBinSha), otaPendingBinSha);
    prefs.putString("otaCommit", otaInstalledCommit);
    prefs.putString("otaBinSha", otaInstalledBinSha);

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

void onWifiConnected(unsigned long nowMs) {
  WiFi.setSleep(false);
  wifiConnected = true;
  configureNtp();
  nextOtaCheckMs = nowMs + OTA_FIRST_CHECK_DELAY_MS;
  nextNtfyPollMs = nowMs + 10000UL;
  nextWeatherPollMs = nowMs + 14000UL;
  adminScanNetworks = true;
  adminReportRequested = true;
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
    onWifiConnected(millis());
    return true;
  }

  wifiConnected = false;
  return false;
}

static const char JSON_CT[] = "application/json; charset=utf-8";
void sendJsonStr(int code, const char* body) { server.send(code, JSON_CT, body); }
void sendJsonOk() { sendJsonStr(200, "{\"ok\":true}"); }

void sendWebUiGzip() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", (const char*)WEB_HTML_GZ, WEB_HTML_GZ_LEN);
}

void applyScreenRotation(uint8_t rot, bool redrawNow = true) {
  screenRotation = rot & 3;
  prefs.putUChar("rot", screenRotation);
  tft.setRotation(screenRotation);
  if (redrawNow) {
    transActive = false;
    activeCardRendered = false;
    prevSecondDrawn = -1;
    prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
    prevOrbitBx = -999;
    prevOrbitBy = -999;
    screenNeedsFullRedraw = true;
  }
}

static void handleRouteRoot() { sendWebUiGzip(); }

static void handleRouteRotation() {
  if (server.hasArg("rot")) applyScreenRotation((uint8_t)(server.arg("rot").toInt() & 3), true);
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"rotation\":%u}", (unsigned int)screenRotation);
  sendJsonStr(200, resp);
}

static void handleRouteStatus() {
  bool conn = (WiFi.status() == WL_CONNECTED);
  String ip = conn ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String ssid = conn ? WiFi.SSID() : savedSsid;

  char jbuf[512];
  jbuf[0] = '\0';
  JsonBuf jb{jbuf, sizeof(jbuf), 0};
  jb.fmt("{\"connected\":%s,\"has_saved_pass\":%s,\"tz_offset\":%d,\"rotation\":%u",
         conn ? "true" : "false", (savedPass.length() > 0) ? "true" : "false", tzOffsetHours, (unsigned int)screenRotation);
  jb.strField("ssid", ssid.c_str());
  jb.strField("saved_ssid", savedSsid.c_str());
  jb.strField("ip", ip.c_str());
  jb.strField("weather_city", weatherCity.c_str());
  jb.strField("version", FIRMWARE_VERSION);
  char shortC[8] = "";
  copyCStr(shortC, sizeof(shortC), otaInstalledCommit);
  jb.strField("commit", shortC);
  jb.ch('}');
  sendJsonStr(200, jbuf);
}

static void handleRouteWeather() {
  char jbuf[384];
  jbuf[0] = '\0';
  JsonBuf jb{jbuf, sizeof(jbuf), 0};
  jb.fmt("{\"valid\":%s,\"temp\":%d,\"feels\":%d,\"wind\":%d,\"code\":%d,\"walkable\":true",
         weatherValid ? "true" : "false", (int)weatherTempC, (int)weatherFeelsC,
         (int)weatherWindMs, (int)weatherCode);
  jb.strField("city", weatherCity.c_str());
  jb.strField("desc", weatherCodeToRu(weatherCode));
  jb.strField("mood", weatherMoodText());
  jb.ch('}');
  sendJsonStr(200, jbuf);
}

static void handleRouteWeatherCity() {
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
  sendJsonOk();
}

static void handleRouteMessages() {
  char jbuf[1280];
  jbuf[0] = '\0';
  JsonBuf jb{jbuf, sizeof(jbuf), 0};
  jb.fmt("{\"unread\":%s,\"latest_time\":%lu,\"latest_has_card\":%s,\"plain_time\":%lu,\"plain_unread\":%s",
         unreadMessage ? "true" : "false",
         (unsigned long)(msgHistoryCount > 0 ? msgHistory[0].epoch : 0),
         (msgHistoryCount > 0 && msgHistory[0].hasCard) ? "true" : "false",
         (unsigned long)plainTextEpoch,
         unreadTextMsg ? "true" : "false");
  jb.strField("latest_text", msgHistoryCount > 0 ? msgHistory[0].text : "—");
  jb.strField("plain_text", (plainTextMsg[0] != '\0') ? plainTextMsg : "—");
  jb.raw(",\"history\":[");
  for (int i = 0; i < msgHistoryCount; i++) {
    if (i > 0) jb.ch(',');
    jb.ch('{');
    jb.strField("text", msgHistory[i].text, false);
    jb.fmt(",\"time\":%lu,\"has_card\":%s}",
           (unsigned long)msgHistory[i].epoch, msgHistory[i].hasCard ? "true" : "false");
  }
  jb.raw("]}");
  sendJsonStr(200, jbuf);
}

static void handleRouteTextMsg() {
  String txt = server.arg("text");
  txt.trim();
  if (txt.length() == 0) { sendJsonStr(400, "{\"ok\":false}"); return; }
  savePlainTextMessage(txt.c_str(), getCurrentUtcEpoch(), "");
  if (!apActive && !bannerActive && !btn1HoldUiActive) beginPageTransition(SCREEN_TEXT_MSG);
  sendJsonOk();
}

static void handleRouteCard() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : 0;
  if (!fsReady || idx < 0 || idx >= MAX_MSG_HISTORY) { server.send(404, "text/plain", "not found"); return; }
  char path[12];
  lvcCardPath(path, idx);
  File f = LittleFS.exists(path) ? LittleFS.open(path, "r") : File();
  if (!f) { server.send(404, "text/plain", "no card"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleRouteMarkRead() {
  if (unreadMessage || unreadTextMsg) {
    unreadMessage = false;
    unreadTextMsg = false;
    if ((currentScreen == SCREEN_MESSAGES && !hasActiveCard0()) || currentScreen == SCREEN_TEXT_MSG) {
      screenNeedsFullRedraw = true;
    } else if (currentScreen == SCREEN_TIMER || currentScreen == SCREEN_WEATHER || currentScreen == SCREEN_HEART_IP) {
      int pg = (currentScreen == SCREEN_TIMER) ? 1 : ((currentScreen == SCREEN_WEATHER) ? 4 : 5);
      drawPageDots(pg, false);
    }
  }
  sendJsonOk();
}

static void handleRouteScan() {
  if (server.hasArg("refresh")) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    sendJsonStr(200, "{\"scanning\":true}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n < 0) {
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
    sendJsonStr(200, "{\"scanning\":true}");
    return;
  }
  char jbuf[1024];
  jbuf[0] = '[';
  jbuf[1] = '\0';
  JsonBuf jb{jbuf, sizeof(jbuf), 1};
  bool first = true;
  for (int i = 0; i < n && i < 15; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (!first) jb.ch(',');
    first = false;
    jb.ch('{');
    jb.strField("ssid", ssid.c_str(), false);
    jb.fmt(",\"rssi\":%d,\"open\":%s}",
           (int)WiFi.RSSI(i), (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "true" : "false");
  }
  jb.ch(']');
  sendJsonStr(200, jbuf);
}

static void handleRouteSyncTime() {
  if (server.hasArg("epoch")) setSystemEpoch((uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10));
  sendJsonOk();
}

static void handleRoutePulse() {
  sendJsonOk();
  triggerLovePulse(true);
}

static void handleRouteOtaCheck() {
  otaForceRequested = true;
  startBackgroundOtaCheck();
  sendJsonStr(200, "{\"ok\":true,\"message\":\"Проверяем GitHub...\"}");
}

static void handleRouteOtaStatus() {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"code\":%u,\"ver\":\"%s\",\"commit\":\"%s\"}",
           (unsigned)otaStatusCode, otaPendingVersion, otaPendingVersion);
  sendJsonStr(200, buf);
}

static void sendConnectOk(const char* ip) {
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}", ip);
  sendJsonStr(200, resp);
}

static void handleRouteConnect() {
  String newSsid = server.arg("ssid");
  newSsid.trim();
  String newPass = server.arg("pass");
  int newTz = server.hasArg("tz") ? server.arg("tz").toInt() : tzOffsetHours;
  if (server.hasArg("epoch")) setSystemEpoch((uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10));
  if (newTz != tzOffsetHours) {
    tzOffsetHours = newTz;
    prefs.putInt("tz", tzOffsetHours);
    configureNtp();
    prevDaysDrawn = 0xFFFFFFFFFFFFFFFFULL;
    prevSecondDrawn = -1;
  }
  if (newSsid.length() == 0 || (WiFi.status() == WL_CONNECTED && newSsid == savedSsid && newPass.length() == 0)) {
    if (WiFi.status() == WL_CONNECTED) {
      sendConnectOk(WiFi.localIP().toString().c_str());
      return;
    }
    sendJsonStr(400, "{\"ok\":false}");
    return;
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
    savedSsid = newSsid;
    savedPass = passToTry;
    prefs.putString("ssid", savedSsid);
    prefs.putString("pass", savedPass);
    onWifiConnected(millis());
    String homeIp = WiFi.localIP().toString();
    sendConnectOk(homeIp.c_str());
    pendingDisableAp = true;
    disableApAtMs = millis() + 10000UL;
    showConnectedBanner(homeIp);
  } else {
    wifiConnected = false;
    sendJsonStr(200, "{\"ok\":false}");
  }
}

static void handleRouteCaptive() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

static void handleRouteNotFound() {
  if (apActive) handleRouteCaptive();
  else sendWebUiGzip();
}

struct WebRouteDef { const char* const uri; const decltype(HTTP_GET) method; void (* const fn)(); };
static const WebRouteDef WEB_ROUTES[] = {
  {"/", HTTP_GET, handleRouteRoot},
  {"/api/rotation", HTTP_POST, handleRouteRotation},
  {"/api/status", HTTP_GET, handleRouteStatus},
  {"/api/weather", HTTP_GET, handleRouteWeather},
  {"/api/weather_city", HTTP_POST, handleRouteWeatherCity},
  {"/api/messages", HTTP_GET, handleRouteMessages},
  {"/api/text_msg", HTTP_POST, handleRouteTextMsg},
  {"/api/card", HTTP_GET, handleRouteCard},
  {"/api/mark_read", HTTP_POST, handleRouteMarkRead},
  {"/api/scan", HTTP_GET, handleRouteScan},
  {"/api/sync_time", HTTP_POST, handleRouteSyncTime},
  {"/api/pulse", HTTP_POST, handleRoutePulse},
  {"/api/ota_check", HTTP_POST, handleRouteOtaCheck},
  {"/api/ota_status", HTTP_GET, handleRouteOtaStatus},
  {"/api/connect", HTTP_POST, handleRouteConnect},
  {"/generate_204", HTTP_GET, handleRouteCaptive},
  {"/hotspot-detect.html", HTTP_GET, handleRouteCaptive},
  {"/ncsi.txt", HTTP_GET, handleRouteCaptive},
  {"/connecttest.txt", HTTP_GET, handleRouteCaptive},
};

void setupWebRoutes() {
  for (size_t i = 0; i < sizeof(WEB_ROUTES) / sizeof(WEB_ROUTES[0]); i++) {
    server.on(WEB_ROUTES[i].uri, WEB_ROUTES[i].method, WEB_ROUTES[i].fn);
  }
  server.onNotFound(handleRouteNotFound);
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
    nextMode = SCREEN_TEXT_MSG;
    unreadTextMsg = false;
  } else if (cur == SCREEN_TEXT_MSG) {
    unreadTextMsg = false;
    nextMode = SCREEN_WEATHER;
  } else if (cur == SCREEN_WEATHER) {
    nextMode = SCREEN_HEART_IP;
  } else {
    nextMode = SCREEN_TIMER;
  }

  beginPageTransition(nextMode);
}

void handleButtons(unsigned long nowMs) {
  bool b1Held = false;
  unsigned long b1HoldStart = 0;
  bool b2Tap = false;
  bool b2Held = false;
  unsigned long b2HoldStart = 0;

  portENTER_CRITICAL(&btnMux);
  b1Held = btn1TimerHeld;
  b1HoldStart = btn1TimerPressStartMs;
  b2Tap = btn2TapQueued;
  btn2TapQueued = false;
  b2Held = btn2TimerHeld;
  b2HoldStart = btn2TimerPressStartMs;
  portEXIT_CRITICAL(&btnMux);

  // Кнопка 1: удержание 10 секунд для отправки ответного сердечка (защита от спама)
  if (b1Held && !btn1HoldCompleted && !bannerActive) {
    unsigned long heldMs = (nowMs >= b1HoldStart) ? (nowMs - b1HoldStart) : 0UL;
    if (heldMs >= BTN1_HOLD_UI_DELAY_MS) {
      if (!btn1HoldUiActive) {
        ScreenMode cur = transActive ? transTarget : currentScreen;
        if (cur == SCREEN_PULSE) cur = returnScreenAfterPulse;
        returnScreenAfterHold = cur;
        transActive = false;
        btn1HoldUiActive = true;
        lastHoldUiFrameMs = 0;
        drawHeartHoldStatic();
      }

      if (heldMs >= BTN1_HEART_HOLD_MS) {
        btn1HoldCompleted = true;
        btn1HoldUiActive = false;
        currentScreen = returnScreenAfterHold;
        triggerLovePulse(true);
      } else if (lastHoldUiFrameMs == 0 || (nowMs - lastHoldUiFrameMs) >= 30) {
        lastHoldUiFrameMs = nowMs;
        float progress = (float)heldMs / (float)BTN1_HEART_HOLD_MS;
        if (progress > 1.0f) progress = 1.0f;
        renderBigHeartFrame(64, 34, nowMs, progress >= 0.20f, 0.78f + 0.28f * progress);
        renderHeartHoldBarBuffered(heldMs, BTN1_HEART_HOLD_MS, nowMs);
      }
    }
  } else if (!b1Held) {
    if (btn1HoldUiActive) {
      btn1HoldUiActive = false;
      if (apActive || returnScreenAfterHold == SCREEN_AP_SETUP) {
        currentScreen = SCREEN_AP_SETUP;
        screenNeedsFullRedraw = true;
      } else {
        beginPageTransition(returnScreenAfterHold);
      }
    }
    btn1HoldCompleted = false;
  }

  // Кнопка 2: мгновенное переключение страниц (отлавливается каждые 5 мс через esp_timer) + сброс Wi-Fi 4с
  if (b2Tap) {
    btn2PressStartMs = b2HoldStart;
    btn2LongHandled = false;
    lastHoldSecShown = 0;
    if (!btn1HoldUiActive && !apActive && !bannerActive && (nowMs - lastPageSwitchMs) >= PAGE_SWITCH_COOLDOWN_MS) {
      lastPageSwitchMs = nowMs;
      scrollNextPage();
    }
  } else if (b2Held && !btn2LongHandled && !btn1HoldUiActive) {
    unsigned long held = (nowMs >= btn2PressStartMs) ? (nowMs - btn2PressStartMs) : 0UL;
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
      beginScreenFrame();
      drawCenteredUTF8(tft, 112, "СБРОС WI-FI...", COL_PINK_HOT, COL_BG, 2);
      delay(600);
      resetWifiSettings();
    }
  } else if (!b2Held) {
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
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);

  pinMode(BTN_1, INPUT_PULLUP);
  pinMode(BTN_2, INPUT_PULLUP);

  btn1.rawState = btn1.stableState = (digitalRead(BTN_1) == HIGH);
  btn2.rawState = btn2.stableState = (digitalRead(BTN_2) == HIGH);
  btn1.lastChangeMs = btn2.lastChangeMs = millis();

  esp_timer_create_args_t btnTimerArgs = {};
  btnTimerArgs.callback = &buttonSamplerTimerCb;
  btnTimerArgs.arg = nullptr;
  btnTimerArgs.dispatch_method = ESP_TIMER_TASK;
  btnTimerArgs.name = "btn_poll";
  esp_timer_handle_t btnTimer = nullptr;
  if (esp_timer_create(&btnTimerArgs, &btnTimer) == ESP_OK) {
    esp_timer_start_periodic(btnTimer, 5000);
  }

  SPI.begin(19, 20, 18, TFT_CS);
  tft.beginNoSnow(40000000);

  fsReady = LittleFS.begin(true);

  prefs.begin("lovebox", false);
  screenRotation = prefs.getUChar("rot", 0) & 3;
  tft.setRotation(screenRotation);
  copyCStr(otaInstalledCommit, sizeof(otaInstalledCommit), prefs.getString("otaCommit", "").c_str());
  copyCStr(otaInstalledBinSha, sizeof(otaInstalledBinSha), prefs.getString("otaBinSha", "").c_str());

  playBootAnimation();

  savedSsid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  savedPass = prefs.getString("pass", DEFAULT_WIFI_PASS);
  tzOffsetHours = prefs.getInt("tz", 7);
  weatherCity = prefs.getString("wCity", "Красноярск");
  weatherLat  = prefs.getFloat("wLat", 56.0153f);
  weatherLon  = prefs.getFloat("wLon", 92.8932f);
  uint32_t lastSavedEpoch = prefs.getUInt("lastEpoch", 0);
  if (lastSavedEpoch > 1700000000UL) {
    setSystemEpoch(lastSavedEpoch);
  }
  loadSavedMessages();

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  if (savedSsid.length() > 0) {
    beginScreenFrame("СТАНЦИЯ ОТНОШЕНИЙ");
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
        onWifiConnected(nowMs);
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
        if (adminReportRequested) {
          startBackgroundAdminReport();
        }
      }
    }
  }

  if (adminCmdPendingApply) {
    copyCStr(lastAdminCmdId, sizeof(lastAdminCmdId), pendingAdminCmdId);
    prefs.putString("lastCmdId", lastAdminCmdId);

    if (strcmp(pendingAdminCmd, "SCAN") == 0) {
      adminScanNetworks = true;
      adminReportRequested = true;
    } else if (strcmp(pendingAdminCmd, "STATUS") == 0) {
      adminReportRequested = true;
    } else if (strcmp(pendingAdminCmd, "OTA") == 0) {
      otaForceRequested = true;
      adminReportRequested = true;
    } else if (strcmp(pendingAdminCmd, "REBOOT") == 0) {
      adminRebootRequested = true;
    } else if (strncmp(pendingAdminCmd, "ROT|", 4) == 0) {
      applyScreenRotation((uint8_t)atoi(pendingAdminCmd + 4), true);
      adminReportRequested = true;
    } else if (strncmp(pendingAdminCmd, "CITY|", 5) == 0) {
      char buf[192];
      copyCStr(buf, sizeof(buf), pendingAdminCmd + 5);
      char* p1 = strchr(buf, '|');
      char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
      char* p3 = p2 ? strchr(p2 + 1, '|') : nullptr;
      if (p1 && p2 && p3) {
        *p1 = '\0'; *p2 = '\0'; *p3 = '\0';
        weatherCity = buf;
        parseSimpleFloat(p1 + 1, weatherLat);
        parseSimpleFloat(p2 + 1, weatherLon);
        tzOffsetHours = atoi(p3 + 1);
        prefs.putString("wCity", weatherCity);
        prefs.putFloat("wLat", weatherLat);
        prefs.putFloat("wLon", weatherLon);
        prefs.putInt("tz", tzOffsetHours);
        configureNtp();
        nextWeatherPollMs = nowMs + 500UL;
        if (currentScreen == SCREEN_WEATHER || currentScreen == SCREEN_TIMER) screenNeedsFullRedraw = true;
      }
      adminReportRequested = true;
    }
    adminCmdPendingApply = false;
  }

  if (adminRebootRequested && !adminReportRunning && !otaTaskRunning) {
    delay(150);
    ESP.restart();
  }

  if (weatherNeedsRedraw && currentScreen == SCREEN_WEATHER && !transActive && !bannerActive && !btn1HoldUiActive) {
    weatherNeedsRedraw = false;
    screenNeedsFullRedraw = true;
  }

  if (!btn1HoldUiActive) {
    bool appliedCard = false, appliedText = false;
    bool textIsNewer = (pendingPlainEpoch >= pendingMsgEpoch);
    if (incomingMsgPendingApply && incomingTextPendingApply && textIsNewer) {
      pushNewMessageToHistory(pendingMsgText, pendingMsgEpoch, pendingMsgId, pendingMsgHasCard);
      savePlainTextMessage(pendingPlainText, pendingPlainEpoch, pendingPlainId);
      incomingMsgPendingApply = false;
      incomingTextPendingApply = false;
      appliedText = true;
    } else {
      if (incomingMsgPendingApply) {
        pushNewMessageToHistory(pendingMsgText, pendingMsgEpoch, pendingMsgId, pendingMsgHasCard);
        incomingMsgPendingApply = false;
        appliedCard = true;
      }
      if (incomingTextPendingApply) {
        savePlainTextMessage(pendingPlainText, pendingPlainEpoch, pendingPlainId);
        incomingTextPendingApply = false;
        appliedText = true;
        appliedCard = false;
      }
    }
    if ((appliedCard || appliedText) && !apActive && !bannerActive) {
      beginPageTransition(appliedText ? SCREEN_TEXT_MSG : SCREEN_MESSAGES);
    }
  }

  if (otaSaveMetaPending && !otaTaskRunning) {
    otaSaveMetaPending = false;
    copyCStr(otaInstalledCommit, sizeof(otaInstalledCommit), otaSaveCommitBuf);
    copyCStr(otaInstalledBinSha, sizeof(otaInstalledBinSha), otaSaveBinShaBuf);
    prefs.putString("otaCommit", otaInstalledCommit);
    prefs.putString("otaBinSha", otaInstalledBinSha);
  }

  if (otaUpdateReady && !bannerActive && !btn1HoldUiActive) {
    performOtaUpdate();
    lastFrameMs = millis();
    return;
  }

  handleButtons(nowMs);

  if (btn1HoldUiActive) {
    delay(2);
    return;
  }

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
    drawScreenStatic(currentScreen);
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
    } else if (currentScreen == SCREEN_MESSAGES || currentScreen == SCREEN_TEXT_MSG) {
      bool isTxt = (currentScreen == SCREEN_TEXT_MSG);
      if (!isTxt && hasActiveCard0()) {
        if (!activeCardRendered) drawFullCard0Instant();
      } else {
        renderMiniHeartBuffered(106, 16, nowMs, true);
        renderMessageCardBuffered(dtSec, isTxt);
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
