#include "ui/console.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

using namespace meas;

namespace ui {

void Console::begin(MeasurementCore &core) {
    core_ = &core;
    printCalibration();
    printHelp();
}

void Console::printCalibration() {
    const Calibration c = core_->calibration();
    Serial.println(F("--- калібрування ---"));
    Serial.printf("  дільник K   %.6f\n", c.divider_k);
    Serial.printf("  стеля       %.2f В  (обмежує %s)\n",
                  core_->usableMaxVolts(),
                  core_->usableMaxVolts() < core_->maxBatteryVolts()
                      ? "живлення АЦП" : "шкала PGA");
    Serial.printf("  шунт        %.6f Ом\n", c.shunt_ohms);
    Serial.printf("  V: gain %.6f  offset %+.6f В   %s\n",
                  c.v_gain, c.v_offset, c.v_calibrated ? "калібровано" : "НОМІНАЛ");
    Serial.printf("  I: gain %.6f  offset %+.6f А   %s\n",
                  c.i_gain, c.i_offset, c.i_calibrated ? "калібровано" : "НОМІНАЛ");
}

void Console::captureVoltagePoint(uint8_t idx, float real_v) {
    v_measured_[idx] = averageUncalibrated(*core_, true);
    v_real_[idx] = real_v;
    v_have_[idx] = true;
    Serial.printf("V точка %u: виміряно %.5f В -> еталон %.5f В\n",
                  idx + 1, v_measured_[idx], real_v);

    if (!(v_have_[0] && v_have_[1])) return;

    Calibration c = core_->calibration();
    float g, o;
    // Мінімальний рознос точок: 1 В. Ближче — коефіцієнт визначається шумом.
    if (solveTwoPoint(v_measured_[0], v_real_[0], v_measured_[1], v_real_[1],
                      1.0f, g, o)) {
        c.v_gain = g;
        c.v_offset = o;
        c.v_calibrated = true;
        core_->setCalibration(c);
        // Слоти звільняються одразу. Інакше стара точка чекає й
        // мовчки псує наступний розв'язок — саме так ми сьогодні
        // отримали gain 0 з пари, де обидва еталони були нулями.
        v_have_[0] = v_have_[1] = false;
        Serial.printf("V калібровано: gain %.6f offset %+.6f\n", g, o);
    } else {
        Serial.println(F("точки занадто близькі — рознеси їх (напр. 4.2 В і 20 В)"));
    }
}

void Console::captureCurrentPoint(uint8_t idx, float real_a) {
    i_measured_[idx] = averageUncalibrated(*core_, false);
    i_real_[idx] = real_a;
    i_have_[idx] = true;
    Serial.printf("I точка %u: виміряно %.5f А -> еталон %.5f А\n",
                  idx + 1, i_measured_[idx], real_a);

    if (!(i_have_[0] && i_have_[1])) return;

    Calibration c = core_->calibration();
    float g, o;
    // Мінімальний рознос 50 мА, а не 200: на 0.25-ватних резисторах більше
    // за ~130 мА без батареї резисторів не витягнути, а 0 -> 130 мА вже
    // повністю визначає зсув і дає масштаб із похибкою близько відсотка.
    // Екстраполяція масштабу на амперy безпечна: і ADS1115, і шунт лінійні.
    if (solveTwoPoint(i_measured_[0], i_real_[0], i_measured_[1], i_real_[1],
                      0.05f, g, o)) {
        c.i_gain = g;
        c.i_offset = o;
        c.i_calibrated = true;
        core_->setCalibration(c);
        // Слоти звільняються одразу. Інакше стара точка чекає й
        // мовчки псує наступний розв'язок — саме так ми сьогодні
        // отримали gain 0 з пари, де обидва еталони були нулями.
        i_have_[0] = i_have_[1] = false;
        Serial.printf("I калібровано: gain %.6f offset %+.6f А\n", g, o);
        const float span = fabsf(i_real_[1] - i_real_[0]);
        if (span < 0.5f) {
            Serial.printf("УВАГА: рознос точок лише %.0f мА. Масштаб визначено "
                          "слабко,\n", span * 1000.0f);
            Serial.println(F("       перевір на робочому струмі, коли буде навантаження."));
        }
    } else {
        Serial.println(F("точки занадто близькі: потрібно щонайменше 50 мА різниці"));
        Serial.println(F("(добра пара — 0 А при розірваному колі і ~130 мА)"));
    }
}

void Console::printHelp() {
    Serial.println(F(
        "\n--- команди ---\n"
        "  ?            ця довідка\n"
        "  s            повний знімок стану\n"
        "  q            увімкнути/вимкнути потік рядків\n"
        "  run / stop   старт / пауза інтегрування Ah,Wh\n"
        "  zero         обнулити Ah, Wh, час\n"
        "  cal          показати калібрування\n"
        "  v1 <В>       точка 1 калібрування напруги (еталон вольтметра)\n"
        "  v2 <В>       точка 2 -> розв'язати gain/offset\n"
        "  i1 <А>       точка 1 калібрування струму (напр. 0 при розриві)\n"
        "  i2 <А>       точка 2 -> розв'язати gain/offset\n"
        "  iz           обнулити offset струму (при знятому навантаженні)\n"
        "  iflip        перевернути полярність струму (розряд має бути +)\n"
        "  ical G O     задати gain та offset струму напряму (А)\n"
        "  div <K>      коефіцієнт дільника (номінал 10.0)\n"
        "  sh <Ом>      опір шунта (номінал 0.010)\n"
        "  save/load/erase   калібрування в NVS\n"
        "  fault        показати та зняти аварію\n"
        "  page [0..2]  сторінка дисплея: live / test / diag\n"
        "  net          адреса веб-інтерфейсу\n"
        "  wifi S P     зберегти мережу в NVS (без аргументів — стерти)\n"
        "  sd           стан картки та список тестів\n"
        "  i2c          скан шини: які адреси відповідають\n"
        "  sim on|off   синтетичні дані замість АЦП (тільки вручну)\n"
        "  ain          що на кожному вході AIN0..AIN3 окремо\n"
        "  avg [1..32]  ковзне середнє по V та I (1 — вимкнути)\n"
        "  pga [0..4]   зафіксувати діапазон V (без аргументу — автоматика)\n"
        "               0=+/-0.256 1=+/-0.512 2=+/-1.024 3=+/-2.048 4=+/-4.096\n"));
    if (extra_cb_) extra_cb_("?", nullptr);
}

void Console::printSnapshot() {
    Measurement m;
    Diagnostics d;
    const bool ok = core_->latest(m);
    core_->diagnostics(d);

    if (!ok) {
        Serial.println(F("ще немає жодного вимірювання"));
        return;
    }

    Serial.println(F("--- знімок ---"));
    Serial.printf("  V   %9.4f В\n", m.voltage);
    Serial.printf("  I   %9.4f А   %s\n", m.current,
                  fabsf(m.current) < 0.005f ? "IDLE"
                      : ((m.flags & FLAG_CHARGE) ? "CHARGE" : "DISCHARGE"));
    Serial.printf("  P   %9.4f Вт\n", m.power);
    Serial.printf("  Ah  %9.5f    Wh %9.5f\n", m.Ah, m.Wh);
    TestStats st;
    core_->stats(st);
    if (!isnan(st.ah_cutoff)) {
        Serial.printf("  до cutoff: %.5f Ah  %.5f Wh  за %.0f с\n",
                      st.ah_cutoff, st.wh_cutoff, st.s_cutoff);
    }
    Serial.printf("  час %9.1f с   %s\n", core_->elapsedSeconds(),
                  core_->integrating() ? "RUNNING" : "PAUSED");
    Serial.printf("  seq %u  flags 0x%02X  fault %s\n",
                  m.sequence, m.flags, faultName(core_->fault()));
    Serial.printf("  ADC: V raw %6d, діапазон %u (+/-%.3f В)   I raw %6d\n",
                  d.v_raw, d.v_pga, d.v_fs, d.i_raw);
    Serial.printf("  до калібрування: V %.5f  I %.5f\n", d.v_uncal, d.i_uncal);
    Serial.printf("  цикл %u мкс (макс %u)  помилок ADC %u  змін діапазону %u  втрачено %u\n",
                  d.cycle_us, d.cycle_max_us, d.adc_errors, d.range_changes, d.dropped);
}

void Console::handleCommand(char *line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;

    char *arg = strchr(line, ' ');
    float value = 0;
    if (arg) {
        *arg++ = '\0';
        value = atof(arg);
    }

    if (!strcmp(line, "?") || !strcmp(line, "help")) { printHelp(); }
    else if (!strcmp(line, "s")) { printSnapshot(); }
    else if (!strcmp(line, "q")) {
        streaming_ = !streaming_;
        Serial.printf("потік %s\n", streaming_ ? "увімкнено" : "вимкнено");
    }
    else if (!strcmp(line, "run")) {
        core_->clearFault();          // залиплу аварію старт знімає, див. web_ui
        if (start_cb_) start_cb_();
        else core_->setIntegrating(true);
        Serial.println(F("RUNNING"));
    }
    else if (!strcmp(line, "stop")) { core_->setIntegrating(false); Serial.println(F("PAUSED")); }
    else if (!strcmp(line, "zero")) {
        core_->clearFault();
        core_->resetIntegration();
        Serial.println(F("Ah/Wh обнулено"));
    }
    else if (!strcmp(line, "cal")) { printCalibration(); }
    else if (!strcmp(line, "v1")) { captureVoltagePoint(0, value); }
    else if (!strcmp(line, "v2")) { captureVoltagePoint(1, value); }
    else if (!strcmp(line, "i1")) { captureCurrentPoint(0, value); }
    else if (!strcmp(line, "i2")) { captureCurrentPoint(1, value); }
    else if (!strcmp(line, "ical")) {
        // Потрібно, коли дві точки §54 зняті в різні моменти й через
        // команди i1/i2 їх уже не захопити: коефіцієнти рахуються зовні
        // й задаються готовими.
        char *second = arg ? strchr(arg, ' ') : nullptr;
        if (!arg || !second) {
            Serial.println(F("формат: ical <gain> <offset у А>"));
        } else {
            *second++ = '\0';
            Calibration c = core_->calibration();
            c.i_gain = atof(arg);
            c.i_offset = atof(second);
            c.i_calibrated = true;
            core_->setCalibration(c);
            Serial.printf("I: gain %.6f  offset %+.6f А (не забудь save)\n",
                          c.i_gain, c.i_offset);
        }
    }
    else if (!strcmp(line, "iflip")) {
        // Полярність живе в знаку i_gain, а не окремим полем: так вона
        // зберігається в NVS разом із рештою калібрування, і двоточкове
        // калібрування §54 виставляє її саме тим самим механізмом.
        Calibration c = core_->calibration();
        c.i_gain = -c.i_gain;
        core_->setCalibration(c);
        Serial.printf("полярність струму перевернуто, i_gain %+.6f\n", c.i_gain);
        Serial.println(F("(додатний струм = РОЗРЯД; не забудь save)"));
    }
    else if (!strcmp(line, "iz")) {
        Calibration c = core_->calibration();
        const float measured = averageUncalibrated(*core_, false);
        c.i_offset = -c.i_gain * measured;
        core_->setCalibration(c);
        Serial.printf("offset струму = %+.6f А (зміщення шунта %.6f А)\n",
                      c.i_offset, measured);
    }
    else if (!strcmp(line, "div")) {
        Calibration c = core_->calibration();
        if (value > 0.0f) { c.divider_k = value; core_->setCalibration(c); }
        printCalibration();
    }
    else if (!strcmp(line, "sh")) {
        Calibration c = core_->calibration();
        if (value > 0.0f) { c.shunt_ohms = value; core_->setCalibration(c); }
        printCalibration();
    }
    else if (!strcmp(line, "save")) {
        Serial.println(saveCalibration(core_->calibration()) ? F("збережено в NVS")
                                                            : F("помилка запису NVS"));
    }
    else if (!strcmp(line, "load")) {
        Calibration c;
        if (loadCalibration(c)) { core_->setCalibration(c); printCalibration(); }
        else Serial.println(F("у NVS немає калібрування"));
    }
    else if (!strcmp(line, "erase")) {
        Serial.println(eraseCalibration() ? F("NVS очищено") : F("помилка NVS"));
    }
    else if (!strcmp(line, "page")) {
        if (!page_cb_) Serial.println(F("дисплея немає в цій прошивці"));
        else page_cb_(arg ? static_cast<int>(value) : -1);
    }
    else if (!strcmp(line, "net")) {
        if (net_cb_) net_cb_();
        else Serial.println(F("мережі немає в цій прошивці"));
    }
    else if (!strcmp(line, "wifi")) {
        // Пароль живе тільки в NVS: у коді проєкту його немає й не буде.
        char *pass = arg ? strchr(arg, ' ') : nullptr;
        if (pass) *pass++ = '\0';
        if (!cred_cb_) {
            Serial.println(F("мережі немає в цій прошивці"));
        } else if (cred_cb_(arg, pass)) {
            Serial.println(arg && *arg ? F("мережу збережено, перезавантаж плату")
                                       : F("мережу стерто, буде точка доступу"));
        } else {
            Serial.println(F("помилка запису NVS"));
        }
    }
    else if (!strcmp(line, "sd")) {
        if (sd_cb_) sd_cb_();
        else Serial.println(F("картки немає в цій прошивці"));
    }
    else if (!strcmp(line, "i2c")) {
        // Wire на ESP32 має власний замок, тож скан паралельно з вимірювальною
        // задачею безпечний; кілька циклів просто отримають ADC_ERROR.
        Serial.println(F("скан I2C..."));
        uint8_t found = 0;
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                Serial.printf("  0x%02X%s\n", a,
                              (a >= 0x48 && a <= 0x4B) ? "  <- схоже на ADS1115" : "");
                found++;
            }
        }
        if (!found) {
            Serial.println(F("  порожньо."));
            Serial.println(F("  перевір: живлення модуля, GND, SDA=21, SCL=22,"));
            Serial.println(F("  ADDR на GND (0x48), підтяжки на шині"));
        }
    }
    else if (!strcmp(line, "sim")) {
        const bool on = arg && !strcmp(arg, "on");
        core_->setSimulate(on);
        Serial.println(on ? F("СИМУЛЯЦІЯ УВІМКНЕНА — дані синтетичні")
                          : F("симуляція вимкнена, дані з АЦП"));
    }
    else if (!strcmp(line, "ain")) {
        core_->requestPinScan();
        delay(300);
        float v[4];
        const bool ok = core_->pinScan(v);
        Serial.println(F("однобічно відносно GND, діапазон +/-4.096 В:"));
        for (uint8_t ch = 0; ch < 4; ch++) {
            Serial.printf("  AIN%u  %+9.4f В  %s\n", ch, v[ch],
                          isnan(v[ch]) ? "помилка"
                          : (fabsf(v[ch]) < 0.05f ? "коло нуля — схоже на sense шунта"
                          : (v[ch] > 3.3f ? "!! вище живлення" : "")));
        }
        Serial.printf("  диференціал AIN0-AIN1 = %+.5f В (%.2f мА на 10 mΩ)\n",
                      v[0] - v[1], (v[0] - v[1]) / 0.010f * 1000.0f);
        Serial.printf("  диференціал AIN2-AIN3 = %+.5f В\n", v[2] - v[3]);
        if (!ok) Serial.println(F("  частина конверсій не вдалася"));
    }
    else if (!strcmp(line, "avg")) {
        if (arg) core_->setAverage(static_cast<uint8_t>(value));
        const uint8_t n = core_->average();
        Serial.printf("усереднення %u %s", n, n > 1 ? "зразків" : "(вимкнено)");
        if (n > 1) Serial.printf(", вікно %.0f мс, затримка %.0f мс",
                                 n * 31.25f, (n - 1) * 0.5f * 31.25f);
        Serial.println();
    }
    else if (!strcmp(line, "pga")) {
        const int idx = arg ? static_cast<int>(value) : -1;
        core_->setVoltageRange(idx);
        if (idx < 0) Serial.println(F("діапазон V: автоматика"));
        else         Serial.printf("діапазон V зафіксовано: %d\n", idx);
    }
    else if (!strcmp(line, "fault")) {
        Serial.printf("аварія: %s\n", faultName(core_->fault()));
        core_->clearFault();
    }
    else if (extra_cb_ && extra_cb_(line, arg)) {
        // оброблено зовнішнім обробником
    }
    else {
        Serial.printf("невідома команда: %s (\"?\" — довідка)\n", line);
    }
}

void Console::stream() {
    if (!streaming_ || millis() < next_stream_ms_) return;
    next_stream_ms_ = millis() + 500;   // 2 Гц, незалежно від 32 SPS ядра

    Measurement m;
    if (!core_->latest(m)) return;

    Serial.printf("V %8.4f  I %8.4f  P %8.3f  Ah %8.5f  Wh %8.5f  t %7.1f  %s%s\n",
                  m.voltage, m.current, m.power, m.Ah, m.Wh,
                  core_->elapsedSeconds(),
                  fabsf(m.current) < 0.005f ? "IDLE"
                      : ((m.flags & FLAG_CHARGE) ? "CHARGE" : "DISCHARGE"),
                  core_->fault() != Fault::NONE ? "  FAULT" : "");
}

void Console::poll() {
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            buf_[len_] = '\0';
            handleCommand(buf_);
            len_ = 0;
        } else if (len_ < sizeof(buf_) - 1) {
            buf_[len_++] = ch;
        }
    }
    stream();
}

}  // namespace ui
