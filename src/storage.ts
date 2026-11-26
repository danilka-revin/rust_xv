/** Persisted sketch storage (localStorage), plus starter examples. */

export interface SketchFile {
  id: string;
  name: string;
  content: string;
}

export interface Sketch {
  id: string;
  name: string;
  files: SketchFile[];
  mainFile: string;
  updatedAt: number;
}

const STORAGE_KEY = 'sketchflash.sketches.v1';
const ACTIVE_KEY = 'sketchflash.active.v1';

export function uid(): string {
  return Math.random().toString(36).slice(2, 10);
}

export function newSketch(name = 'Новый скетч', content = BLINK): Sketch {
  const file = { id: uid(), name: `${sanitize(name)}.ino`, content };
  return { id: uid(), name, files: [file], mainFile: file.name, updatedAt: Date.now() };
}

function sanitize(name: string): string {
  return name.replace(/[^\p{L}\p{N}_-]+/gu, '_').replace(/^_+|_+$/g, '') || 'sketch';
}

export function loadSketches(): Sketch[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as Sketch[];
    if (!Array.isArray(parsed)) return [];
    return parsed.filter((s) => s && Array.isArray(s.files) && s.files.length);
  } catch {
    return [];
  }
}

export function saveSketches(sketches: Sketch[]): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(sketches));
  } catch {
    /* quota exceeded: sketches stay in memory for this session */
  }
}

export function loadActiveId(): string | null {
  return localStorage.getItem(ACTIVE_KEY);
}

export function saveActiveId(id: string): void {
  localStorage.setItem(ACTIVE_KEY, id);
}

export const BLINK = `// Мигаем встроенным светодиодом и шлём строчку в монитор порта
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  Serial.println("Привет из браузера!");
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
}
`;

export const EXAMPLES: { name: string; description: string; content: string }[] = [
  {
    name: 'Blink',
    description: 'Классика: мигает встроенным светодиодом',
    content: BLINK,
  },
  {
    name: 'Кнопка',
    description: 'Читает кнопку на пине 2 и зажигает LED на 13',
    content: `const int BUTTON = 2;
const int LED = 13;

void setup() {
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
}

void loop() {
  bool pressed = digitalRead(BUTTON) == LOW;
  digitalWrite(LED, pressed ? HIGH : LOW);
}
`,
  },
  {
    name: 'Serial Plotter',
    description: 'Отправляет синусоиду и шум в монитор порта',
    content: `// Откройте монитор порта: одна строка — одно значение
#include <math.h>

unsigned long t = 0;

void setup() {
  Serial.begin(9600);
}

void loop() {
  float value = 512 + 400 * sin(t / 20.0);
  Serial.println(value, 0);
  t++;
  delay(50);
}
`,
  },
  {
    name: 'DHT11',
    description: 'Температура и влажность с датчика DHT',
    content: `#include <DHT.h>

#define DHTPIN 2
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(9600);
  dht.begin();
}

void loop() {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Датчик не отвечает");
  } else {
    Serial.print("t=");
    Serial.print(temperature, 1);
    Serial.print(" C  h=");
    Serial.print(humidity, 1);
    Serial.println(" %");
  }
  delay(2000);
}
`,
  },
  {
    name: 'OLED SSD1306',
    description: 'Выводит текст на дисплей 128×64 по I2C',
    content: `#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

Adafruit_SSD1306 display(128, 64, &Wire, -1);

void setup() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Дисплей не найден");
    for (;;) {}
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("SketchFlash");
  display.setTextSize(1);
  display.println("browser build");
  display.display();
}

void loop() {}
`,
  },
  {
    name: 'Сервопривод',
    description: 'Плавно крутит серву на пине 9',
    content: `#include <Servo.h>

Servo servo;

void setup() {
  servo.attach(9);
}

void loop() {
  for (int angle = 0; angle <= 180; angle += 2) {
    servo.write(angle);
    delay(15);
  }
  for (int angle = 180; angle >= 0; angle -= 2) {
    servo.write(angle);
    delay(15);
  }
}
`,
  },
  {
    name: 'Светомузыка (ESP)',
    description: 'ESP-скетч: плавное дыхание LED через ШИМ',
    content: `// Для ESP32 / ESP8266. Компиляции в браузере для ESP нет —
// собирайте в Arduino IDE и загружайте сюда готовый .bin.
#define LED 2

void setup() {
  pinMode(LED, OUTPUT);
}

void loop() {
  for (int duty = 0; duty <= 255; duty++) {
    analogWrite(LED, duty);
    delay(8);
  }
  for (int duty = 255; duty >= 0; duty--) {
    analogWrite(LED, duty);
    delay(8);
  }
}
`,
  },
];
