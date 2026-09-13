#pragma once

#include <Adafruit_ST7789.h>

/*
 * Adafruit_ST7789, налаштований під модуль KMR-1.8 SPI (128x160).
 *
 * Контролер: попри маркування модуля, там ST7789, а не ST7735. Перебір
 * кандидатів дав чистий потік (читабельний текст, рівне коло) лише на
 * ST7789 init(128,160); усі варіанти ST7735 (BLACKTAB / GREENTAB / REDTAB /
 * 144GREENTAB) і ST7735B давали структуровані смуги, бо їхня послідовність
 * налаштовує ST7789 лише частково.
 *
 * Далі знадобились дві поправки, обидві підтверджені перебором режимів:
 *
 * 1. Інверсія має бути ВИКЛЮЧЕНА.
 *    Таблиця generic_st7789 у бібліотеці примусово надсилає ST77XX_INVON —
 *    в коді Adafruit це підписано "hack". Справжнім ST7789 так і треба,
 *    цій панелі ні: інакше чорний фон показується білим.
 *    Тому після init() потрібен явний INVOFF.
 *
 * 2. Порядок каналів має бути BGR, а не RGB.
 *    Adafruit_ST7789::setRotation() жорстко пише ST77XX_MADCTL_RGB і вибору
 *    не дає. Просто дописати MADCTL після ініціалізації недостатньо: будь-який
 *    наступний setRotation() тихо повернув би RGB. Тому setRotation()
 *    перекрито — базовий метод виставляє межі й offsets, а ми одразу
 *    переписуємо MADCTL з тим самим набором бітів, але з BGR.
 *
 * Використання:
 *   Adafruit_ST7789_KMR18 tft(TFT_CS, TFT_DC, TFT_RST);
 *   tft.initKMR18(8000000);          // init + BGR + INVOFF + ландшафт
 */
class Adafruit_ST7789_KMR18 : public Adafruit_ST7789 {
public:
  Adafruit_ST7789_KMR18(int8_t cs, int8_t dc, int8_t rst)
      : Adafruit_ST7789(cs, dc, rst) {}

  // hz — частота SPI для малювання. ST7789 тримає близько 62 МГц на запис,
  // тож обмеження тут не жорсткі; 8 МГц узято із запасом на макетні дроти.
  void initKMR18(uint32_t hz, uint8_t rotation = 1) {
    init(128, 160);
    setSPISpeed(hz);

    // ОБОВ'ЯЗКОВО і саме тут, після init(). Adafruit_ST7789::init() для
    // нестандартного розміру йде в гілку "centered" і ЦЕНТРУЄ вікно в GRAM
    // 240x320:  _colstart = (240-128)/2 = 56,  _rowstart = (320-160)/2 = 80.
    // Ця панель показує з початку GRAM, тому з такими зсувами малювання їде
    // в середину пам'яті, за межі видимої області, і екран лишається білим.
    // Дефолт бібліотеки тут НЕ нульовий — не прибирати цей виклик.
    setOffsets(0, 0);

    setRotation(rotation);   // наше перекриття доставить BGR; воно ж перенесе
                             // зсуви в _xstart/_ystart, тому порядок важливий
    invertDisplay(false);    // скасовуємо INVON з generic_st7789
  }

  // Зсуви вікна в GRAM. У Adafruit_ST77xx вони protected, тож задаємо звідси.
  // setRotation() переносить їх у _xstart/_ystart, тому викликати ДО нього.
  void setOffsets(uint8_t colstart, uint8_t rowstart) {
    _colstart = _colstart2 = colstart;
    _rowstart = _rowstart2 = rowstart;
  }

  void setRotation(uint8_t m) override {
    Adafruit_ST7789::setRotation(m);   // межі, _xstart/_ystart, MADCTL з RGB

    // Ті самі біти, що й у базовому методі, але з BGR замість RGB
    uint8_t madctl = 0;
    switch (m & 3) {
    case 0: madctl = ST77XX_MADCTL_MX | ST77XX_MADCTL_MY; break;
    case 1: madctl = ST77XX_MADCTL_MY | ST77XX_MADCTL_MV; break;
    case 2: madctl = 0;                                   break;
    case 3: madctl = ST77XX_MADCTL_MX | ST77XX_MADCTL_MV; break;
    }
    madctl |= MADCTL_BGR;
    sendCommand(ST77XX_MADCTL, &madctl, 1);
  }

private:
  // Біт 3 MADCTL. У бібліотеці є лише ST77XX_MADCTL_RGB = 0x00,
  // константи для BGR немає.
  static const uint8_t MADCTL_BGR = 0x08;
};
