#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// --- СТРАНИЦЫ КРУГЛОГО ЭКРАНА (ЛИСТАЮТСЯ КНОПКОЙ 2) ---
enum ScreenMode {
  SCREEN_AP_SETUP  = 0,  // Первичная настройка Wi-Fi
  SCREEN_TIMER     = 1,  // Страница 1: Таймер отношений
  SCREEN_MESSAGES  = 2,  // Страница 2: Открытка или послание из интернета
  SCREEN_TEXT_MSG  = 3,  // Страница 3: Отдельное текстовое сообщение
  SCREEN_WEATHER   = 4,  // Страница 4: Краткая погода + «Гулябельно ❤»
  SCREEN_MY_STATUS = 5,  // Страница 5: Чем я сейчас занят (живой статус)
  SCREEN_HEART_IP  = 6,  // Страница 6: Информация о сайте (IP, Wi-Fi, коммит)
  SCREEN_PULSE     = 7   // Всплывающее признание / ответ сердечком по Кнопке 1
};

struct StoredMessage {
  char text[320];
  uint32_t epoch;
  bool hasCard;
};

struct DebouncedButton {
  uint8_t pin;
  bool rawState;
  bool stableState;
  unsigned long lastChangeMs;
};
