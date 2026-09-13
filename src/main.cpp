/*
 * KMR-1.8 SPI (ST7789, 128x160) + microSD на ESP32
 *
 * УВАГА щодо контролера: попри маркування модуля, тут стоїть ST7789,
 * а не ST7735. Перебір кандидатів дав чисту картинку лише на
 * ST7789 init(128,160); усі варіанти ST7735 (BLACKTAB/GREENTAB/REDTAB/
 * 144GREENTAB) і ST7735B давали структуровані смуги — їхня
 * послідовність налаштовує ST7789 лише частково, після чого формат
 * пікселя й адресація не ті.
 *
 * Плюс дві поправки за бібліотекою: інверсію треба вимикати (generic_st7789
 * вмикає INVON примусово), а порядок каналів має бути BGR. Обидві живуть
 * у підкласі include/Adafruit_ST7789_KMR18.h.
 * Версія для PlatformIO (src/main.cpp)
 *
 * platformio.ini:
 *   lib_deps =
 *       adafruit/Adafruit GFX Library@^1.11.9
 *       adafruit/Adafruit ST7735 and ST7789 Library@^1.10.3
 *       adafruit/Adafruit BusIO@^1.16.1
 *
 * Розводка:
 *   VCC   -> 5V (VIN)   LED+ -> 3V3 (через 100 Ом, якщо на платі немає резистора)
 *     На платі стоїть AMS1117-3.3 з dropout ~1 В: від 3V3 на виході лишається
 *     близько 2.2 В, і панель із карткою недоотримують живлення. Потрібно 5 В.
 *   GND   -> GND        LED- -> GND
 *   SCL   -> GPIO18     SCK  -> GPIO18   (той самий пін)
 *   SDA   -> GPIO23     MOSI -> GPIO23   (той самий пін)
 *   CS    -> GPIO5      MISO -> GPIO19
 *   A0/DC -> GPIO17     SD_CS-> GPIO15
 *     Саме GPIO17, а не 21: піни 21 і 22 — це дефолтні I2C SDA/SCL, і вони
 *     лишені вільними під датчики, тому Wire.begin() без аргументів працює.
 *   RESET -> GPIO4
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789_KMR18.h>
#include <analog_meter.h>

#define TFT_CS   5
// Не GPIO2: на DOIT DevKit V1 там вбудований світлодіод девкіта, тобто
// зайве навантаження на лінію, і це strapping-пін.
// Не GPIO21: 21 і 22 — дефолтні I2C SDA/SCL, лишаємо їх під датчики.
#define TFT_DC  17
#define TFT_RST  4
#define SD_CS   15

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

// Частота шини для SD.
#define SD_SPI_HZ 16000000

// Частота SPI для дисплея. ST7789 тримає близько 62 МГц на запис, тож
// запас великий. Якщо на макетних дротах з'явиться сміття — знижувати.
#define TFT_SPI_HZ 16000000

// Максимальна ширина картинки в пікселях (розмір буфера рядка)
#define MAX_ROW_PX 160

Adafruit_ST7789_KMR18 tft(TFT_CS, TFT_DC, TFT_RST);
AnalogMeter           meter(tft);

bool sdReady = false;

// --- прототипи: у .cpp компілятор їх сам не генерує, на відміну від .ino ---
void listRoot();
void drawBMP(const char *filename, int16_t x, int16_t y);
static uint16_t read16(File &f);
static uint32_t read32(File &f);

// Пробудження карти перед SD.begin().
// У діагностиці SD монтувалася стабільно на всіх частотах, а в робочій
// прошивці падала з f_mount error 3 — і затримки не допомагали. Єдина
// суттєва різниця: діагностика перед монтуванням надсилала карті стандартну
// послідовність виходу в SPI-режим — 80 тактів при CS=HIGH і CMD0
// (GO_IDLE_STATE). Стара SDSC-карта без цього в SPI-режим не переходить,
// а драйвер ESP32 сам цього не робить.
static void wakeCard() {
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

  digitalWrite(SD_CS, HIGH);
  for (uint8_t i = 0; i < 10; i++) SPI.transfer(0xFF);   // 80 тактів

  digitalWrite(SD_CS, LOW);
  SPI.transfer(0xFF);
  SPI.transfer(0x40);                                    // CMD0
  SPI.transfer(0x00); SPI.transfer(0x00);
  SPI.transfer(0x00); SPI.transfer(0x00);
  SPI.transfer(0x95);                                    // CRC для CMD0

  uint8_t r1 = 0xFF;
  for (uint8_t i = 0; i < 16; i++) {
    r1 = SPI.transfer(0xFF);
    if (!(r1 & 0x80)) break;                             // чекаємо, поки зникне старший біт
  }

  digitalWrite(SD_CS, HIGH);
  SPI.transfer(0xFF);

  // CMD8 (SEND_IF_COND). У діагностиці, де карта монтувалась, пробник
  // надсилав обидві команди. З одним CMD0 відповідь була 0x05 замість 0x01,
  // тому послідовність відтворюємо повністю.
  digitalWrite(SD_CS, LOW);
  SPI.transfer(0xFF);
  SPI.transfer(0x48);                                    // CMD8
  SPI.transfer(0x00); SPI.transfer(0x00);
  SPI.transfer(0x01); SPI.transfer(0xAA);
  SPI.transfer(0x87);                                    // CRC для CMD8

  uint8_t r8 = 0xFF;
  for (uint8_t i = 0; i < 16; i++) {
    r8 = SPI.transfer(0xFF);
    if (!(r8 & 0x80)) break;
  }
  uint8_t tail[4];
  for (uint8_t i = 0; i < 4; i++) tail[i] = SPI.transfer(0xFF);

  digitalWrite(SD_CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();

  Serial.printf("wakeCard: CMD0 -> 0x%02X, CMD8 -> 0x%02X (%02X %02X %02X %02X)\n",
                r1, r8, tail[0], tail[1], tail[2], tail[3]);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Явно піднімаємо обидва CS, щоб пристрої не заважали один одному
  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS,  OUTPUT); digitalWrite(SD_CS,  HIGH);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  // Не викликати тут pinMode() на SPI_MISO: на ESP32 pinMode() на піні,
  // вже підключеному до SPI, від'єднує його від матриці периферії.

  // --- Дисплей ПЕРШИМ ---
  // Порядок критичний, перевірено обома варіантами: якщо спершу піднімати SD,
  // драйвер SD залишає шину в стані, після якого ініціалізація дисплея не
  // проходить і екран лишається білим. Тому дисплей — до будь-якої роботи з SD.
  //
  // init(128,160) + BGR + INVOFF + ландшафт. Деталі, чому саме так,
  // і що доводиться правити за бібліотекою — в include/Adafruit_ST7789_KMR18.h
  // Пауза перед першою ініціалізацією: панель після подачі живлення не
  // одразу готова приймати команди, а бібліотека своїх затримок майже не має.
  delay(150);

  tft.initKMR18(TFT_SPI_HZ);   // rotation 1 = ландшафт 160x128
  Serial.printf("дисплей: width=%d height=%d\n", tft.width(), tft.height());

  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println("Display OK");

  // --- SD ---
  // Повне пробудження CMD0 + CMD8 обов'язкове: з одним CMD0 карта лишається
  // в проміжному стані, віддає 0x05 замість 0x01, і f_mount падає з error 3.
  wakeCard();

  for (uint8_t attempt = 1; attempt <= 3 && !sdReady; attempt++) {
    if (SD.begin(SD_CS, SPI, SD_SPI_HZ)) {
      sdReady = true;
      Serial.printf("SD.begin() OK зі спроби %u\n", attempt);
    } else {
      SD.end();
      delay(400);
    }
  }

  if (sdReady) {
    tft.setCursor(4, 16);
    tft.setTextColor(ST77XX_GREEN);
    tft.printf("SD OK  %lluMB\n", SD.cardSize() / (1024ULL * 1024ULL));
    listRoot();
  } else {
    tft.setCursor(4, 16);
    tft.setTextColor(ST77XX_RED);
    tft.println("SD FAIL");
    Serial.println("SD не піднялася");
  }

  delay(1500);

  if (sdReady && SD.exists("/image.bmp")) {
    tft.fillScreen(ST77XX_BLACK);
    drawBMP("/image.bmp", 0, 0);
    delay(2000);
  }

  meter.draw();
  Serial.println("готово");
}

void loop() {
  static uint32_t nextNeedle = 0;
  static int      deg        = 0;

  const uint32_t now = millis();

  // Оригінал прикладу перемальовував шкалу повністю раз на 2 с, і це давало
  // видиме блимання. Тут воно не потрібне: кінчик стрілки має радіус 98*s,
  // а дуга шкали й засічки починаються зі 100*s, тобто стрілка до шкали не
  // дотягується і нічого не псує. Два підписи, які вона перетинає (центральний
  // і числовий), перемальовуються непрозорим фоном на кожному кроці.

  if (now >= nextNeedle) {
    nextNeedle = now + 35;              // крок стрілки, як в оригіналі

    // Тестовий сигнал: синус 0..100. Замінити на реальне значення датчика.
    deg += 4;
    if (deg >= 360) deg = 0;
    const int v = (int)(50 + 50 * sin(deg * 0.0174532925));

    meter.plotNeedle(v, 0);
  }
}

// ---------------------------------------------------------------- утиліти

void listRoot() {
  File root = SD.open("/");
  Serial.println("--- SD root ---");
  while (File f = root.openNextFile()) {
    Serial.printf("%-24s %8u B\n", f.name(), (unsigned)f.size());
    f.close();
  }
  root.close();
}

// читання BMP із SD 24-бітні нестиснені BMP. Рядок читається цілком, конвертується в RGB565
// і виливається на дисплей одним викликом writePixels().

static uint16_t read16(File &f) {
  uint16_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  return r;
}

static uint32_t read32(File &f) {
  uint32_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  ((uint8_t *)&r)[2] = f.read();
  ((uint8_t *)&r)[3] = f.read();
  return r;
}

void drawBMP(const char *filename, int16_t x, int16_t y) {
  File bmp = SD.open(filename);
  if (!bmp) {
    Serial.printf("Cannot open %s\n", filename);
    return;
  }

  if (read16(bmp) != 0x4D42) {          // сигнатура "BM"
    Serial.println("Not a BMP file");
    bmp.close();
    return;
  }

  read32(bmp);                          // розмір файлу
  read32(bmp);                          // reserved
  uint32_t imageOffset = read32(bmp);
  uint32_t headerSize  = read32(bmp);
  int32_t  bmpWidth    = (int32_t)read32(bmp);
  int32_t  bmpHeight   = (int32_t)read32(bmp);
  uint16_t planes      = read16(bmp);
  uint16_t depth       = read16(bmp);
  uint32_t compression = read32(bmp);

  Serial.printf("BMP %ldx%ld, %u bpp, header %u\n",
                (long)bmpWidth, (long)bmpHeight,
                (unsigned)depth, (unsigned)headerSize);

  if (planes != 1 || depth != 24 || compression != 0) {
    Serial.println("Unsupported BMP format (need 24-bit uncompressed)");
    bmp.close();
    return;
  }

  bool flip = true;                     // BMP зазвичай зберігається знизу вгору
  if (bmpHeight < 0) {
    bmpHeight = -bmpHeight;
    flip = false;
  }

  uint32_t rowSize = (bmpWidth * 3 + 3) & ~3;   // рядок вирівняний по 4 байти
  int16_t  w = (int16_t)min((int32_t)(tft.width()  - x), bmpWidth);
  int16_t  h = (int16_t)min((int32_t)(tft.height() - y), bmpHeight);
  if (w > MAX_ROW_PX) w = MAX_ROW_PX;
  if (w <= 0 || h <= 0) { bmp.close(); return; }

  uint8_t  raw[MAX_ROW_PX * 3];
  uint16_t line[MAX_ROW_PX];

  for (int16_t row = 0; row < h; row++) {
    uint32_t rowStart = flip
      ? imageOffset + (uint32_t)(bmpHeight - 1 - row) * rowSize
      : imageOffset + (uint32_t)row * rowSize;

    bmp.seek(rowStart);
    bmp.read(raw, (size_t)w * 3);

    for (int16_t col = 0; col < w; col++) {
      uint8_t b = raw[col * 3 + 0];
      uint8_t g = raw[col * 3 + 1];
      uint8_t r = raw[col * 3 + 2];
      line[col] = tft.color565(r, g, b);
    }

    tft.startWrite();
    tft.setAddrWindow(x, y + row, w, 1);
    tft.writePixels(line, (uint32_t)w);
    tft.endWrite();
  }

  bmp.close();
  Serial.println("BMP drawn");
}