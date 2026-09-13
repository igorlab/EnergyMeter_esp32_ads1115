#include <analog_meter.h>

#include <Arduino.h>
#include <math.h>

// Кольори RGB565 локально, щоб клас не залежав від конкретного драйвера.
// Значення GREY і ORANGE узяті з оригіналу (TFT_GREY, TFT_ORANGE).
static const uint16_t C_BLACK   = 0x0000;
static const uint16_t C_WHITE   = 0xFFFF;
static const uint16_t C_GREY    = 0x5AEB;
static const uint16_t C_GREEN   = 0x07E0;
static const uint16_t C_RED     = 0xF800;
static const uint16_t C_MAGENTA = 0xF81F;
static const uint16_t C_ORANGE  = 0xFD20;

static const float DEG2RAD = 0.0174532925f;

AnalogMeter::AnalogMeter(Adafruit_GFX &gfx, float scale)
    : _g(gfx), _s(scale) {
  _osx = (int16_t)(_s * 120);
  _osy = (int16_t)(_s * 120);
}

// ----------------------------------------------------------------- текст

void AnalogMeter::centreString(const char *s, int16_t x, int16_t y, uint8_t size) {
  _g.setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  _g.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  _g.setCursor(x - bw / 2, y);
  _g.print(s);
}

void AnalogMeter::rightString(const char *s, int16_t x, int16_t y, uint8_t size) {
  _g.setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  _g.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  _g.setCursor(x - bw, y);
  _g.print(s);
}

void AnalogMeter::leftString(const char *s, int16_t x, int16_t y, uint8_t size) {
  _g.setTextSize(size);
  _g.setCursor(x, y);
  _g.print(s);
}

// ----------------------------------------------------------------- шкала

void AnalogMeter::draw() {
  const float s = _s;

  // Корпус індикатора: сіра підкладка і біле поле шкали
  _g.fillRect(0, 0, (int16_t)(s * 239), (int16_t)(s * 131), C_GREY);
  _g.fillRect(1, (int16_t)(s * 3), (int16_t)(s * 234), (int16_t)(s * 125), C_WHITE);

  _g.setTextColor(C_BLACK);

  // Засічки кожні 5° від -50 до +50, тобто повна розгортка 100°
  for (int i = -50; i < 51; i += 5) {
    int tl = 15;                                  // довга засічка

    const float sx = cosf((i - 90) * DEG2RAD);
    const float sy = sinf((i - 90) * DEG2RAD);
    int16_t x0 = (int16_t)(sx * (s * 100 + tl) + s * 120);
    int16_t y0 = (int16_t)(sy * (s * 100 + tl) + s * 150);
    int16_t x1 = (int16_t)(sx * s * 100 + s * 120);
    int16_t y1 = (int16_t)(sy * s * 100 + s * 150);

    // Координати наступної засічки — потрібні, щоб залити сектор зони
    const float sx2 = cosf((i + 5 - 90) * DEG2RAD);
    const float sy2 = sinf((i + 5 - 90) * DEG2RAD);
    const int16_t x2 = (int16_t)(sx2 * (s * 100 + tl) + s * 120);
    const int16_t y2 = (int16_t)(sy2 * (s * 100 + tl) + s * 150);
    const int16_t x3 = (int16_t)(sx2 * s * 100 + s * 120);
    const int16_t y3 = (int16_t)(sy2 * s * 100 + s * 150);

    // Зелена зона: 50..75 за шкалою
    if (i >= 0 && i < 25) {
      _g.fillTriangle(x0, y0, x1, y1, x2, y2, C_GREEN);
      _g.fillTriangle(x1, y1, x2, y2, x3, y3, C_GREEN);
    }

    // Оранжева зона: 75..100
    if (i >= 25 && i < 50) {
      _g.fillTriangle(x0, y0, x1, y1, x2, y2, C_ORANGE);
      _g.fillTriangle(x1, y1, x2, y2, x3, y3, C_ORANGE);
    }

    // Коротка засічка між кратними 25°
    if (i % 25 != 0) tl = 8;

    // Перерахунок, бо довжина засічки могла змінитись
    x0 = (int16_t)(sx * (s * 100 + tl) + s * 120);
    y0 = (int16_t)(sy * (s * 100 + tl) + s * 150);
    x1 = (int16_t)(sx * s * 100 + s * 120);
    y1 = (int16_t)(sy * s * 100 + s * 150);

    _g.drawLine(x0, y0, x1, y1, C_BLACK);

    // Підписи на кратних 25° з невеликими поправками положення
    if (i % 25 == 0) {
      x0 = (int16_t)(sx * (s * 100 + tl + 10) + s * 120);
      y0 = (int16_t)(sy * (s * 100 + tl + 10) + s * 150);
      switch (i / 25) {
      case -2: centreString("0",   x0 + 4, y0 - 4, 1); break;
      case -1: centreString("25",  x0 + 2, y0,     1); break;
      case  0: centreString("50",  x0,     y0,     1); break;
      case  1: centreString("75",  x0,     y0,     1); break;
      case  2: centreString("100", x0 - 2, y0 - 4, 1); break;
      }
    }

    // Дуга шкали, останній відрізок не малюємо
    const float ax = cosf((i + 5 - 90) * DEG2RAD);
    const float ay = sinf((i + 5 - 90) * DEG2RAD);
    x0 = (int16_t)(ax * s * 100 + s * 120);
    y0 = (int16_t)(ay * s * 100 + s * 150);
    if (i < 50) _g.drawLine(x0, y0, x1, y1, C_BLACK);
  }

  // Одиниці праворуч знизу. В оригіналі це drawString зі шрифтом 2 у
  // фіксованій точці; вбудований шрифт Adafruit ширший, тому вирівнюємо
  // по правому краю поля, інакше текст вилазить за 160 пікселів.
  _g.setTextColor(C_BLACK);
  rightString(_units, (int16_t)(s * 234) - 4, (int16_t)(s * (119 - 20)), 1);

  // Великий підпис у центрі шкали
  centreString(_units, (int16_t)(s * 120), (int16_t)(s * 75), 2);

  // Рамка корпусу
  _g.drawRect(1, (int16_t)(s * 3), (int16_t)(s * 236), (int16_t)(s * 126), C_BLACK);

  plotNeedle(0, 0);   // стрілка на нуль
}

// --------------------------------------------------------------- стрілка

void AnalogMeter::plotNeedle(int value, uint8_t msDelay) {
  const float s = _s;

  // Числове значення з непрозорим фоном, щоб само себе затирало
  _g.setTextColor(C_BLACK, C_WHITE);
  char buf[8];
  snprintf(buf, sizeof(buf), "%4d", value);
  rightString(buf, 52, (int16_t)(s * (119 - 20)), 2);

  if (value < -10) value = -10;      // обмежувачі, як механічні упори
  if (value > 110) value = 110;

  while (_oldValue != value) {
    if (_oldValue < value) _oldValue++;
    else                   _oldValue--;

    if (msDelay == 0) _oldValue = value;   // без затримки — одразу в точку

    const float sdeg = map(_oldValue, -10, 110, -150, -30);   // значення -> кут

    const float sx = cosf(sdeg * DEG2RAD);
    const float sy = sinf(sdeg * DEG2RAD);

    // Зсув основи стрілки: вона починається не в самій вісі
    const float tx = tanf((sdeg + 90) * DEG2RAD);

    // Стираємо стару стрілку
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)) - 1, (int16_t)(s * (150 - 24)),
                _osx - 1, _osy, C_WHITE);
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)),     (int16_t)(s * (150 - 24)),
                _osx,     _osy, C_WHITE);
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)) + 1, (int16_t)(s * (150 - 24)),
                _osx + 1, _osy, C_WHITE);

    // Повертаємо підпис, який стрілка могла зачепити
    _g.setTextColor(C_BLACK, C_WHITE);
    centreString(_units, (int16_t)(s * 120), (int16_t)(s * 75), 2);

    _ltx = tx;
    _osx = (int16_t)(s * (sx * 98 + 120));
    _osy = (int16_t)(s * (sy * 98 + 150));

    // Малюємо стрілку трьома лініями, щоб виглядала товщою
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)) - 1, (int16_t)(s * (150 - 24)),
                _osx - 1, _osy, C_RED);
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)),     (int16_t)(s * (150 - 24)),
                _osx,     _osy, C_MAGENTA);
    _g.drawLine((int16_t)(s * (120 + 24 * _ltx)) + 1, (int16_t)(s * (150 - 24)),
                _osx + 1, _osy, C_RED);

    // Ближче до цілі трохи гальмуємо
    if (abs(_oldValue - value) < 10) msDelay += msDelay / 5;

    delay(msDelay);
  }
}
