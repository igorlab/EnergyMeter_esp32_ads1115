/*
 * Serial-консоль: калібрування (ТЗ §54) і приймальні перевірки (ТЗ §69).
 *
 * Живе в loop(), тобто в задачі пріоритету 1. Команди калібрування навмисно
 * блокують loop() приблизно на секунду, поки набирається середнє — на
 * Measurement Core (пріоритет 10) це не впливає ніяк, і саме це є щоденною
 * перевіркою §71: UI має право гальмувати, ядро — ні.
 */
#pragma once

#include "measurement/core.h"

namespace ui {

class Console {
public:
    // Хук перемикання сторінок дисплея: -1 = наступна. Через вказівник на
    // функцію, а не через посилання на TftDisplay — інакше середовище
    // `measure`, де дисплея немає, тягнуло б за собою його реалізацію.
    using PageHandler = void (*)(int page);
    // Друк стану підсистеми, якої консоль не знає (картка). Той самий прийом,
    // що й з дисплеєм: вказівник на функцію, щоб середовище `measure` не
    // тягнуло за собою storage/.
    using StatusHook = void (*)();
    using CredHandler = bool (*)(const char *ssid, const char *pass);
    // Команди, яких консоль не знає, віддаються сюди. Повернути true,
    // якщо команда оброблена. Виклик з cmd = "?" — привід додати свої
    // рядки в довідку.
    using CmdHandler = bool (*)(const char *cmd, char *arg);
    // Старт тесту вирішує system-шар: чи це новий тест (з обнуленням),
    // чи продовження відновленого (§51). Консоль сама цього не знає.
    using StartHandler = void (*)();

    void begin(meas::MeasurementCore &core);
    void setPageHandler(PageHandler h) { page_cb_ = h; }
    void setSdHook(StatusHook h) { sd_cb_ = h; }
    void setNetHook(StatusHook h) { net_cb_ = h; }
    void setCredHandler(CredHandler h) { cred_cb_ = h; }
    void setCommandHandler(CmdHandler h) { extra_cb_ = h; }
    void setStartHandler(StartHandler h) { start_cb_ = h; }
    void poll();

private:
    void  printCalibration();
    void  printSnapshot();
    void  printHelp();
    void  captureVoltagePoint(uint8_t idx, float real_v);
    void  captureCurrentPoint(uint8_t idx, float real_a);
    void  handleCommand(char *line);
    void  stream();

    meas::MeasurementCore *core_ = nullptr;
    PageHandler page_cb_ = nullptr;
    StatusHook  sd_cb_ = nullptr;
    StatusHook  net_cb_ = nullptr;
    CredHandler cred_cb_ = nullptr;
    CmdHandler  extra_cb_ = nullptr;
    StartHandler start_cb_ = nullptr;
    bool     streaming_ = true;
    uint32_t next_stream_ms_ = 0;

    char    buf_[64];
    uint8_t len_ = 0;

    // Точки, захоплені командами v1/v2 та i1/i2. Друга точка одразу
    // розв'язує пару gain/offset.
    float v_measured_[2] = {0, 0}, v_real_[2] = {0, 0};
    bool  v_have_[2] = {false, false};
    float i_measured_[2] = {0, 0}, i_real_[2] = {0, 0};
    bool  i_have_[2] = {false, false};
};

}  // namespace ui
