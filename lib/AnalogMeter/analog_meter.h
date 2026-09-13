#pragma once

#include <Adafruit_GFX.h>

/*
 * Аналоговий індикатор зі стрілкою.
 * Порт прикладу "TFT_eSPI Analogue meter" (Bodmer) на Adafruit_GFX.
 *
 * Що збережено один в один: вся геометрія шкали — кут розгортки 100°,
 * засічки кожні 5°, довгі на кратних 25°, зелена та оранжева зони,
 * дуга шкали, розміщення поділок, механіка малювання й стирання стрілки
 * трьома лініями та плавний хід до нового значення.
 *
 * Що довелося змінити: розміщення тексту. TFT_eSPI має нумеровані шрифти
 * (1, 2, 4) і методи drawCentreString / drawRightString / drawString, яких
 * в Adafruit_GFX немає. Тут вони відтворені через getTextBounds(), а розміри
 * зіставлені так: шрифт 1 -> setTextSize(1), шрифт 2 -> setTextSize(2),
 * шрифт 4 -> setTextSize(2) для центрального підпису. Вбудований шрифт
 * Adafruit ширший за еквівалентний у TFT_eSPI, тому кілька координат тексту
 * підтягнуто, інакше він вилазив за межі 160 пікселів.
 *
 * Клас працює з будь-яким Adafruit_GFX, тобто не зав'язаний на ST7789.
 *
 * Використання:
 *   AnalogMeter meter(tft);
 *   meter.draw();                  // повна шкала, стрілка на нулі
 *   meter.plotNeedle(value, 0);    // 0..100
 */
class AnalogMeter {
public:
  // scale — той самий M_SIZE з оригіналу. 0.667 дає шкалу під 160x128.
  explicit AnalogMeter(Adafruit_GFX &gfx, float scale = 0.667f);

  // Повна промальовка шкали. Достатньо один раз на старті: кінчик стрілки
  // має радіус 98*scale, а дуга шкали й засічки починаються зі 100*scale,
  // тому стрілка до них не дотягується. Оригінал прикладу перемальовував
  // шкалу раз на 2 с, і це давало видиме блимання без жодної потреби.
  void draw();

  // value у діапазоні 0..100 (приймає -10..110 як в оригіналі).
  // msDelay — затримка на кожен крок руху; 0 = миттєвий стрибок.
  void plotNeedle(int value, uint8_t msDelay = 0);

  void setUnits(const char *units) { _units = units; }

private:
  void centreString(const char *s, int16_t x, int16_t y, uint8_t size);
  void rightString(const char *s, int16_t x, int16_t y, uint8_t size);
  void leftString(const char *s, int16_t x, int16_t y, uint8_t size);

  Adafruit_GFX &_g;
  float _s;
  const char *_units = "wAh";

  float   _ltx = 0;          // збережений x основи стрілки
  int16_t _osx, _osy;        // збережені координати кінчика
  int     _oldValue = -999;  // останнє показане значення
};
