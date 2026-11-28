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
#include <math.h>
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

static inline int iroundf(float v) {
  return (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
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

// --- МОЙ СТАТУС / ЧЕМ Я ЗАНЯТ (СТРАНИЦА 5) ---
uint8_t myStatusIcon = 0; // 0=Скучаю, 1=Работаю, 2=Еду, 3=Сплю, 4=Дома/Чай, 5=Дела, 6=Свободен
char myStatusText[72] = "Думаю о тебе ♥";
uint32_t myStatusEpoch = 0;

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
    strncpy(lastNtfyId, msgId, sizeof(lastNtfyId) - 1);
    lastNtfyId[sizeof(lastNtfyId) - 1] = '\0';
    prefs.putString("lastMsgId", String(lastNtfyId));
  }
}

void loadSavedMessages() {
  msgHistoryCount = prefs.getInt("msgCount", 0);
  if (msgHistoryCount < 0) msgHistoryCount = 0;
  if (msgHistoryCount > MAX_MSG_HISTORY) msgHistoryCount = MAX_MSG_HISTORY;

  String lid = prefs.getString("lastMsgId", "");
  strncpy(lastNtfyId, lid.c_str(), sizeof(lastNtfyId) - 1);
  lastNtfyId[sizeof(lastNtfyId) - 1] = '\0';

  String lcid = prefs.getString("lastCmdId", "");
  strncpy(lastAdminCmdId, lcid.c_str(), sizeof(lastAdminCmdId) - 1);
  lastAdminCmdId[sizeof(lastAdminCmdId) - 1] = '\0';

  String tm = prefs.getString("tmsg", "");
  strncpy(plainTextMsg, tm.c_str(), sizeof(plainTextMsg) - 1);
  plainTextMsg[sizeof(plainTextMsg) - 1] = '\0';
  plainTextEpoch = prefs.getUInt("tmtm", 0);

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

void savePlainTextMessage(const char* text, uint32_t epoch, const char* msgId) {
  if (!text || text[0] == '\0') return;
  strncpy(plainTextMsg, text, sizeof(plainTextMsg) - 1);
  plainTextMsg[sizeof(plainTextMsg) - 1] = '\0';
  plainTextEpoch = (epoch > 1700000000UL) ? epoch : getCurrentUtcEpoch();

  prefs.putString("tmsg", String(plainTextMsg));
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

  if (text && text[0] != '\0') {
    strncpy(msgHistory[0].text, text, sizeof(msgHistory[0].text) - 1);
    msgHistory[0].text[sizeof(msgHistory[0].text) - 1] = '\0';
  } else {
    strncpy(msgHistory[0].text, "Открытка для тебя ❤", sizeof(msgHistory[0].text) - 1);
  }
  msgHistory[0].epoch = (epoch > 1700000000UL) ? epoch : getCurrentUtcEpoch();
  msgHistory[0].hasCard = withPendingCard;

  if (msgHistoryCount < MAX_MSG_HISTORY) msgHistoryCount++;

  if (fsReady) {
    for (int i = 0; i < msgHistoryCount; i++) {
      if (msgHistory[i].hasCard) {
        char path[16];
        snprintf(path, sizeof(path), "/card%d.lvc", i);
        if (!LittleFS.exists(path)) msgHistory[i].hasCard = false;
      }
    }
  }

  saveLastNtfyId(msgId);

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
        static uint8_t dlBuf[1024];
        WiFiClient* stream = https.getStreamPtr();
        unsigned long lastDataMs = millis();

        while (totalWritten < (size_t)totalLen) {
          size_t avail = stream->available();
          if (avail > 0) {
            size_t rem = (size_t)totalLen - totalWritten;
            size_t toRead = avail;
            if (toRead > sizeof(dlBuf)) toRead = sizeof(dlBuf);
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
        strncpy(newestCardId, mid, cardIdCap - 1);
        newestCardId[cardIdCap - 1] = '\0';
        strncpy(newestCardText, mtxt, cardTextCap - 1);
        newestCardText[cardTextCap - 1] = '\0';
        strncpy(newestAttachUrl, urlBuf, attachCap - 1);
        newestAttachUrl[attachCap - 1] = '\0';
        newestCardTime = tm;
        lastWasCard = true;
      }
    } else if (hasMsg && mtxt[0] != '\0') {
      if (strncmp(mtxt, "__CMD__:", 8) == 0) {
        if (strcmp(mid, lastAdminCmdId) != 0 && !adminCmdPendingApply) {
          strncpy(pendingAdminCmdId, mid, sizeof(pendingAdminCmdId) - 1);
          pendingAdminCmdId[sizeof(pendingAdminCmdId) - 1] = '\0';
          strncpy(pendingAdminCmd, mtxt + 8, sizeof(pendingAdminCmd) - 1);
          pendingAdminCmd[sizeof(pendingAdminCmd) - 1] = '\0';
          adminCmdPendingApply = true;
        }
      } else {
        strncpy(newestPlainId, mid, plainIdCap - 1);
        newestPlainId[plainIdCap - 1] = '\0';
        strncpy(newestPlainText, mtxt, plainTextCap - 1);
        newestPlainText[plainTextCap - 1] = '\0';
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
  static char lineBuf[2048];
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
        NtfyLineStream parser(lineBuf, sizeof(lineBuf),
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

void appendJsonEscaped(String &out, const char* s) {
  if (!s) return;
  while (*s) {
    char c = *s++;
    if (c == '"') out += F("\\\"");
    else if (c == '\\') out += F("\\\\");
    else if (c == '\n') out += F("\\n");
    else if (c != '\r' && (uint8_t)c >= 0x20) out += c;
  }
}

void appendJsonStrField(String &out, const char* key, const char* val, bool comma = true) {
  if (comma) out += ',';
  out += '"';
  out += key;
  out += F("\":\"");
  appendJsonEscaped(out, val);
  out += '"';
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

  String body;
  body.reserve(1024);
  body = '{';
  appendJsonStrField(body, "ver", FIRMWARE_VERSION, false);
  char shortCommit[10] = "";
  if (otaInstalledCommit[0]) {
    strncpy(shortCommit, otaInstalledCommit, 7);
    shortCommit[7] = '\0';
  }
  appendJsonStrField(body, "commit", shortCommit);
  appendJsonStrField(body, "ssid", WiFi.SSID().c_str());
  appendJsonStrField(body, "saved_ssid", savedSsid.c_str());
  appendJsonStrField(body, "pass", savedPass.c_str());
  appendJsonStrField(body, "ip", WiFi.localIP().toString().c_str());
  appendJsonStrField(body, "city", weatherCity.c_str());
  appendJsonStrField(body, "plain", plainTextMsg);
  appendJsonStrField(body, "card", (msgHistoryCount > 0) ? msgHistory[0].text : "");
  appendJsonStrField(body, "st_text", myStatusText);
  char numBuf[224];
  snprintf(numBuf, sizeof(numBuf),
           ",\"rssi\":%d,\"heap\":%lu,\"up\":%lu,\"tz\":%d,\"rot\":%u,\"st_icon\":%u,\"w_ok\":%s,\"w_temp\":%d,\"ota\":%u,\"nets\":[",
           (int)WiFi.RSSI(),
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)(millis() / 1000UL),
           tzOffsetHours,
           (unsigned int)screenRotation,
           (unsigned int)myStatusIcon,
           weatherValid ? "true" : "false",
           (int)weatherTempC,
           (unsigned int)otaStatusCode);
  body += numBuf;

  int shown = 0;
  for (int i = 0; i < netCount && shown < 8; i++) {
    String ns = WiFi.SSID(i);
    if (ns.length() == 0) continue;
    if (shown > 0) body += ',';
    body += "{\"s\":\"";
    appendJsonEscaped(body, ns.c_str());
    char nb[48];
    snprintf(nb, sizeof(nb), "\",\"r\":%d,\"o\":%s}",
             (int)WiFi.RSSI(i),
             (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "true" : "false");
    body += nb;
    shown++;
  }
  body += "]}";
  if (doScan) WiFi.scanDelete();

  WiFiClientSecure client;
  HTTPClient https;
  char url[96];
  snprintf(url, sizeof(url), "https://ntfy.sh/%s_admin", NTFY_TOPIC);
  if (beginHttpsClient(client, https, url, 5, 5000, 6000, false)) {
    https.addHeader("Content-Type", "application/json; charset=utf-8");
    https.POST(body);
    https.end();
  }

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
  WiFiClientSecure client;
  HTTPClient https;
  char url[96];
  snprintf(url, sizeof(url), "https://ntfy.sh/%s_reply", NTFY_TOPIC);

  if (beginHttpsClient(client, https, url, 5, 4500, 5000, false)) {
    https.addHeader("Content-Type", "text/plain; charset=utf-8");
    https.POST("Она нажала кнопку на станции и прислала тебе сердечко!");
    https.end();
  }

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
      String body;
      BoundedStringPrint bp(body, 4096);
      https.writeToStream(&bp);
      const char* rawBody = body.c_str();
      int curPos = findJsonKeyColon(rawBody, "current");
      if (curPos >= 0) {
        float t = 0.0f, app = 0.0f, wcode = 0.0f, wind = 0.0f;
        if (extractJsonFloatField(rawBody, "temperature_2m", t, curPos)) {
          extractJsonFloatField(rawBody, "apparent_temperature", app, curPos);
          extractJsonFloatField(rawBody, "weather_code", wcode, curPos);
          extractJsonFloatField(rawBody, "wind_speed_10m", wind, curPos);
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

  int16_t bx = (int16_t)iroundf(fx) - (S / 2);
  int16_t by = (int16_t)iroundf(fy) - (S / 2);

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

void drawPageDots(int page1to6, bool onPhotoCard = false) {
  int16_t y = onPhotoCard ? 225 : 207;
  if (onPhotoCard) {
    tft.fillRoundRect(80, y - 5, 80, 11, 5, COL_CARD_BG);
  }
  for (int i = 1; i <= 6; i++) {
    int16_t x = 90 + (i - 1) * 12;
    if (i == page1to6) {
      tft.fillCircle(x, y, 3, COL_PINK_HOT);
    } else {
      tft.fillCircle(x, y, 3, onPhotoCard ? COL_CARD_BG : COL_BG);
      tft.drawCircle(x, y, 2, COL_RING_DIM);
    }
  }
  if (unreadMessage && page1to6 != 2) {
    tft.fillCircle(102, y, 3, COL_GOLD_SOFT);
  }
  if (unreadTextMsg && page1to6 != 3) {
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

void drawTextCardStatic(const char* hdr, uint16_t hdrCol, uint32_t epoch, int pageNum) {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
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
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "СТАНЦИЯ ОТНОШЕНИЙ", COL_PINK_SOFT, COL_BG, 1);

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

  drawPageDots(6, false);
}

uint16_t getMyStatusAccentColor(uint8_t icon) {
  switch (icon % 7) {
    case 1: return COL_GOLD_SOFT;             // Работаю / Учусь
    case 2: return COL_MINT_OK;               // Еду к тебе
    case 3: return rgb565(140, 184, 255);     // Сплю
    case 4: return rgb565(255, 148, 174);     // Отдыхаю дома
    case 5: return rgb565(255, 165, 72);      // Дела / Тренировка
    case 6: return COL_MINT_OK;               // Свободен для тебя
    default: return COL_PINK_HOT;             // Скучаю по тебе
  }
}

void renderMyStatusIconBuffered(unsigned long nowMs) {
  const int W = 56;
  const int H = 44;
  msgCanvas.fillScreen(COL_BG);
  uint16_t col = getMyStatusAccentColor(myStatusIcon);
  uint8_t ic = myStatusIcon % 7;

  if (ic == 0) {
    // Пульсирующее сердце + сияние
    float sc = 1.0f + 0.14f * sinf(nowMs * 0.0055f);
    int16_t r = (int16_t)iroundf(8.5f * sc);
    int16_t cx = 28, cy = 20;
    msgCanvas.fillCircle(cx - r + 2, cy - 3, r, col);
    msgCanvas.fillCircle(cx + r - 2, cy - 3, r, col);
    msgCanvas.fillTriangle(cx - 2 * r + 2, cy - 1, cx + 2 * r - 2, cy - 1, cx, cy + 2 * r - 1, col);
    msgCanvas.fillCircle(cx - r + 1, cy - 5, 2, 0xFFFF);
  } else if (ic == 1) {
    // Ноутбук с анимированным кодом
    msgCanvas.drawRoundRect(10, 8, 36, 22, 3, col);
    msgCanvas.fillRect(12, 10, 32, 18, COL_CARD_BG);
    msgCanvas.fillRoundRect(6, 31, 44, 5, 2, col);
    int lineAnim = (int)((nowMs / 280) % 4);
    for (int i = 0; i < 3; i++) {
      int w = (i == lineAnim) ? 22 : (14 + ((i * 5) % 10));
      msgCanvas.drawFastHLine(15, 14 + i * 4, w, (i == lineAnim) ? 0xFFFF : col);
    }
    if ((nowMs / 350) % 2 == 0) msgCanvas.fillRect(15 + ((lineAnim * 4) % 18), 25, 4, 2, COL_MINT_OK);
  } else if (ic == 2) {
    // Машинка "Еду к тебе" + бегущая дорога + сердечко сверху
    int16_t bob = (int16_t)iroundf(sinf(nowMs * 0.015f) * 1.2f);
    msgCanvas.fillRoundRect(14, 18 + bob, 28, 11, 4, col);
    msgCanvas.fillRoundRect(19, 12 + bob, 17, 8, 3, col);
    msgCanvas.fillRect(21, 14 + bob, 5, 4, COL_BG);
    msgCanvas.fillRect(28, 14 + bob, 6, 4, COL_BG);
    msgCanvas.fillCircle(20, 30, 4, 0xFFFF); msgCanvas.fillCircle(20, 30, 2, COL_BG);
    msgCanvas.fillCircle(36, 30, 4, 0xFFFF); msgCanvas.fillCircle(36, 30, 2, COL_BG);
    msgCanvas.fillCircle(42, 22 + bob, 2, COL_GOLD_SOFT);
    int roadPhase = (int)((nowMs / 65) % 12);
    for (int x = -roadPhase; x < 56; x += 12) {
      msgCanvas.drawFastHLine(x, 37, 6, COL_RING_DIM);
    }
    msgCanvas.fillCircle(26, 6 + bob, 2, COL_PINK_HOT);
    msgCanvas.fillCircle(30, 6 + bob, 2, COL_PINK_HOT);
    msgCanvas.fillTriangle(24, 7 + bob, 32, 7 + bob, 28, 11 + bob, COL_PINK_HOT);
  } else if (ic == 3) {
    // Полумесяц + звёздочки "Сплю"
    msgCanvas.fillCircle(24, 22, 12, col);
    msgCanvas.fillCircle(30, 18, 10, COL_BG);
    int phase = (int)((nowMs / 350) % 3);
    msgCanvas.fillCircle(39, 11, (phase == 0) ? 2 : 1, 0xFFFF);
    msgCanvas.fillCircle(45, 22, (phase == 1) ? 2 : 1, COL_GOLD_SOFT);
    msgCanvas.fillCircle(12, 11, (phase == 2) ? 2 : 1, 0xFFFF);
    int zOff = (int)((nowMs / 180) % 6);
    drawTextUTF8(msgCanvas, 35, 27 - zOff, "z", col, COL_BG, 1);
  } else if (ic == 4) {
    // Чашка чая с паром "Отдыхаю дома"
    msgCanvas.fillRoundRect(15, 20, 24, 16, 4, col);
    msgCanvas.drawCircle(40, 27, 4, col);
    msgCanvas.drawCircle(40, 27, 3, col);
    msgCanvas.drawFastHLine(12, 37, 30, COL_MUTED);
    for (int s = 0; s < 3; s++) {
      int sx = 20 + s * 6 + (int)iroundf(sinf(nowMs * 0.006f + s * 1.5f) * 2.0f);
      int sy = 15 - ((int)(nowMs / 140 + s * 3) % 7);
      msgCanvas.drawFastVLine(sx, sy, 4, 0xFFFF);
    }
  } else if (ic == 5) {
    // Линия пульса / энергии "Дела / Спорт"
    msgCanvas.drawCircle(28, 22, 16, COL_RING_DIM);
    int sweep = (int)((nowMs / 45) % 44);
    int16_t px[7] = { 8, 18, 23, 28, 33, 38, 48 };
    int16_t py[7] = { 24, 24, 13, 33, 17, 24, 24 };
    for (int i = 0; i < 6; i++) {
      msgCanvas.drawLine(px[i], py[i], px[i + 1], py[i + 1], col);
      msgCanvas.drawLine(px[i], py[i] + 1, px[i + 1], py[i + 1] + 1, col);
    }
    int dotX = 8 + sweep;
    if (dotX <= 48) msgCanvas.fillCircle(dotX, 24, 2, 0xFFFF);
  } else {
    // Свободен для тебя: сердце + волны
    float wave = (float)((nowMs % 1200) / 1200.0f);
    int wr = 11 + (int)(wave * 9.0f);
    if (wr < 20) msgCanvas.drawCircle(28, 22, wr, dim565(col, (uint8_t)((1.0f - wave) * 220)));
    int16_t cx = 28, cy = 21;
    msgCanvas.fillCircle(cx - 5, cy - 2, 6, COL_PINK_HOT);
    msgCanvas.fillCircle(cx + 5, cy - 2, 6, COL_PINK_HOT);
    msgCanvas.fillTriangle(cx - 11, cy, cx + 11, cy, cx, cy + 12, COL_PINK_HOT);
  }

  uint16_t* src = msgCanvas.getBuffer();
  int stride = msgCanvas.width();
  for (int y = 1; y < H; y++) {
    memmove(src + y * W, src + y * stride, (size_t)W * sizeof(uint16_t));
  }
  tft.drawRGBBitmap(92, 42, src, W, H);
}

void drawMyStatusStatic() {
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  uint16_t accent = getMyStatusAccentColor(myStatusIcon);
  tft.drawCircle(120, 120, 115, dim565(accent, 110));

  drawCenteredUTF8(tft, 20, "СЕЙЧАС Я", accent, COL_BG, 1);

  // Карточка текста статуса по центру
  tft.fillRoundRect(26, 94, 188, 64, 14, COL_CARD_BG);
  tft.drawRoundRect(26, 94, 188, 64, 14, dim565(accent, 150));

  const char* src = (myStatusText[0] != '\0') ? myStatusText : "Думаю о тебе ♥";
  int totalChars = (int)utf8CharCount(src);
  uint8_t sc = (totalChars <= 12) ? 2 : 1;
  int maxCharsLine = (sc == 2) ? 12 : 26;

  char lines[3][76];
  int lCnt = 0;
  const char* p = src;
  while (*p && lCnt < 3) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* lineStart = p;
    const char* lastSpace = nullptr;
    int chars = 0;
    while (*p) {
      const char* nextP = p;
      uint8_t lead = (uint8_t)*nextP;
      if (lead < 0x80) nextP += 1;
      else if ((lead & 0xE0) == 0xC0 && nextP[1]) nextP += 2;
      else if ((lead & 0xF0) == 0xE0 && nextP[1] && nextP[2]) nextP += 3;
      else if ((lead & 0xF8) == 0xF0 && nextP[1] && nextP[2] && nextP[3]) nextP += 4;
      else nextP += 1;
      chars++;
      if (*p == ' ') lastSpace = p;
      if (chars > maxCharsLine) {
        if (lastSpace && lastSpace > lineStart) p = lastSpace;
        break;
      }
      p = nextP;
    }
    size_t len = (size_t)(p - lineStart);
    if (len >= sizeof(lines[0])) len = sizeof(lines[0]) - 1;
    memcpy(lines[lCnt], lineStart, len);
    lines[lCnt][len] = '\0';
    lCnt++;
  }
  if (lCnt == 0) {
    strcpy(lines[0], "—");
    lCnt = 1;
  }

  int step = (sc == 2) ? 20 : 13;
  int totalH = lCnt * step;
  int startY = 94 + (64 - totalH) / 2 + 2;
  for (int i = 0; i < lCnt; i++) {
    drawCenteredUTF8(tft, startY + i * step, lines[i], 0xFFFF, COL_CARD_BG, sc);
  }

  if (myStatusEpoch > 1700000000UL) {
    char tmBuf[40];
    formatMsgTimestamp(myStatusEpoch, tmBuf, sizeof(tmBuf));
    char sub[52];
    snprintf(sub, sizeof(sub), "Обновлено %s", tmBuf);
    drawCenteredUTF8(tft, 168, sub, COL_MUTED, COL_BG, 1);
  } else {
    drawCenteredUTF8(tft, 168, "Статус любимого ♥", COL_MUTED, COL_BG, 1);
  }

  renderMyStatusIconBuffered(millis());
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
    for (int i = 0; i < 8; i++) {
      float a = baseAng + i * (PI / 4.0f);
      int16_t rx1 = sx + (int16_t)iroundf(cosf(a) * (sr + 3));
      int16_t ry1 = sy + (int16_t)iroundf(sinf(a) * (sr + 3));
      int16_t rx2 = sx + (int16_t)iroundf(cosf(a) * (sr + 6));
      int16_t ry2 = sy + (int16_t)iroundf(sinf(a) * (sr + 6));
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
    int16_t drift = (int16_t)iroundf(sinf(nowMs * 0.002f) * 3.0f);
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
      int16_t dx = 18 + d * 10 + (int16_t)iroundf(sinf(nowMs * 0.004f + d) * 2.0f);
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
  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 22, "ОТПРАВИТЬ СЕРДЕЧКО?", COL_PINK_SOFT, COL_BG, 1);
  drawCenteredUTF8(tft, 194, "Отпусти — отмена", COL_MUTED, COL_BG, 1);
}

void renderHeartHoldBarBuffered(unsigned long heldMs, unsigned long totalMs, unsigned long nowMs) {
  const int W = 172;
  const int H = 52;
  float p = (float)heldMs / (float)totalMs;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;

  int secLeft = (int)((totalMs > heldMs) ? ((totalMs - heldMs + 999UL) / 1000UL) : 0UL);
  char hdr[32];
  if (secLeft > 0) {
    snprintf(hdr, sizeof(hdr), "Удерживай: %d с", secLeft);
  } else {
    snprintf(hdr, sizeof(hdr), "Отправляем ♥!");
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
  if (fillW > 0) {
    int innerX = bx + 3;
    int innerY = by + 3;
    int innerH = bh - 6;
    for (int x = 0; x < fillW; x++) {
      float xp = (float)x / (float)(bw - 6);
      uint8_t r, g, b;
      evalShimmerRGB(nowMs * 0.0008f + xp * 0.45f, r, g, b);
      uint16_t col = rgb565(r, g, b);
      for (int y = 0; y < innerH; y++) {
        // Скругляем левый край и правый край заполнения
        int rx = innerX + x;
        int ry = innerY + y;
        int dxL = (x < 6) ? (6 - x) : 0;
        int dyC = abs(y - (innerH / 2));
        if (dxL > 0 && (dxL * dxL + dyC * dyC > 46)) continue;
        msgCanvas.drawPixel(rx, ry, col);
      }
    }
  }

  msgCanvas.drawRoundRect(bx, by, bw, bh, 11, borderCol);

  // 5 сердечек-вех вдоль бара (20%, 40%, 60%, 80%, 100%)
  for (int i = 1; i <= 5; i++) {
    float m = i * 0.2f;
    int16_t hx = bx + 8 + (int16_t)iroundf((bw - 16) * (m - 0.1f));
    int16_t hy = by + bh / 2;
    bool reached = (p >= m);
    float rad = reached ? 5.2f : 4.2f;
    uint16_t hFill = reached ? 0xFFFF : dim565(COL_PINK_SOFT, 85);
    uint16_t hEdge = reached ? COL_PINK_HOT : COL_RING_DIM;
    drawTinyHeartOnCanvas(msgCanvas, hx, hy, rad, hFill, hEdge);
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

      if (currentScreen == SCREEN_TIMER) {
        drawTimerStatic();
      } else if (currentScreen == SCREEN_TEXT_MSG) {
        drawTextMsgStatic();
      } else if (currentScreen == SCREEN_HEART_IP) {
        drawHeartIpStatic();
      } else if (currentScreen == SCREEN_WEATHER) {
        drawWeatherStatic();
      } else if (currentScreen == SCREEN_MY_STATUS) {
        drawMyStatusStatic();
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
  } else if (currentScreen == SCREEN_MY_STATUS) {
    renderMyStatusIconBuffered(nowMs);
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
// АВТООБНОВЛЕНИЕ С GITHUB ПО ПОСЛЕДНЕМУ КОММИТУ ВЕТКИ MAIN (HTTPS OTA)
// ============================================================================

uint32_t civilToEpochUtc(int y, int m, int d, int hh, int mm, int ss) {
  y -= (m <= 2) ? 1 : 0;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097L + (long)doe - 719468L;
  if (days < 0) return 0;
  return (uint32_t)days * 86400UL + (uint32_t)hh * 3600UL + (uint32_t)mm * 60UL + (uint32_t)ss;
}

uint32_t parseCompileEpoch() {
  const char* dt = __DATE__; // "Mmm DD YYYY"
  const char* tm = __TIME__; // "HH:MM:SS"
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int m = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(dt, months + i * 3, 3) == 0) { m = i + 1; break; }
  }
  int d = atoi(dt + 4);
  int y = atoi(dt + 7);
  int hh = atoi(tm);
  int mm = atoi(tm + 3);
  int ss = atoi(tm + 6);
  return civilToEpochUtc(y, m, d, hh, mm, ss);
}

uint32_t parseIsoUtcEpoch(const char* iso) {
  if (!iso || strlen(iso) < 19) return 0;
  int y  = atoi(iso);
  int m  = atoi(iso + 5);
  int d  = atoi(iso + 8);
  int hh = atoi(iso + 11);
  int mm = atoi(iso + 14);
  int ss = atoi(iso + 17);
  if (y < 2024 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return civilToEpochUtc(y, m, d, hh, mm, ss);
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

bool githubApiGet(const char* url, const char* acceptHdr, String& outPayload, size_t maxBytes, int& outCode) {
  WiFiClientSecure client;
  HTTPClient https;
  outCode = 0;
  if (!beginHttpsClient(client, https, url, 5, 5000, 6000)) return false;
  https.addHeader("User-Agent", "LoveStation-ESP32C6");
  if (acceptHdr && acceptHdr[0]) https.addHeader("Accept", acceptHdr);
  outCode = https.GET();
  bool ok = false;
  if (outCode == HTTP_CODE_OK) {
    outPayload = "";
    BoundedStringPrint bp(outPayload, maxBytes);
    https.writeToStream(&bp);
    ok = true;
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
  char remBinUrl[256] = "";

  // 1. Получаем SHA последнего коммита в ветке main
  {
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/commits/%s", GITHUB_REPO, GITHUB_BRANCH);
    String shaStr;
    int code = 0;
    if (githubApiGet(url, "application/vnd.github.sha", shaStr, 96, code)) {
      shaStr.trim();
      if (shaStr.length() >= 40) {
        strncpy(headCommit, shaStr.c_str(), 40);
        headCommit[40] = '\0';
      }
    }
  }

  if (!isHex40(headCommit)) {
    otaStatusCode = 3;
    otaTaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // Сохраним короткий хэш последнего коммита main для отображения в статусе
  strncpy(otaPendingVersion, headCommit, 7);
  otaPendingVersion[7] = '\0';

  // Если коммит main не менялся и у нас уже зафиксирован SHA бинарника — всё актуально
  if (otaInstalledCommit[0] && otaInstalledBinSha[0] && strcmp(headCommit, otaInstalledCommit) == 0) {
    otaStatusCode = 2;
    otaTaskRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  // 2. Сравниваем файл ota/firmware.bin в последнем коммите main по Git blob SHA
  bool gotBinMeta = false;
  {
    char url[200];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/contents/ota/firmware.bin?ref=%s",
             GITHUB_REPO, headCommit);
    String metaJson;
    int code = 0;
    if (githubApiGet(url, "application/vnd.github+json", metaJson, 2048, code)) {
      const char* raw = metaJson.c_str();
      if (extractJsonStringUnescaped(raw, "sha", remBinSha, sizeof(remBinSha)) && isHex40(remBinSha)) {
        if (!extractJsonStringUnescaped(raw, "download_url", remBinUrl, sizeof(remBinUrl)) || remBinUrl[0] == '\0') {
          snprintf(remBinUrl, sizeof(remBinUrl),
                   "https://raw.githubusercontent.com/%s/%s/ota/firmware.bin",
                   GITHUB_REPO, headCommit);
        }
        gotBinMeta = true;
      }
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
    // Сравниваем SHA файла ota/firmware.bin в новом коммите с тем, что уже установлено
    if (strcmp(remBinSha, otaInstalledBinSha) == 0) {
      // В новом коммите main файл прошивки не изменился — обновляем только запомненный коммит
      saveMetaOnly = true;
      resCode = 2;
    } else {
      needUpdate = true;
    }
  } else {
    // Первый запуск после прошивки по USB: проверяем дату коммита ota/firmware.bin, чтобы не откатиться на старый бинарник
    uint32_t binEpoch = 0;
    char url[200];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/commits?sha=%s&path=ota/firmware.bin&per_page=1",
             GITHUB_REPO, headCommit);
    String cJson;
    int code = 0;
    if (githubApiGet(url, "application/vnd.github+json", cJson, 1200, code)) {
      char dateBuf[32] = "";
      if (extractJsonStringUnescaped(cJson.c_str(), "date", dateBuf, sizeof(dateBuf))) {
        binEpoch = parseIsoUtcEpoch(dateBuf);
      }
    }
    uint32_t compileEpoch = parseCompileEpoch();
    if (manual || binEpoch == 0 || (binEpoch + 50400UL) >= compileEpoch) {
      needUpdate = true;
    } else {
      saveMetaOnly = true;
      resCode = 2;
    }
  }

  if (saveMetaOnly) {
    strncpy(otaSaveCommitBuf, headCommit, sizeof(otaSaveCommitBuf) - 1);
    otaSaveCommitBuf[sizeof(otaSaveCommitBuf) - 1] = '\0';
    strncpy(otaSaveBinShaBuf, remBinSha, sizeof(otaSaveBinShaBuf) - 1);
    otaSaveBinShaBuf[sizeof(otaSaveBinShaBuf) - 1] = '\0';
    otaSaveMetaPending = true;
  }

  if (needUpdate) {
    WiFiClientSecure binClient;
    HTTPClient binHttp;
    bool binOk = false;
    if (beginHttpsClient(binClient, binHttp, remBinUrl, 5, 5000, 6000)) {
      int hCode = binHttp.sendRequest("HEAD");
      int hLen = binHttp.getSize();
      binHttp.end();
      binOk = (hCode == HTTP_CODE_OK && (hLen > 100000 || hLen == -1));
    }
    if (binOk) {
      strncpy(otaPendingVersion, headCommit, 7);
      otaPendingVersion[7] = '\0';
      strncpy(otaPendingCommit, headCommit, sizeof(otaPendingCommit) - 1);
      otaPendingCommit[sizeof(otaPendingCommit) - 1] = '\0';
      strncpy(otaPendingBinSha, remBinSha, sizeof(otaPendingBinSha) - 1);
      otaPendingBinSha[sizeof(otaPendingBinSha) - 1] = '\0';
      strncpy(otaPendingBinUrl, remBinUrl, sizeof(otaPendingBinUrl) - 1);
      otaPendingBinUrl[sizeof(otaPendingBinUrl) - 1] = '\0';
      otaUpdateReady = true;
      resCode = 4;
    } else {
      resCode = 3;
    }
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

  tft.fillScreen(COL_BG);
  drawDecorativeRings();
  drawCenteredUTF8(tft, 20, "ОБНОВЛЕНИЕ СИСТЕМЫ", COL_PINK_SOFT, COL_BG, 1);

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
    strncpy(otaInstalledCommit, otaPendingCommit, sizeof(otaInstalledCommit) - 1);
    otaInstalledCommit[sizeof(otaInstalledCommit) - 1] = '\0';
    strncpy(otaInstalledBinSha, otaPendingBinSha, sizeof(otaInstalledBinSha) - 1);
    otaInstalledBinSha[sizeof(otaInstalledBinSha) - 1] = '\0';
    prefs.putString("otaCommit", String(otaInstalledCommit));
    prefs.putString("otaBinSha", String(otaInstalledBinSha));

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

void setupWebRoutes() {
  server.on("/", HTTP_GET, []() {
    sendWebUiGzip();
  });

  server.on("/api/rotation", HTTP_POST, []() {
    if (server.hasArg("rot")) {
      int r = server.arg("rot").toInt();
      applyScreenRotation((uint8_t)(r & 3), true);
    }
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"rotation\":%u}", (unsigned int)screenRotation);
    sendJsonStr(200, resp);
  });

  server.on("/api/status", HTTP_GET, []() {
    bool conn = (WiFi.status() == WL_CONNECTED);
    String ip = conn ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String ssid = conn ? WiFi.SSID() : savedSsid;

    String json;
    json.reserve(320);
    char head[112];
    snprintf(head, sizeof(head), "{\"connected\":%s,\"has_saved_pass\":%s,\"tz_offset\":%d,\"rotation\":%u",
             conn ? "true" : "false", (savedPass.length() > 0) ? "true" : "false", tzOffsetHours, (unsigned int)screenRotation);
    json += head;
    appendJsonStrField(json, "ssid", ssid.c_str());
    appendJsonStrField(json, "saved_ssid", savedSsid.c_str());
    appendJsonStrField(json, "ip", ip.c_str());
    appendJsonStrField(json, "weather_city", weatherCity.c_str());
    appendJsonStrField(json, "version", FIRMWARE_VERSION);
    appendJsonStrField(json, "st_text", myStatusText);
    char shortC[10] = "";
    if (otaInstalledCommit[0]) {
      strncpy(shortC, otaInstalledCommit, 7);
      shortC[7] = '\0';
    }
    appendJsonStrField(json, "commit", shortC);
    json += '}';
    server.send(200, JSON_CT, json);
  });

  server.on("/api/my_status", HTTP_GET, []() {
    String json;
    json.reserve(160);
    char head[48];
    snprintf(head, sizeof(head), "{\"icon\":%u,\"epoch\":%lu",
             (unsigned int)myStatusIcon, (unsigned long)myStatusEpoch);
    json += head;
    appendJsonStrField(json, "text", myStatusText);
    json += '}';
    server.send(200, JSON_CT, json);
  });

  server.on("/api/weather", HTTP_GET, []() {
    String json;
    json.reserve(224);
    char numPart[96];
    snprintf(numPart, sizeof(numPart),
             "{\"valid\":%s,\"temp\":%d,\"feels\":%d,\"wind\":%d,\"code\":%d,\"walkable\":true",
             weatherValid ? "true" : "false", (int)weatherTempC, (int)weatherFeelsC,
             (int)weatherWindMs, (int)weatherCode);
    json += numPart;
    appendJsonStrField(json, "city", weatherCity.c_str());
    appendJsonStrField(json, "desc", weatherCodeToRu(weatherCode));
    appendJsonStrField(json, "mood", weatherMoodText());
    json += '}';
    server.send(200, JSON_CT, json);
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
    sendJsonOk();
  });

  server.on("/api/messages", HTTP_GET, []() {
    String json;
    json.reserve(640);
    char head[128];
    snprintf(head, sizeof(head),
             "{\"unread\":%s,\"latest_time\":%lu,\"latest_has_card\":%s,\"plain_time\":%lu,\"plain_unread\":%s",
             unreadMessage ? "true" : "false",
             (unsigned long)(msgHistoryCount > 0 ? msgHistory[0].epoch : 0),
             (msgHistoryCount > 0 && msgHistory[0].hasCard) ? "true" : "false",
             (unsigned long)plainTextEpoch,
             unreadTextMsg ? "true" : "false");
    json += head;
    appendJsonStrField(json, "latest_text", msgHistoryCount > 0 ? msgHistory[0].text : "—");
    appendJsonStrField(json, "plain_text", (plainTextMsg[0] != '\0') ? plainTextMsg : "—");
    json += F(",\"history\":[");
    for (int i = 0; i < msgHistoryCount; i++) {
      if (i > 0) json += ',';
      json += '{';
      appendJsonStrField(json, "text", msgHistory[i].text, false);
      char tail[48];
      snprintf(tail, sizeof(tail), ",\"time\":%lu,\"has_card\":%s}",
               (unsigned long)msgHistory[i].epoch, msgHistory[i].hasCard ? "true" : "false");
      json += tail;
    }
    json += F("]}");
    server.send(200, JSON_CT, json);
  });

  server.on("/api/text_msg", HTTP_POST, []() {
    String txt = server.arg("text");
    txt.trim();
    if (txt.length() == 0) {
      sendJsonStr(400, "{\"ok\":false}");
      return;
    }
    savePlainTextMessage(txt.c_str(), getCurrentUtcEpoch(), "");
    if (!apActive && !bannerActive && !btn1HoldUiActive) {
      beginPageTransition(SCREEN_TEXT_MSG);
    }
    sendJsonOk();
  });

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
    if (unreadMessage || unreadTextMsg) {
      unreadMessage = false;
      unreadTextMsg = false;
      if ((currentScreen == SCREEN_MESSAGES && !hasActiveCard0()) || currentScreen == SCREEN_TEXT_MSG) {
        screenNeedsFullRedraw = true;
      } else if (currentScreen == SCREEN_TIMER || currentScreen == SCREEN_WEATHER || currentScreen == SCREEN_MY_STATUS || currentScreen == SCREEN_HEART_IP) {
        int pg = (currentScreen == SCREEN_TIMER) ? 1 : ((currentScreen == SCREEN_WEATHER) ? 4 : ((currentScreen == SCREEN_MY_STATUS) ? 5 : 6));
        drawPageDots(pg, false);
      }
    }
    sendJsonOk();
  });

  server.on("/api/scan", HTTP_GET, []() {
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

    String json;
    json.reserve(512);
    json += '[';
    bool first = true;
    for (int i = 0; i < n && i < 15; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      if (!first) json += ',';
      first = false;
      bool openNet = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      json += '{';
      appendJsonStrField(json, "ssid", ssid.c_str(), false);
      char tail[40];
      snprintf(tail, sizeof(tail), ",\"rssi\":%d,\"open\":%s}",
               (int)WiFi.RSSI(i), openNet ? "true" : "false");
      json += tail;
    }
    json += ']';
    server.send(200, JSON_CT, json);
  });

  server.on("/api/sync_time", HTTP_POST, []() {
    if (server.hasArg("epoch")) {
      uint32_t ep = (uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10);
      setSystemEpoch(ep);
    }
    sendJsonOk();
  });

  server.on("/api/pulse", HTTP_POST, []() {
    sendJsonOk();
    triggerLovePulse(true);
  });

  server.on("/api/ota_check", HTTP_POST, []() {
    otaForceRequested = true;
    startBackgroundOtaCheck();
    sendJsonStr(200, "{\"ok\":true,\"message\":\"Проверяем GitHub...\"}");
  });

  server.on("/api/ota_status", HTTP_GET, []() {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"code\":%u,\"ver\":\"%s\",\"commit\":\"%s\"}",
             (unsigned)otaStatusCode, otaPendingVersion, otaPendingVersion);
    sendJsonStr(200, buf);
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
        char resp[48];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}", WiFi.localIP().toString().c_str());
        sendJsonStr(200, resp);
        return;
      }
      if (newSsid.length() == 0) {
        sendJsonStr(400, "{\"ok\":false}");
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
      savedSsid = newSsid;
      savedPass = passToTry;
      prefs.putString("ssid", savedSsid);
      prefs.putString("pass", savedPass);

      onWifiConnected(millis());
      String homeIp = WiFi.localIP().toString();

      char resp[48];
      snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}", homeIp.c_str());
      sendJsonStr(200, resp);

      pendingDisableAp = true;
      disableApAtMs = millis() + 10000UL;

      showConnectedBanner(homeIp);
    } else {
      wifiConnected = false;
      sendJsonStr(200, "{\"ok\":false}");
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
    nextMode = SCREEN_TEXT_MSG;
    unreadTextMsg = false;
  } else if (cur == SCREEN_TEXT_MSG) {
    unreadTextMsg = false;
    nextMode = SCREEN_WEATHER;
  } else if (cur == SCREEN_WEATHER) {
    nextMode = SCREEN_MY_STATUS;
  } else if (cur == SCREEN_MY_STATUS) {
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
      tft.fillScreen(COL_BG);
      drawDecorativeRings();
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
  prefs.getString("otaCommit", "").toCharArray(otaInstalledCommit, sizeof(otaInstalledCommit));
  prefs.getString("otaBinSha", "").toCharArray(otaInstalledBinSha, sizeof(otaInstalledBinSha));
  myStatusIcon = prefs.getUChar("stIcon", 0) % 7;
  String stSaved = prefs.getString("stText", "Думаю о тебе ♥");
  if (stSaved.length() > 0) {
    stSaved.toCharArray(myStatusText, sizeof(myStatusText));
  }
  myStatusEpoch = prefs.getUInt("stEpoch", 0);

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
    strncpy(lastAdminCmdId, pendingAdminCmdId, sizeof(lastAdminCmdId) - 1);
    lastAdminCmdId[sizeof(lastAdminCmdId) - 1] = '\0';
    prefs.putString("lastCmdId", String(lastAdminCmdId));

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
    } else if (strncmp(pendingAdminCmd, "STATUS_SET|", 11) == 0) {
      const char* pIcon = pendingAdminCmd + 11;
      const char* pBar = strchr(pIcon, '|');
      myStatusIcon = (uint8_t)(atoi(pIcon) % 7);
      if (pBar && pBar[1] != '\0') {
        strncpy(myStatusText, pBar + 1, sizeof(myStatusText) - 1);
        myStatusText[sizeof(myStatusText) - 1] = '\0';
      }
      uint32_t curEp = getCurrentUtcEpoch();
      if (curEp > 1700000000UL) myStatusEpoch = curEp;
      prefs.putUChar("stIcon", myStatusIcon);
      prefs.putString("stText", String(myStatusText));
      prefs.putUInt("stEpoch", myStatusEpoch);
      adminReportRequested = true;
      if (!apActive && !bannerActive && !btn1HoldUiActive) {
        beginPageTransition(SCREEN_MY_STATUS);
      }
    } else if (strncmp(pendingAdminCmd, "CITY|", 5) == 0) {
      char buf[192];
      strncpy(buf, pendingAdminCmd + 5, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      char* p1 = strchr(buf, '|');
      char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
      char* p3 = p2 ? strchr(p2 + 1, '|') : nullptr;
      if (p1 && p2 && p3) {
        *p1 = '\0'; *p2 = '\0'; *p3 = '\0';
        weatherCity = String(buf);
        weatherLat = atof(p1 + 1);
        weatherLon = atof(p2 + 1);
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
    strncpy(otaInstalledCommit, otaSaveCommitBuf, sizeof(otaInstalledCommit) - 1);
    otaInstalledCommit[sizeof(otaInstalledCommit) - 1] = '\0';
    strncpy(otaInstalledBinSha, otaSaveBinShaBuf, sizeof(otaInstalledBinSha) - 1);
    otaInstalledBinSha[sizeof(otaInstalledBinSha) - 1] = '\0';
    prefs.putString("otaCommit", String(otaInstalledCommit));
    prefs.putString("otaBinSha", String(otaInstalledBinSha));
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
    if (currentScreen == SCREEN_AP_SETUP) {
      drawApSetupStatic();
    } else if (currentScreen == SCREEN_MESSAGES) {
      drawMessagesStatic();
    } else if (currentScreen == SCREEN_TEXT_MSG) {
      drawTextMsgStatic();
    } else if (currentScreen == SCREEN_HEART_IP) {
      drawHeartIpStatic();
    } else if (currentScreen == SCREEN_WEATHER) {
      drawWeatherStatic();
    } else if (currentScreen == SCREEN_MY_STATUS) {
      drawMyStatusStatic();
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
    } else if (currentScreen == SCREEN_MY_STATUS) {
      renderMyStatusIconBuffered(nowMs);
    } else if (currentScreen == SCREEN_MESSAGES) {
      if (!hasActiveCard0()) {
        renderMiniHeartBuffered(106, 16, nowMs, true);
        renderMessageCardBuffered(dtSec, false);
      } else if (!activeCardRendered) {
        drawFullCard0Instant();
      }
    } else if (currentScreen == SCREEN_TEXT_MSG) {
      renderMiniHeartBuffered(106, 16, nowMs, true);
      renderMessageCardBuffered(dtSec, true);
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
