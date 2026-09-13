/*
 * KMR-1.8 SPI (128x160) на ESP32 — контрольна мішень для дисплея.
 * Збірка:  pio run -e diag -t upload -t monitor
 *
 * Підсумок діагностики, до якого прийшли:
 *   - контролер ST7789, не ST7735, попри маркування модуля;
 *   - інверсію треба вимикати: generic_st7789 вмикає INVON примусово;
 *   - порядок каналів BGR, не RGB;
 *   - зсуви лишаються нульовими, тобто дефолтними;
 *   - живлення, частота SPI, пін DC і карта на шині причиною НЕ були.
 * Усе це зашито в include/Adafruit_ST7789_KMR18.h, там же й пояснення.
 *
 * Ця прошивка малює мішень, на якій видно кожен із можливих дефектів:
 *   смуги — порядок RGB і інверсію
 *   рамка точно по межі — зсув вікна і незакриті смужки
 *   коло — геометрію та адресацію
 *   текст — чистоту потоку: якщо читається, жоден біт не проковзує
 *
 * SD тут свідомо не задіяна: SD_CS тримається HIGH. Тести картки —
 * під DIAG_SD_TESTS.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789_KMR18.h>

#define TFT_CS   5
// Не GPIO2 (світлодіод девкіта + strapping) і не GPIO21 (дефолтний I2C SDA)
#define TFT_DC  17
#define TFT_RST  4
#define SD_CS   15

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

#define TFT_SPI_HZ 8000000

Adafruit_ST7789_KMR18 tft(TFT_CS, TFT_DC, TFT_RST);

// ------------------------------------------------------------------------

static void drawTarget() {
  const int16_t w = tft.width(), h = tft.height();

  tft.fillScreen(ST77XX_BLACK);

  const uint16_t bars[] = {ST77XX_RED,    ST77XX_GREEN, ST77XX_BLUE,
                           ST77XX_YELLOW, ST77XX_CYAN,  ST77XX_MAGENTA,
                           ST77XX_WHITE};
  const int16_t bw = w / 7;
  for (int8_t i = 0; i < 7; i++) tft.fillRect(i * bw, 0, bw, 20, bars[i]);

  tft.drawRect(0, 0, w, h, ST77XX_WHITE);
  tft.fillCircle(w - 26, h - 26, 18, ST77XX_CYAN);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(6, 30);
  tft.println("ST7789");
  tft.setTextSize(1);
  tft.setCursor(6, 54);
  tft.printf("%d x %d  BGR  INVOFF", w, h);
  tft.setCursor(6, 68);
  tft.printf("SPI %lu Hz", (unsigned long)TFT_SPI_HZ);
}

// ------------------------------------------------------- допоміжне по SD
// Для запитів на кшталт "покажи вміст картки" чи "дай контент файлу".
// За замовчуванням не компілюється, щоб SD не сиділа на шині під час тестів.

#ifdef DIAG_SD_TESTS
static void listDir(File dir, uint8_t depth) {
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    for (uint8_t i = 0; i < depth; i++) Serial.print("  ");
    if (f.isDirectory()) {
      Serial.printf("[DIR] %s\n", f.name());
      if (depth < 4) listDir(f, depth + 1);
    } else {
      Serial.printf("%-30s %9u B\n", f.name(), (unsigned)f.size());
    }
    f.close();
  }
}

static void listCard() {
  Serial.println();
  Serial.println("=== Вміст картки ===");
  if (!SD.begin(SD_CS, SPI, 8000000)) {
    Serial.println("  не змонтувалась");
    return;
  }
  Serial.printf("  тип %d, всього %lluMB, зайнято %lluMB\n",
                (int)SD.cardType(),
                SD.totalBytes() / (1024ULL * 1024ULL),
                SD.usedBytes()  / (1024ULL * 1024ULL));
  File root = SD.open("/");
  if (!root) { Serial.println("  корінь не відкрився"); SD.end(); return; }
  listDir(root, 1);
  root.close();
  SD.end();
  Serial.println("=== кінець списку ===");
}

// Маркери потрібні, щоб хост міг надійно вирізати тіло файлу з-під решти виводу
static void dumpFile(const char *path) {
  if (!SD.begin(SD_CS, SPI, 8000000)) {
    Serial.println("DUMP: карта не змонтувалась");
    return;
  }
  File f = SD.open(path);
  if (!f) {
    Serial.printf("DUMP: %s не відкрився\n", path);
    SD.end();
    return;
  }
  Serial.printf("<<<BEGIN %s %u bytes>>>\n", path, (unsigned)f.size());
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (!n) break;
    Serial.write(buf, n);
  }
  f.close();
  Serial.println();
  Serial.println("<<<END>>>");
  SD.end();
}
#endif

// ------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("##### ST7789: контрольна мішень #####");
  Serial.printf("SCK=%d  MISO=%d  MOSI=%d  CS=%d  DC=%d  RST=%d\n",
                SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS, TFT_DC, TFT_RST);
  Serial.println("Конфігурація: ST7789 init(128,160), BGR, INVOFF, rotation 1.");
  Serial.println("SD не задіяна: SD_CS тримається HIGH.");

  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS,  OUTPUT); digitalWrite(SD_CS,  HIGH);   // щоб карта не лізла на шину

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  tft.initKMR18(TFT_SPI_HZ);
  Serial.printf("дисплей готовий, %d x %d\n", tft.width(), tft.height());

#ifdef DIAG_SD_TESTS
  listCard();
#endif

  drawTarget();
  Serial.println("мішень намальована");
}

void loop() {
}
