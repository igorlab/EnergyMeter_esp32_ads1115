/*
 * Калібрування V та I (ТЗ §54) і його зберігання в NVS (ТЗ §49).
 *
 *   V_real = v_gain * V_measured + v_offset
 *   I_real = i_gain * I_measured + i_offset
 *
 * V_measured — напруга на AIN2, помножена на divider_k.
 * I_measured — напруга на шунті, поділена на shunt_ohms.
 * Тобто gain/offset прибирають залишкову похибку, а не замінюють номінали:
 * номінальні divider_k та shunt_ohms лишаються окремими полями, щоб при заміні
 * шунта чи резисторів дільника не тягнути похибку через коефіцієнт.
 */
#pragma once

#include <stdint.h>

namespace meas {

struct Calibration {
    float divider_k   = 10.0f;    // 90k + 10k → 10.0 (ТЗ §12)
    float shunt_ohms  = 0.010f;   // 10 mΩ (ТЗ §7)
    float v_gain      = 1.0f;
    float v_offset    = 0.0f;     // В
    float i_gain      = 1.0f;
    float i_offset    = 0.0f;     // А
    bool  v_calibrated = false;
    bool  i_calibrated = false;

    // Полярність струму (ТЗ §28). Властивість конкретного монтажу — того,
    // як припаяні sense-дроти шунта, — тому живе тут, разом із номіналом
    // шунта й дільника, і зберігається в NVS. Константа в коді означала б
    // перезбирання прошивки після перепаювання двох дротів.
    // false: додатній diff AIN0-AIN1 = CHARGE.
    bool  invert_current = false;
};

// Двоточковий розв'язок: measured/real — дві пари, повертає false, якщо точки
// надто близькі, щоб коефіцієнт мав сенс.
bool solveTwoPoint(float measured_1, float real_1,
                   float measured_2, float real_2,
                   float min_span, float &gain, float &offset);

// NVS. Namespace "battcal", один запис зі своєю версією.
bool loadCalibration(Calibration &out);
bool saveCalibration(const Calibration &cal);
bool eraseCalibration();

}  // namespace meas
