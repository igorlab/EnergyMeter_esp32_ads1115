#include "ui/web_ui.h"

#include <Preferences.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include "ui/web_pages.h"

using namespace meas;
using namespace storage;

namespace ui {

namespace {
constexpr char kNamespace[] = "wifi";
::WebServer *http = nullptr;
WebUi *self = nullptr;

// Скільки record читається з картки за один захват шини. 64 × 32 = 2 КБ:
// достатньо, щоб передача була ефективною, і достатньо мало, щоб дисплей
// не чекав помітно (§33.1).
constexpr size_t kChunkRecords = 64;

const char *stateName(uint8_t flags, float current) {
    if (fabsf(current) < 0.005f) return "IDLE";
    return (flags & FLAG_CHARGE) ? "CHARGE" : "DISCHARGE";
}
}  // namespace

bool WebUi::take(uint32_t ms) {
    if (!bus_) return true;
    return xSemaphoreTake(bus_, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void WebUi::give() { if (bus_) xSemaphoreGive(bus_); }

bool WebUi::saveCredentials(const char *ssid, const char *pass) {
    Preferences p;
    if (!p.begin(kNamespace, false)) return false;
    if (!ssid || !*ssid) {
        p.clear();
    } else {
        p.putString("ssid", ssid);
        p.putString("pass", pass ? pass : "");
    }
    p.end();
    return true;
}

bool WebUi::loadCredentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    Preferences p;
    if (!p.begin(kNamespace, true)) return false;
    const size_t n = p.getString("ssid", ssid, ssid_len);
    p.getString("pass", pass, pass_len);
    p.end();
    return n > 0 && ssid[0] != '\0';
}

IPAddress WebUi::ip() const { return ap_mode_ ? WiFi.softAPIP() : WiFi.localIP(); }

bool WebUi::connectSta() {
    char ssid[33] = {0}, pass[65] = {0};
    if (!loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) return false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    const uint32_t deadline = millis() + cfg_.sta_timeout_ms;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        return false;
    }
    strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    ap_mode_ = false;
    return true;
}

void WebUi::startAp() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg_.ap_ssid, cfg_.ap_pass);
    strncpy(ssid_, cfg_.ap_ssid, sizeof(ssid_) - 1);
    ap_mode_ = true;
}

bool WebUi::begin(const Config &cfg, MeasurementCore &core, SdLogger &sd,
                  SemaphoreHandle_t bus) {
    cfg_ = cfg;
    core_ = &core;
    sd_ = &sd;
    bus_ = bus;
    self = this;

    cmds_ = xQueueCreate(4, sizeof(uint8_t));
    if (!cmds_) return false;

    return xTaskCreatePinnedToCore(&WebUi::taskEntry, "web", cfg_.task_stack, this,
                                   cfg_.task_priority, &task_, cfg_.task_core) == pdPASS;
}

void WebUi::taskEntry(void *arg) { static_cast<WebUi *>(arg)->run(); }

void WebUi::run() {
    if (!connectSta()) startAp();

    http = new ::WebServer(cfg_.port);
    routes();
    http->begin();
    up_ = true;

    for (;;) {
        http->handleClient();
        // Wi-Fi упав — піднімаємо точку доступу, вимірювання це не зачіпає (§64).
        if (!ap_mode_ && WiFi.status() != WL_CONNECTED) {
            up_ = false;
            if (!connectSta()) startAp();
            up_ = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void WebUi::routes() {
    http->on("/", []() { self->handleRoot(); });
    http->on("/style.css", []() {
        http->send_P(200, "text/css", STYLE_CSS);
    });
    http->on("/chart.js", []() {
        http->send_P(200, "application/javascript", CHART_JS);
    });
    http->on("/tests", []() { self->handleTests(); });
    http->on("/test", []() { self->handleTest(); });
    http->on("/csv", []() { self->handleCsv(); });
    http->on("/api/status", []() { self->handleStatus(); });
    http->on("/api/start", HTTP_POST, []() { self->handleCommand(CMD_START); });
    http->on("/api/stop",  HTTP_POST, []() { self->handleCommand(CMD_STOP); });
    http->on("/api/reset", HTTP_POST, []() { self->handleCommand(CMD_RESET); });
    http->on("/settings", []() { self->handleSettingsPage(); });
    http->on("/api/settings", HTTP_GET, []() { self->handleSettingsGet(); });
    http->on("/api/settings", HTTP_POST, []() { self->handleSettingsPost(); });
    http->on("/api/nvs", HTTP_POST, []() { self->handleNvs(); });
    http->on("/api/cal", HTTP_POST, []() { self->handleCalPoint(); });
    http->on("/api/rm", HTTP_POST, []() { self->handleRemove(); });
    http->on("/bin", []() { self->handleBin(); });
    http->onNotFound([]() { http->send(404, "text/plain", "404"); });
}

void WebUi::handleRoot() { http->send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
void WebUi::handleSettingsPage() {
    http->send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
}

void WebUi::handleSettingsGet() {
    const Calibration c = core_->calibration();
    Diagnostics d{};
    core_->diagnostics(d);

    char buf[900];
    snprintf(buf, sizeof(buf),
             "{\"div\":%.6f,\"vgain\":%.6f,\"voffset\":%.6f,\"ceiling\":%.2f,"
             "\"shunt\":%.6f,\"igain\":%.6f,\"ioffset\":%.6f,"
             "\"chem\":\"%s\",\"cells\":%u,\"cap\":%.4f,"
             "\"cutoff\":%.2f,\"floor\":%.2f,\"imax\":%.2f,\"tmax\":%.0f,"
             "\"rprof\":%.4f,\"rint\":%.4f,\"ocv\":%.4f,"
             "\"avg\":%u,\"cycle\":%.1f,\"cycleMax\":%.1f,"
             "\"pga\":%u,\"pgaFs\":%.3f,"
             "\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":%s,"
             "\"sd\":%s,\"card\":%llu,\"used\":%llu,\"free\":%llu,"
             "\"test\":%u,\"heap\":%u,\"heapTotal\":%u,"
             "\"vraw\":%.5f,\"iraw\":%.5f}",
             c.divider_k, c.v_gain, c.v_offset, core_->usableMaxVolts(),
             c.shunt_ohms, c.i_gain, c.i_offset,
             strcmp(prof_name_, "LiFePO4") == 0 ? "lfp" : "li",
             prof_cells_, prof_cap_,
             prof_cutoff_, prof_floor_, prof_imax_, prof_tmax_,
             prof_r_, r_int_, isnan(ocv_) ? 0.0f : ocv_,
             core_->average(), d.cycle_us / 1000.0f, d.cycle_max_us / 1000.0f,
             d.v_pga, d.v_fs,
             ssid_, ip().toString().c_str(), ap_mode_ ? "true" : "false",
             sd_->cardPresent() ? "true" : "false", sd_->cardSizeMB(),
             sd_->usedMB(), sd_->freeMB(),
             sd_->testId(), ESP.getFreeHeap() / 1024u,
             ESP.getHeapSize() / 1024u,
             d.v_uncal, d.i_uncal);
    http->send(200, "application/json", buf);
}

void WebUi::handleSettingsPost() {
    Calibration c = core_->calibration();
    bool cal_changed = false;

    if (http->hasArg("div")) { c.divider_k = http->arg("div").toFloat(); cal_changed = true; }
    if (http->hasArg("shunt")) { c.shunt_ohms = http->arg("shunt").toFloat(); cal_changed = true; }
    if (http->hasArg("igain")) { c.i_gain = http->arg("igain").toFloat(); cal_changed = true; }
    if (http->hasArg("ioffset")) { c.i_offset = http->arg("ioffset").toFloat(); cal_changed = true; }

    // Захист від нуля: шунт 0 Ом зробив би струм нескінченним, дільник 0 —
    // напругу. Обидва поля редагуються вручну, тож перевірка обов'язкова.
    if (!(c.divider_k > 0.0f) || !(c.shunt_ohms > 0.0f)) {
        http->send(400, "text/plain", "divider and shunt must be greater than zero");
        return;
    }
    if (cal_changed) core_->setCalibration(c);

    if (http->hasArg("avg")) core_->setAverage(http->arg("avg").toInt());

    if (prof_cb_ && http->hasArg("chem")) {
        prof_cb_(http->arg("chem").c_str(), http->arg("cells").toInt(),
                 http->arg("cap").toFloat(), http->arg("imax").toFloat(),
                 http->arg("tmax").toFloat(), http->arg("rint").toFloat());
    }
    http->send(200, "text/plain", "applied (remember to save to NVS)");
}

void WebUi::handleNvs() {
    const String op = http->arg("op");
    if (op == "save") {
        http->send(200, "text/plain", saveCalibration(core_->calibration())
                       ? "saved to NVS" : "write failed");
    } else if (op == "load") {
        Calibration c;
        if (loadCalibration(c)) { core_->setCalibration(c); http->send(200, "text/plain", "loaded from NVS"); }
        else http->send(200, "text/plain", "no calibration in NVS");
    } else if (op == "erase") {
        http->send(200, "text/plain", eraseCalibration() ? "NVS erased" : "failed");
    } else {
        http->send(400, "text/plain", "op must be save, load or erase");
    }
}

// §54 з веба. Точка захоплюється як середнє по сирих відліках, розв'язок
// запускається, щойно обидві точки каналу заповнені.
void WebUi::handleCalPoint() {
    const bool volt = http->arg("ch") == "v";
    const int slot = http->arg("slot").toInt() & 1;
    const float ref = http->arg("ref").toFloat();
    const uint8_t ch = volt ? 0 : 1;

    cal_m_[ch][slot] = averageUncalibrated(*core_, volt);
    cal_r_[ch][slot] = ref;
    cal_have_[ch][slot] = true;

    char msg[160];
    if (!(cal_have_[ch][0] && cal_have_[ch][1])) {
        snprintf(msg, sizeof(msg), "point %d: measured %.5f -> reference %.5f",
                 slot + 1, cal_m_[ch][slot], ref);
        http->send(200, "text/plain", msg);
        return;
    }

    float g, o;
    const float span = volt ? 1.0f : 0.05f;
    if (!solveTwoPoint(cal_m_[ch][0], cal_r_[ch][0], cal_m_[ch][1], cal_r_[ch][1],
                       span, g, o)) {
        snprintf(msg, sizeof(msg), "points too close: need a span of at least %.2f",
                 span);
        http->send(200, "text/plain", msg);
        return;
    }

    Calibration c = core_->calibration();
    if (volt) { c.v_gain = g; c.v_offset = o; c.v_calibrated = true; }
    else      { c.i_gain = g; c.i_offset = o; c.i_calibrated = true; }
    core_->setCalibration(c);
    cal_have_[ch][0] = cal_have_[ch][1] = false;   // слоти звільняються
    snprintf(msg, sizeof(msg), "calibrated: gain %.6f offset %+.6f", g, o);
    http->send(200, "text/plain", msg);
}
void WebUi::handleTest() { http->send_P(200, "text/html; charset=utf-8", TEST_HTML); }

// §37. Тільки читає останній готовий Measurement — жодних вимірювань за
// запитом HTTP (§36).
void WebUi::handleStatus() {
    Measurement m{};
    core_->latest(m);
    Diagnostics d{};
    core_->diagnostics(d);
    TestStats stt{};
    core_->stats(stt);

    char buf[520];
    snprintf(buf, sizeof(buf),
             "{\"voltage\":%.4f,\"current\":%.4f,\"power\":%.4f,"
             "\"Ah\":%.5f,\"Wh\":%.5f,\"temperature\":%.1f,"
             "\"mode\":\"%s\",\"running\":%s,\"elapsed\":%.1f,\"uptime\":%.3f,"
             "\"fault\":\"%s\",\"simulated\":%s,\"sd\":%s,\"logging\":%s,"
             "\"test\":%u,\"seq\":%u,"
             "\"soc\":%.1f,"
             "\"profile\":\"%s\",\"cutoff\":%.2f,\"capacity\":%.4f,\"rint\":%.4f,"
             "\"ahCutoff\":%.5f,\"whCutoff\":%.5f}",
             m.voltage, m.current, m.power, m.Ah, m.Wh,
             isnan(m.temperature) ? 0.0f : m.temperature,
             stateName(m.flags, m.current),
             (m.flags & FLAG_INTEGRATING) ? "true" : "false",
             core_->elapsedSeconds(), m.timestamp_us / 1e6,
             faultName(core_->fault()),
             d.simulated ? "true" : "false",
             sd_->cardPresent() ? "true" : "false",
             sd_->logging() ? "true" : "false",
             sd_->testId(), m.sequence,
             isnan(soc_) ? -1.0f : soc_,
             prof_name_, prof_cutoff_, prof_cap_, r_int_,
             isnan(stt.ah_cutoff) ? -1.0f : stt.ah_cutoff,
             isnan(stt.wh_cutoff) ? -1.0f : stt.wh_cutoff);
    http->send(200, "application/json", buf);
}

// Обробник лише ставить команду в чергу й одразу відповідає. Нічого з
// вимірюванням, карткою чи станом тесту тут не відбувається (§39).
void WebUi::handleCommand(Cmd c) {
    const uint8_t v = static_cast<uint8_t>(c);
    const bool queued = cmds_ && xQueueSend(cmds_, &v, 0) == pdTRUE;
    http->send(queued ? 202 : 503, "application/json",
               queued ? "{\"queued\":true}" : "{\"queued\":false}");
}

// Викликається з loopTask. Тут і тільки тут команда стає дією.
void WebUi::pump() {
    if (!cmds_) return;
    uint8_t v;
    while (xQueueReceive(cmds_, &v, 0) == pdTRUE) {
        switch (static_cast<Cmd>(v)) {
            case CMD_START:
                // Старт знімає залиплу аварію. Інакше вона переїжджає в новий
                // тест і дає безглузді поєднання на кшталт NO_BATTERY при
                // CHARGE — рівно те, що ми зловили на тесті #20.
                //
                // Замаскувати справжню проблему це не може: якщо умова досі
                // виконується, safety підніме аварію знову за свою витримку
                // (секунда для напруги, 94 мс для решти).
                core_->clearFault();
                if (start_cb_) start_cb_();
                else core_->setIntegrating(true);
                break;
            case CMD_STOP:
                core_->setIntegrating(false);
                break;
            case CMD_RESET:
                core_->clearFault();
                // Спершу зупинити, потім обнулити. Інакше SD-логер побачить
                // короткий провал FLAG_INTEGRATING і розріже файл тесту
                // навпіл, а користувач цього не просив.
                core_->setIntegrating(false);
                core_->resetIntegration();
                break;
        }
    }
}

// §38 /tests — перелік файлів на картці з місцем, завантаженням і видаленням.
void WebUi::handleTests() {
    String page = F("<!doctype html><meta charset=utf-8>"
                    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
                    "<title>Tests</title><link rel=stylesheet href=/style.css>"
                    "<header><h1>TESTS</h1><nav><a href=/>dashboard</a>"
                    "<a href=/tests>tests</a><a href=/settings>settings</a></nav>"
                    "<span id=st class=dim></span></header><main>");

    if (!sd_->cardPresent()) {
        page += F("<p class=dim>SD: NOT PRESENT &middot; logging: DISABLED</p>");
    } else if (!take(3000)) {
        page += F("<p class=bad>bus busy, try again</p>");
    } else {
        page += "<p class=dim>card " + String((uint32_t)sd_->cardSizeMB()) +
                " MB &middot; used " + String((uint32_t)sd_->usedMB()) +
                " &middot; free <b>" + String((uint32_t)sd_->freeMB()) + " MB</b></p>";

        File dir = SD.open("/BATTERY_TESTS");
        page += F("<ul class=files>");
        uint16_t n = 0;
        while (File f = dir.openNextFile()) {
            const String name = f.name();
            const size_t sz = f.size();
            f.close();
            if (!name.endsWith(".BIN") && !name.endsWith(".bin")) continue;
            const int id = atoi(name.c_str() + 5);   // TEST_0001.BIN
            const String sid = String(id);
            page += "<li><a href=/test?id=" + sid + ">" + name + "</a>";
            page += "<a class=dim href=/csv?id=" + sid + ">CSV</a>";
            page += "<a class=dim href=\"/csv?id=" + sid + "&dl=1\">&darr;CSV</a>";
            page += "<a class=dim href=/bin?id=" + sid + ">&darr;BIN</a>";
            page += "<span class=sz>" + String(sz / 32) + " records &middot; " +
                    String(sz / 1024) + " kB</span>";
            page += "<button class=rm data-id=" + sid + ">delete</button></li>";
            n++;
        }
        dir.close();
        give();
        page += F("</ul>");
        if (!n) page += F("<p class=dim>no tests yet</p>");
    }
    page += F("</main><script>"
              "document.querySelectorAll('.rm').forEach(b=>b.onclick=async()=>{"
              " const id=b.dataset.id;"
              " if(!confirm('Delete TEST_'+String(id).padStart(4,'0')+'.BIN? This is permanent.'))return;"
              " b.disabled=true;"
              " const r=await fetch('/api/rm?id='+id,{method:'POST'});"
              " st.textContent=await r.text(); st.className=r.ok?'curr':'bad';"
              " if(r.ok)b.closest('li').remove(); else b.disabled=false;});"
              "</script>");
    http->send(200, "text/html; charset=utf-8", page);
}

void WebUi::handleRemove() {
    const int id = http->arg("id").toInt();
    if (id <= 0) { http->send(400, "text/plain", "id required"); return; }
    if (sd_->deleteTest(static_cast<uint16_t>(id))) {
        char msg[64];
        snprintf(msg, sizeof(msg), "TEST_%04u.BIN deleted", id);
        http->send(200, "text/plain", msg);
    } else {
        http->send(409, "text/plain",
                   "failed: file belongs to the running test, or no card");
    }
}

// Сирий бінарник як він лежить на картці (§44). Потрібен, коли CSV замало —
// наприклад щоб перечитати лог іншим інструментом без втрати точності.
void WebUi::handleBin() {
    const int id = http->arg("id").toInt();
    char path[40], disp[64];
    snprintf(path, sizeof(path), "/BATTERY_TESTS/TEST_%04u.BIN", id);
    snprintf(disp, sizeof(disp), "attachment; filename=TEST_%04u.BIN", id);

    if (!sd_->cardPresent()) { http->send(503, "text/plain", "no card"); return; }
    if (!take(3000)) { http->send(503, "text/plain", "bus busy"); return; }
    File f = SD.open(path, FILE_READ);
    if (!f) { give(); http->send(404, "text/plain", "no such test"); return; }
    http->sendHeader("Content-Disposition", disp);
    http->streamFile(f, "application/octet-stream");
    f.close();
    give();
}

// §46 — CSV робиться з бінарного лога вже після тесту, а не під час нього.
void WebUi::handleCsv() { streamLog(nullptr, true); }

void WebUi::streamLog(const char *, bool) {
    const int id = http->arg("id").toInt();
    char path[40];
    snprintf(path, sizeof(path), "/BATTERY_TESTS/TEST_%04u.BIN", id);

    if (!sd_->cardPresent()) { http->send(503, "text/plain", "no card"); return; }

    // За замовчуванням CSV відкривається у вкладці — так його зручно
    // прогледіти не завантажуючи. Файлом віддається лише за явним dl=1.
    if (http->arg("dl") == "1") {
        char disp[64];
        snprintf(disp, sizeof(disp), "attachment; filename=TEST_%04u.csv", id);
        http->sendHeader("Content-Disposition", disp);
    }
    http->setContentLength(CONTENT_LENGTH_UNKNOWN);
    http->send(200, "text/csv; charset=utf-8", "");
    http->sendContent(F("timestamp,V,I,P,Ah,Wh,T,state\n"));

    LogRecord chunk[kChunkRecords];
    uint32_t offset = 0;
    String out;
    out.reserve(4096);

    for (;;) {
        size_t got = 0;
        if (!take(3000)) break;
        File f = SD.open(path, FILE_READ);
        if (!f) { give(); break; }
        f.seek(offset);
        got = f.read(reinterpret_cast<uint8_t *>(chunk), sizeof(chunk));
        f.close();
        give();

        const size_t n = got / sizeof(LogRecord);
        if (!n) break;
        offset += n * sizeof(LogRecord);

        out = "";
        for (size_t k = 0; k < n; k++) {
            const LogRecord &r = chunk[k];
            char line[128];
            snprintf(line, sizeof(line), "%.3f,%.4f,%.4f,%.4f,%.5f,%.5f,%.1f,%s\n",
                     r.timestamp_ms / 1000.0f, r.voltage, r.current, r.power,
                     r.Ah, r.Wh, isnan(r.temperature) ? 0.0f : r.temperature,
                     stateName(r.flags, r.current));
            out += line;
        }
        http->sendContent(out);
        if (n < kChunkRecords) break;
    }
    http->sendContent("");
}

}  // namespace ui
