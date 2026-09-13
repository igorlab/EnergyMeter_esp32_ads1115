# Технічне завдання

## Універсальний тестер та аналізатор акумуляторів на ESP32 + ADS1115

**Версія:** 1.0
**Призначення:** проєктування апаратної частини, firmware та web-інтерфейсу.

---

# 1. Мета проєкту

Розробити універсальний вимірювально-тестовий пристрій для акумуляторів із пріоритетом на Li-ion/LiPo елементи та батарейні збірки.

Пристрій повинен вимірювати:

* напругу;
* струм;
* миттєву потужність;
* накопичений заряд;
* накопичену енергію;
* час тесту;
* напрямок потоку енергії;
* опціонально температуру.

Основний розрахунковий параметр для тесту акумулятора:

**ємність у Ah**

Додатковий параметр:

**енергія в Wh**

Пристрій повинен підтримувати як **моніторинг**, так і **автоматизований тест розряду**, за умови підключення зовнішнього або інтегрованого електронного навантаження.

---

# 2. Основні характеристики

| Параметр            |                               Ціль |
| ------------------- | ---------------------------------: |
| Напруга             |                          0–30 V DC |
| Основна область     |                       Li-ion 1S–4S |
| Струм               |                           0–5 A DC |
| Основний шунт       |                              10 mΩ |
| ADC                 |                    ADS1115, 16 bit |
| MCU                 |                              ESP32 |
| Вимірювання струму  |                       differential |
| Вимірювання напруги |         single-ended через divider |
| Автоматичний PGA    |                                так |
| Ah                  |                                так |
| Wh                  |                                так |
| Локальний дисплей   |            TFT 160×128 ST7789, SPI |
| Web interface       |                                так |
| Realtime WebSocket  |                                так |
| Data logging        |                            microSD |
| Калібрування        |                              V + I |
| Температура         |                        опціонально |
| Electronic load     | опціонально / окремий силовий блок |

---

# 3. Основний архітектурний принцип

## 3.1. Measurement Core є незалежним

Вимірювальна частина повинна бути логічно та програмно відокремлена від:

* TFT;
* Wi-Fi;
* HTTP;
* WebSocket;
* SD card;
* web UI;
* експорту CSV.

Жодна з цих підсистем не повинна впливати на:

* частоту вимірювань;
* timing;
* інтегрування Ah/Wh;
* вибір PGA;
* калібрування;
* safety logic.

Основний принцип:

```text
                    ┌───────────────────────┐
                    │   MEASUREMENT CORE    │
                    │                       │
                    │ ADS1115               │
                    │ Timing                │
                    │ V / I acquisition     │
                    │ Auto-GAIN             │
                    │ Calibration           │
                    │ P / Ah / Wh            │
                    │ Safety                │
                    └───────────┬───────────┘
                                │
                         measurement data
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                 │
              ▼                 ▼                 ▼
            TFT               WEB               SD
           Task               Task              Task
```

---

# 4. Апаратна архітектура

```text
                    BATTERY
                 0 ... 30 V DC
                      │
                      │
                  LOAD / DUT
                      │
                      │
                ┌─────▼─────┐
                │  SHUNT    │
                │  10 mΩ    │
                │  Kelvin   │
                └─────┬─────┘
                      │
                     GND

        ┌─────────────────────────────┐
        │           ADS1115            │
        │                              │
        │ AIN0/AIN1 → current         │
        │ AIN2      → battery voltage │
        │ AIN3      → reserve          │
        └──────────────┬───────────────┘
                       │ I²C
                       │
                 ┌─────▼─────┐
                 │   ESP32   │
                 └─────┬─────┘
                       │
       ┌───────────────┼─────────────────┐
       │               │                 │
      TFT             Wi-Fi             SD
       │               │                 │
       │          HTTP/WebSocket         │
       │               │                 │
       ▼               ▼                 ▼
    display         browser            logging
```

---

# 5. ADS1115

Використовується **ADS1115** як основний precision ADC.

Вимоги:

* живлення: 3.3 V;
* I²C;
* 16-bit resolution;
* використання differential measurement для шунта;
* використання PGA;
* використання selectable data rate;
* можливість зміни PGA між конверсіями.

ADS1115 має один ADC, тому канали вимірюються **послідовно**.

---

# 6. Розподіл каналів ADS1115

## AIN0/AIN1

Differential current measurement:

```text
AIN0 - AIN1
```

Підключення безпосередньо до Kelvin sense terminals шунта.

## AIN2

Single-ended battery voltage.

Вхід отримує напругу після резистивного дільника.

## AIN3

Резерв.

Можливе майбутнє використання:

* температура;
* друга напруга;
* додатковий analog sensor;
* контроль навантаження.

---

# 7. Вимірювання струму

## 7.1. Шунт

Основний шунт:

**10 mΩ**

Бажані характеристики:

* 1% або краще;
* оптимально 0.5%;
* 4-terminal Kelvin;
* low temperature coefficient;
* потужність не менше 1 W;
* бажано 2 W.

---

# 8. Розрахунок шунта

При 5 A:

$$
V_{shunt}=5A\times0.010Ω=50mV
$$

$$
P_{shunt}=5^2\times0.010=0.25W
$$

При 2 A:

$$
V_{shunt}=20mV
$$

$$
P_{shunt}=0.04W
$$

Один шунт 10 mΩ використовується для обох режимів.

---

# 9. Kelvin connection

Шунт повинен мати окремі:

* FORCE+
* FORCE−
* SENSE+
* SENSE−

Силовий струм проходить через FORCE terminals.

ADS1115 підключається тільки до SENSE terminals.

```text
        FORCE+
           │
       ┌───┴────┐
       │ 10mΩ   │
       └───┬────┘
           │
        FORCE-

        │       │
      SENSE+  SENSE-
        │       │
       AIN0    AIN1
```

Power traces та sense traces фізично розділити.

---

# 10. Current PGA

Для основного діапазону:

**GAIN = 16**

ADC range:

**±256 mV**

При 5 A:

**50 mV**

При 2 A:

**20 mV**

Струм:

$$
I=\frac{V_{shunt}}{R_{shunt}}
$$

Напрямок струму визначається знаком differential measurement.

---

# 11. Low-side sensing

Шунт встановлюється в negative/low-side лінії.

```text
Battery +
    │
    │
    LOAD
    │
    │
  SHUNT 10mΩ
    │
    │
   GND
```

Це спрощує differential measurement при живленні ADS1115 від 3.3 V.

---

# 12. Вимірювання напруги 0–30 V

Battery voltage не підключається безпосередньо до ADS1115.

Використовується резистивний divider.

Початковий варіант:

```text
Battery+
   │
  90k 0.1%
   │
   ├──────── AIN2
   │
  10k 0.1%
   │
  GND
```

Коефіцієнт:

$$
K=0.1
$$

Отже:

```text
30 V → 3.0 V
20 V → 2.0 V
10 V → 1.0 V
 8 V → 0.8 V
4.2 V → 0.42 V
```

Значення можуть бути скориговані на етапі аналогового проєктування.

---

# 13. Voltage input protection

Передбачити:

* series resistor;
* RC filtering;
* за необхідності захист від коротких імпульсних перенапруг;
* обмеження вхідної напруги ADS1115 у межах datasheet.

PGA не використовується як protection.

---

# 14. Auto-Range для напруги

Напруга вимірюється з автоматичним вибором PGA.

Ціль — постійно використовувати **максимальний можливий gain**, не входячи близько до saturation.

Орієнтовна схема:

```text
0 ... ~2.3 V      → GAIN 16
~2.3 ... 4.6 V    → GAIN 8
~4.6 ... 9.2 V    → GAIN 4
~9.2 ... 18.4 V   → GAIN 2
~18.4 ... 30 V    → GAIN 2/3
```

Точні пороги задаються firmware після верифікації аналогової частини.

---

# 15. Hysteresis Auto-Range

Для кожного діапазону використовувати hysteresis.

Приклад:

```text
GAIN 8 → GAIN 4
V > 4.8 V

GAIN 4 → GAIN 8
V < 4.4 V
```

Мета:

не допустити:

```text
8 → 4 → 8 → 4 → 8
```

при коливанні напруги біля порога.

---

# 16. Auto-Range procedure

Після зміни PGA:

1. стара конверсія вважається недійсною;
2. встановлюється новий PGA;
3. виконується нова конверсія;
4. новий результат використовується для розрахунків.

Не використовувати старий ADC result після зміни configuration.

---

# 17. Realtime measurement scheduler

Measurement Core повинен мати власний scheduler.

Не використовувати:

* HTTP callbacks;
* TFT refresh;
* SD writes;
* Wi-Fi events

для синхронізації вимірювань.

Цільова частота:

**16–32 SPS для струму**

та окремо задана частота для напруги.

Значення може бути скориговане після оцінки noise та часу перемикання PGA.

---

# 18. Реальний часовий базис

Час для інтегрування Ah/Wh визначати за високоточним таймером ESP32.

Не використовувати припущення:

```text
dt = 1 second
```

як єдине джерело часу.

Використовувати реальний:

```text
Δt = timestamp[n] - timestamp[n-1]
```

---

# 19. Структура measurement record

У Measurement Core використовувати єдину структуру:

```cpp
struct Measurement {
    uint64_t timestamp_us;
    float voltage;
    float current;
    float power;
    float Ah;
    float Wh;
    float temperature;
    uint32_t sequence;
    uint8_t flags;
};
```

Це єдиний формат внутрішнього measurement data.

---

# 20. Розрахунок потужності

$$
P=V\times I
$$

Наприклад:

```text
V = 4.200 V
I = 2.000 A

P = 8.400 W
```

---

# 21. Розрахунок ємності

Використовувати чисельне інтегрування струму.

Метод:

**trapezoidal integration**

$$
Ah_n =
Ah_{n-1}+
\frac{I_n+I_{n-1}}{2}
\frac{\Delta t}{3600}
$$

де:

* I — amperes;
* dt — seconds;
* Ah — ampere-hours.

---

# 22. Розрахунок енергії

$$
Wh_n =
Wh_{n-1}+
\frac{P_n+P_{n-1}}{2}
\frac{\Delta t}{3600}
$$

Таким чином пристрій одночасно вимірює:

```text
Ah
Wh
```

---

# 23. Filtering

Hardware:

* RC filter для voltage;
* мінімальний RC filter для current, якщо потрібен.

Software:

* optional low-pass / moving average;
* filtering не повинен створювати значну temporal lag.

Для Ah integration використовувати **фільтрований, але не надмірно затриманий сигнал**.

---

# 24. Measurement vs UI isolation

Measurement Core:

* не працює з Wi-Fi;
* не працює з HTTP;
* не працює з SD filesystem;
* не оновлює TFT;
* не формує HTML;
* не виконує тривалі blocking operations.

Measurement Core лише:

1. отримує дані ADS1115;
2. обробляє їх;
3. розраховує V/I/P/Ah/Wh;
4. виконує safety checks;
5. публікує Measurement record.

---

# 25. FreeRTOS architecture

ESP32 використовує окремі tasks.

Рекомендована структура:

```text
Task: Measurement
    ↓
Task: Safety
    ↓
Queue / RingBuffer
    ├── Display Task
    ├── Web Task
    └── SD Logger Task
```

Measurement Core має найвищий пріоритет.

UI/logging tasks не повинні блокувати measurement task.

---

# 26. Data transport між підсистемами

Використовувати:

* FreeRTOS Queue;
* або lock-free / protected ring buffer.

Не передавати measurement через глобальні змінні без захисту.

Для realtime status дозволити окремий `latestMeasurement`, який безпечно копіюється.

---

# 27. Safety subsystem

Safety logic повинна мати пріоритет вище UI.

Контроль:

* overvoltage;
* undervoltage;
* overcurrent;
* sensor fault;
* ADC fault;
* overtemperature;
* timeout.

При критичній аварії:

```text
STOP LOAD
STOP TEST
MARK FAULT
SAVE STATE
```

---

# 28. Charge / Discharge detection

Струм повинен мати знак. Полярність зафіксована так:

```text
+I → DISCHARGE
-I → CHARGE
```

Це не довільний вибір, а наслідок розводки §11. Шунт стоїть у GND-лінії, і
при розряді струм тече LOAD → SHUNT → GND, тобто SENSE+ вище за SENSE− і
differential AIN0−AIN1 додатній. Протилежна угода вимагала б інверсії знака
в firmware без жодної користі й давала б **від'ємну ємність у розрядному
тесті** — при тому, що розрядний тест є головним сценарієм §29, а його
результат за §57 називається Capacity Ah.

Полярність задається одним прапорцем `CoreConfig::invert_current` і більше
ніде. Якщо шунт переставлять у плюсову лінію, змінюється рівно він.

Знак струму — це внутрішнє представлення напрямку, а не спосіб його
показати. У показаннях величини виводяться **без знака**: напрямок уже
сказаний словом, і мінус перед числом читається як несправність. Знак
зберігається там, де він єдиний носій напрямку — у `/api/status`, у CSV,
у бінарному лозі та в діагностичній консолі.

Web та TFT повинні явно показувати:

```text
CHARGE
```

або:

```text
DISCHARGE
```

---

# 29. Режим тестування акумулятора

Основний сценарій:

```text
START TEST
    ↓
initial measurement
    ↓
start load
    ↓
constant current
    ↓
continuous measurement
    ↓
integrate Ah/Wh
    ↓
V <= cutoff
    ↓
stop load
    ↓
save final result
```

---

# 30. Constant-current load

Електронне навантаження повинно бути окремим силовим блоком.

Можлива архітектура:

```text
ESP32
   │
   │ current setpoint
   ▼
Current regulator
   │
   ▼
MOSFET
   │
   ▼
Battery
```

Цільові струми:

```text
0.1 A
0.5 A
1.0 A
2.0 A
3.0 A
5.0 A
```

Точні характеристики залежать від реалізації load.

Measurement subsystem не повинен залежати від того, де реалізовано load.

---

# 31. Cutoff voltage

Параметр:

```text
Vcutoff
```

налаштовується користувачем.

Приклад:

```text
3.00 V
```

Але значення не є універсальним для всіх типів Li-ion.

Firmware не повинна припускати одну cutoff voltage для будь-якої хімії.

---

# 32. Stop conditions

Тест повинен завершуватися при:

```text
V <= Vcutoff
```

або:

```text
I > Imax
```

або:

```text
T > Tmax
```

або:

```text
Test timeout
```

або:

```text
sensor fault
```

---

# 33. TFT

Локальний індикатор — TFT 1.8" 128×160 на **ST7789** (модуль KMR-1.8), шина SPI,
робоча орієнтація — ландшафт 160×128.

Оновлення:

наприклад 2–5 Hz.

TFT:

* ніколи не читає ADS1115;
* не виконує інтегрування;
* не визначає стан тесту;
* отримує готовий Measurement record.

## 33.1. Спільна шина SPI

TFT і microSD (§41) сидять на одній шині SPI. Це не деталь монтажу, а
архітектурне обмеження, якого не було б у I²C-дисплея, і воно стосується
трьох різних місць ТЗ:

* доступ до шини з Display Task і SD Logger Task серіалізується мьютексом;
  жодна з цих задач не має права тримати шину невизначено довго;
* дисплей ініціалізується **до** першого звернення до картки. У зворотному
  порядку драйвер SD лишає шину в стані, після якого ініціалізація дисплея
  не проходить;
* повний кадр 160×128×16 біт — це 40 KB, тобто близько **20 ms** зайнятої
  шини на 16 MHz. Тому Display Task перемальовує лише поля, що змінились,
  а не кадр цілком. Повне перемальовування допустиме хіба при зміні екрана.

Measurement Core у цьому не бере участі взагалі: ADS1115 висить на I²C і шини
SPI не торкається. Затримка на шині може загальмувати індикацію або логування,
але не вимірювання.

---

# 34. Екрани TFT

160×128 у ландшафті, шрифт Adafruit GFX 6×8 px: 26 символів у рядку при
розмірі 1, 13 — при розмірі 2, 8 — при розмірі 3.

Три сторінки. Перемикання — кнопкою або командою консолі; автопрокрутка
можлива, але вимкнена за замовчуванням: відводити екран від живих показань,
поки на нього дивляться, гірше, ніж зайве натискання.

## 34.1. LIVE

```text
┌──────────────────────────┐
│  3.629 V             SD  │
│                      NET │
│  1.000 A   RUNNING   SIM │
│            DISCHARGE     │
│ P   3.629 W   00:07:30   │
│ Ah   0.1 Wh   0.5 28.5C  │
│ 4.25┤                    │
│     │╲                   │
│ 3.91┼──╲─────────────────│
│     │    ╲──             │
│ 3.58┤        ╲────       │
│     └────────────────────│
│ 0                   7min │
└──────────────────────────┘
```

Графік має обидві осі. Ліві 31 px — вісь напруги: три підписи з засічками
(верх шкали, середина, низ) і горизонтальна лінія сітки посередині поля.
Нижні 10 px — вісь часу: нерухомий `0` ліворуч і повна тривалість праворуч.
Поле побудови — 128 колонок, x від 32 до 159.

Графік без підписаних осей не є вимірювальним приладом: за ним не можна ні
зняти значення, ні порівняти два тести.

Крива показує **весь тест від старту до поточного моменту**. Колонка спершу
коштує 1 секунду; коли 128 колонок закінчуються, сусідні пари зливаються в
одну, а крок по часу подвоюється. Крива ущільнюється, але з екрана не йде
ніколи: 128 с, далі 256 с, 512 с — і так до 4.5 години на тих самих 128
колонках. Горизонтального скролу немає свідомо — він показував би хвіст і
ховав початок розряду, тобто найінформативнішу ділянку.

Вісь часу — час під інтегруванням, а не uptime: пауза її не розтягує, а
скидання ємності скидає й криву.

Шкала напруги автоматична, з полем 8%. Перерахунок меж і повне
перемальовування відбуваються лише тоді, коли нове значення вийшло за межі
поточної шкали; у звичайному кадрі малюється одна колонка.

## 34.2. TEST

Підсумок за §57: Ah і Wh розміром 2, далі стартова, поточна та мінімальна
напруга, середній і максимальний струм, максимальна температура, тривалість
і причина зупинки.

## 34.2.1. Розрядність показань

Ємність і енергія виводяться **до десятих**: `2.6 Ah`, `9.6 Wh`. Поля мають
сталу ширину (`%5.1f`), інакше при переході 9.9 → 10.0 непрозорий фон не
стер би зайвий символ.

Наслідок, про який треба знати: при 1 А перші шість хвилин тесту показують
`0.0 Ah`. Це не помилка інтегрування — накопичена ємність справді ще менша
за половину молодшого розряду індикації.

Serial-консоль лишається з повною розрядністю (`%.5f`). Вона не індикація, а
вимірювальний канал: §69 вимагає звіряти інтегрування з еталонним
навантаженням, а такий тест триває хвилини, і на десятих він був би сліпий.

Середній струм — це Ah, поділені на час: перерахунок готової величини в інші
одиниці, а не повторне інтегрування (§52).

## 34.3. DIAG

Метрики §69 і §70 без serial-кабеля: діапазон PGA і його шкала, сирі відліки
обох каналів, тривалість останнього і найдовшого циклу, лічильники помилок
ADC, змін діапазону та втрачених record, номер запису, вільна пам'ять.

`cyc max` підсвічується жовтим при перевищенні порога — саме за цим числом
перевіряється, що індикація, SD і Wi-Fi не зрушили таймінги вимірювання.

## 34.4. Кольори

Кольором позначаються тільки стан і напрямок струму; числа лишаються білими,
щоб зміна кольору не читалась як зміна величини.

Величина має колір, і той самий колір несуть її число, її одиниця, її вісь
і її крива. Це єдине правило, з якого все інше виводиться.

```text
синій        напруга: число, "V", вісь і підписи графіка, сама крива
зелений      струм: число, "A", Ah, RUNNING, CHARGE
білий        похідні величини: P, Wh, температура, час
сірий        підписи, осі, неактивні індикатори
жовтий       DISCHARGE, перевищення порога діагностики
червоний     FAULT, помилки ADC
пурпуровий   SIM — дані синтетичні
```

RUNNING зелений не випадково: він означає, що струм тече. DISCHARGE жовтий,
бо це напрямок, у якому батарея втрачає заряд, і його треба помітити.

Числові поля перемальовуються по місцю непрозорим фоном; `fillScreen()`
викликається лише при зміні сторінки (§33.1).

## 34.5. Режим симуляції

Якщо ADS1115 не відповідає на I²C, прошивка не зупиняється, а піднімає ядро
на синтетичному розряді Li-ion: розкладку, кольори й таймінги дисплея треба
доводити до того, як припаяний АЦП.

Синтетичні дані **зобов'язані** бути позначені: мітка `SIM` пурпуровим на
екрані, попередження в консолі, прапорець у діагностиці. Мовчазна симуляція
гірша за відсутність даних — вона робить непрацюючий канал схожим на
працюючий.

---

# 35. Realtime Web Interface

ESP32 повинен містити локальний HTTP server.

Адреса:

```text
http://<ESP32-IP>/
```

Працювати через:

* домашню Wi-Fi;
* або Access Point ESP32.

Web UI не повинен впливати на Measurement Core.

---

# 36. Realtime transport

Використовувати **WebSocket**.

Наприклад:

```text
/ws
```

Measurement Core → Web Task → WebSocket clients.

HTTP client не може ініціювати ADC measurement.

---

# 37. REST/API status

Передбачити:

```text
/api/status
```

Приклад:

```json
{
  "voltage": 4.182,
  "current": 1.024,
  "power": 4.284,
  "Ah": 1.284,
  "Wh": 5.391,
  "temperature": 27.4,
  "mode": "DISCHARGE",
  "running": true,
  "elapsed": 4524
}
```

`/api/status` тільки читає останній готовий Measurement.

---

# 38. Web pages

## `/`

Realtime dashboard:

* V
* A
* W
* Ah
* Wh
* temperature
* charge/discharge
* elapsed time
* test status
* SD status

## `/graph`

Realtime charts:

* V/time
* I/time
* W/time
* V/Ah

## `/tests`

Список тестів.

## `/test?id=XXXX`

Деталі тесту.

## `/settings`

Налаштування та калібрування.

---

# 39. Web controls

Web interface дозволяє:

```text
START
STOP
RESET
DOWNLOAD
```

При цьому команда лише передається у відповідний control task.

Web task не виконує physical control operation безпосередньо всередині HTTP callback.

---

# 40. Realtime graph

Для поточного графіка використовувати RAM ring buffer.

Наприклад:

```text
5–30 min
```

останніх даних.

RAM graph buffer не є архівом.

---

# 41. SD card

Для довготривалого логування використовується **microSD**.

Файлова система:

**FAT32**

Інтерфейс:

**SPI**

Рекомендована карта:

**8–32 GB**

Цього достатньо з великим запасом.

---

# 42. SD logging architecture

Не записувати SD безпосередньо з Measurement Core.

Правильно:

```text
ADS1115
   ↓
Measurement Core
   ↓
RAM queue
   ↓
SD Logger task
   ↓
buffer
   ↓
SD block write
```

SD write може тривати або тимчасово блокуватися, але Measurement Core при цьому продовжує працювати.

---

# 43. Logging frequency

Рекомендовано:

**1 record/sec**

або:

**1 record/2 sec**

у штатному режимі.

Measurement може працювати, наприклад, на 16–32 SPS.

UI може отримувати дані частіше.

---

# 44. Binary logging

Під час тесту основний формат:

**binary**

Один record:

```cpp
struct LogRecord {
    uint32_t timestamp_ms;
    float voltage;
    float current;
    float power;
    float Ah;
    float Wh;
    float temperature;
    uint8_t flags;
};
```

Орієнтовний розмір:

~28–32 bytes/record.

---

# 45. Оцінка обсягу

Для 32 bytes:

```text
1 record/s

1 hour   ≈ 115 KB
10 hour  ≈ 1.15 MB
24 hour  ≈ 2.76 MB
100 hour ≈ 11.5 MB
```

Тому microSD має величезний запас.

---

# 46. CSV export

Під час вимірювання не потрібно формувати CSV.

Після завершення тесту:

```text
binary log
      ↓
CSV export
```

CSV має бути доступний через Web Interface.

Приклад:

```text
timestamp,V,I,P,Ah,Wh,T,state
0,4.200,1.000,4.200,0.0003,0.0012,24.1,DISCHARGE
1,4.198,1.001,4.200,0.0006,0.0023,24.1,DISCHARGE
2,4.195,0.999,4.191,0.0008,0.0035,24.2,DISCHARGE
```

---

# 47. Структура SD

Рекомендовано:

```text
/BATTERY_TESTS/
    TEST_0001.bin
    TEST_0001.csv

    TEST_0002.bin
    TEST_0002.csv

    TEST_0003.bin
    TEST_0003.csv
```

Окремо:

```text
/BATTERY_TESTS/INDEX.TXT
```

або аналогічний index file.

---

# 48. RAM buffering

SD logger повинен накопичувати кілька records:

```text
Measurement
Measurement
Measurement
...
        ↓
RAM buffer
        ↓
single SD write
```

Розмір buffer:

орієнтовно 16–64 records.

---

# 49. Внутрішня Flash / NVS

ESP32 internal NVS використовувати для:

* calibration constants;
* voltage divider coefficient;
* shunt resistance;
* cutoff voltage;
* maximum current;
* temperature limits;
* Wi-Fi settings;
* test number;
* current test state;
* cumulative Ah/Wh state.

Не використовувати NVS як realtime log.

---

# 50. Save interval

Поточний test state зберігати періодично.

Наприклад:

**раз на 30 секунд**

та обов'язково:

* при STOP;
* при завершенні;
* при fault;
* при переході state.

---

# 51. Recovery

Після restart ESP32 повинен перевірити:

```text
Was test active?
```

Якщо так:

* відновити test metadata;
* не продовжувати тест автоматично без перевірки безпеки;
* повідомити користувача про interrupted test;
* дозволити resume або abort відповідно до state machine.

---

# 52. Дані, які є authoritative

Єдине authoritative measurement state знаходиться в:

**Measurement Core**

TFT:

тільки display.

Web:

тільки display/control.

SD:

тільки storage.

Жодна з цих систем не повинна самостійно обчислювати Ah/Wh.

---

# 53. Temperature

Опційний датчик:

* NTC;
* або DS18B20.

Бажано контролювати:

* battery temperature;
* MOSFET/load temperature.

При перевищенні Tmax:

```text
LOAD OFF
TEST STOP
FAULT
```

---

# 54. Calibration

## Voltage

Двоточкова calibration:

```text
~4.2 V
~20–30 V
```

Розрахунок:

$$
V_{real}=K_VV_{measured}+Offset_V
$$

## Current

Мінімум:

```text
0 A
1–2 A
```

Бажано:

```text
5 A
```

Розрахунок:

$$
I_{real}=K_II_{measured}+Offset_I
$$

---

# 55. Цільова точність

## Voltage

Після калібрування:

ціль:

**≈ ±0.1% від показання**

або краще у типовому діапазоні.

Для 4.2 V це приблизно:

**±4–5 mV**

як цільова характеристика системи.

## Current

Ціль:

**±0.5% або краще**

у робочому діапазоні.

Точність визначається сукупно:

* ADS1115;
* shunt;
* temperature coefficient;
* PCB parasitics;
* calibration;
* noise.

---

# 56. Малий струм

10 mΩ шунт:

```text
1 mA   → 10 µV
10 mA  → 100 µV
100 mA → 1 mV
1 A    → 10 mV
5 A    → 50 mV
```

Основна точна робоча область:

**~0.1–5 A**

Для <10 mA похибка та noise потребують окремої оцінки.

---

# 57. Battery test result

Після завершення тесту зберігати:

```text
Test ID
Start time
End time

Start voltage
End voltage

Average current
Maximum current

Capacity Ah
Energy Wh

Maximum temperature
Duration

Stop reason
```

---

# 58. Stop reason

Можливі:

```text
CUTOFF_VOLTAGE
OVERCURRENT
OVERTEMPERATURE
TIMEOUT
USER_STOP
SENSOR_ERROR
ADC_ERROR
```

---

# 59. Battery profile

Передбачити профіль:

```text
Battery profile
```

з параметрами:

```text
chemistry
nominal voltage
cutoff voltage
max test current
max temperature
```

Приклади:

```text
Li-ion 1S
Li-ion 2S
Li-ion 3S
Li-ion 4S
Custom
```

---

# 60. Test workflow

```text
INSERT BATTERY
       ↓
MEASURE INITIAL V
       ↓
SELECT PGA
       ↓
SELECT TEST PROFILE
       ↓
SET LOAD CURRENT
       ↓
START
       ↓
MEASUREMENT CORE
       │
       ├── V
       ├── I
       ├── P
       ├── Ah
       └── Wh
       ↓
SAFETY CHECK
       ↓
CUTOFF
       ↓
LOAD OFF
       ↓
FINALIZE LOG
       ↓
TEST RESULT
```

---

# 61. Web realtime workflow

```text
Measurement Core
      │
      ├── latestMeasurement
      │
      └── realtime queue
               │
             Web Task
               │
           WebSocket
               │
        Mac / iPhone / iPad
```

Websocket clients можуть бути:

* 0;
* 1;
* декілька.

Їх наявність не повинна змінювати measurement behaviour.

---

# 62. TFT workflow

```text
Measurement Core
      ↓
latestMeasurement
      ↓
TFT task
      ↓
Display
```

Refresh rate незалежний.

---

# 63. SD workflow

```text
Measurement Core
      ↓
Queue
      ↓
SD Logger
      ↓
RAM Buffer
      ↓
SD Block Write
```

---

# 64. Fault tolerance

У випадку:

### Wi-Fi failure

Вимірювання продовжується.

### Browser disconnected

Вимірювання продовжується.

### TFT failure

Вимірювання продовжується.

### SD absent

Вимірювання продовжується, але logging позначається unavailable.

### SD write failure

Вимірювання продовжується; подія записується у diagnostics.

### HTTP overload

Вимірювання продовжується.

---

# 65. SD absence behaviour

На web:

```text
SD: NOT PRESENT
Logging: DISABLED
```

Measurement Core не повинен переходити у fault лише через відсутність SD, якщо logging не є необхідною умовою конкретного тесту.

---

# 66. UI update rates

Рекомендовано:

```text
Measurement:
16–32 SPS

TFT:
2–5 Hz

WebSocket:
2–10 Hz

SD:
0.5–1 Hz
```

Ці частоти незалежні.

---

# 67. Реaltime data vs archive

Систему чітко розділити:

```text
REALTIME
    ↓
Measurement Core
    ↓
RAM / WebSocket / TFT

ARCHIVE
    ↓
SD
```

SD не використовується як realtime data source.

---

# 68. Розширення V2

Архітектура повинна дозволяти:

* integrated constant-current load;
* battery temperature;
* MOSFET temperature;
* automatic cycling;
* charge/discharge cycles;
* internal resistance measurement;
* Wi-Fi configuration portal;
* MQTT;
* Home Assistant;
* OTA firmware;
* advanced graphs;
* automatic battery classification.

---

# 69. Критерії приймання Measurement Core

## Voltage

Перевірити при:

```text
4.200 V
8.400 V
12.600 V
20.000 V
30.000 V
```

Перевірити:

* правильність measurement;
* правильність Auto-GAIN;
* hysteresis;
* відсутність oscillation.

## Current

Перевірити:

```text
0.1 A
0.5 A
1.0 A
2.0 A
5.0 A
```

## Ah

Провести тест з reference load/current.

Перевірити інтегрування незалежно від:

* Wi-Fi;
* TFT;
* SD;
* web activity.

---

# 70. Критична вимога до тестування архітектури

Потрібно провести окремий stress test:

```text
Measurement running
+
SD continuous write
+
WebSocket full load
+
HTTP requests
+
TFT refresh
```

При цьому:

* sample timing не повинен суттєво змінюватися;
* Ah integration не повинна залежати від UI;
* measurement sequence numbers не повинні пропускатися без реєстрації;
* safety subsystem повинен продовжувати працювати.

---

# 71. Критична вимога до firmware

Заборонені блокуючі операції у Measurement Core, пов'язані з:

```text
Wi-Fi
HTTP
WebSocket
SD
filesystem
TFT
```

Measurement Core повинен мати гарантований шлях виконання.

---

# 72. Рекомендована програмна архітектура

```text
/src
    measurement/
        adc_manager
        voltage_measurement
        current_measurement
        autorange
        calibration
        integration
        safety

    storage/
        nvs_manager
        sd_logger
        csv_export

    ui/
        tft
        web_server
        websocket
        api

    system/
        task_manager
        configuration
        state_machine
```

---

# 73. State machine

Основні стани:

```text
IDLE
READY
RUNNING
CHARGING
DISCHARGING
STOPPING
COMPLETE
FAULT
```

Переходи контролюються центральною state machine.

UI не повинен напряму змінювати hardware state.

---

# 74. Перший апаратний прототип

Рекомендований склад:

```text
ESP32
ADS1115
10 mΩ Kelvin shunt
90k + 10k voltage divider
TFT 1.8" 128x160 ST7789 (KMR-1.8), SPI
microSD module (спільна шина SPI з TFT)
temperature sensor optional
```

Electronic load спочатку можна реалізувати окремим пристроєм для перевірки measurement subsystem.

---

# 75. Перший етап розробки

Спочатку реалізувати тільки:

```text
ESP32
+
ADS1115
+
shunt
+
voltage divider
```

та довести:

* точність V;
* точність I;
* Auto-GAIN;
* calibration;
* Ah;
* Wh.

Після цього додавати:

```text
TFT
SD
Wi-Fi
WebSocket
```

і перевіряти, що Measurement Core не змінив поведінку.

---

# 76. Фінальна ціль

Пристрій повинен забезпечувати:

```text
Voltage       0–30 V
Current       0–5 A
Power         V × A
Capacity      Ah
Energy        Wh
Temperature   optional
Direction     charge/discharge
```

з:

```text
automatic voltage range
16-bit ADC
differential current measurement
Kelvin shunt
real-time integration
microSD logging
HTTP/WebSocket monitoring
TFT display
calibration
test profiles
safety shutdown
```

при принциповій умові:

> **Індикація, Web, Wi-Fi, SD та інші периферійні функції не повинні впливати на вимірювальне ядро ні по timing, ні по точності, ні по коректності інтегрування Ah/Wh.**

---

# 77. Фінальна блок-схема

```text
                         ┌─────────────────┐
                         │     BATTERY     │
                         │     0–30 V      │
                         └────────┬────────┘
                                  │
                                LOAD
                                  │
                           ┌──────▼──────┐
                           │  10 mΩ SHUNT│
                           │    KELVIN   │
                           └──────┬──────┘
                                  │
                                 GND
                                  │
                    ┌─────────────┴─────────────┐
                    │          ADS1115          │
                    │                           │
                    │ AIN0/AIN1 → I differential
                    │ AIN2 → V divider          │
                    │ AIN3 → reserve             │
                    └─────────────┬─────────────┘
                                  │
                                 I²C
                                  │
                    ┌─────────────▼─────────────┐
                    │      MEASUREMENT CORE     │
                    │                           │
                    │ timing                    │
                    │ V / I                     │
                    │ Auto-GAIN                 │
                    │ calibration               │
                    │ P                         │
                    │ Ah / Wh                   │
                    │ safety                    │
                    └─────────────┬─────────────┘
                                  │
                            buffered data
                                  │
             ┌────────────────────┼────────────────────┐
             │                    │                    │
             ▼                    ▼                    ▼
          TFT TASK             WEB TASK            SD TASK
             │                    │                    │
             │                Wi-Fi/HTTP              │
             ▼                    ▼                    ▼
          DISPLAY             WebSocket             LOG
                                  │
                                  ▼
                       Mac / iPhone / iPad
```

---

# 78. Принципова вимога до подальшого hardware design

Перед виготовленням PCB необхідно окремо розрахувати і перевірити:

1. voltage divider;
2. ADC input RC filter — з урахуванням часу заряду конденсатора вибірки
   ADS1115: послідовний резистор фільтра підвищує вихідний опір джерела, і
   завеликий номінал дає перехресну заваду між каналами при перемиканні
   мультиплексора;
3. Kelvin shunt routing;
4. ADS1115 gain/range thresholds;
5. current measurement noise;
6. shunt thermal drift;
7. ESP32/ADS1115 power supply;
8. grounding;
9. SD SPI noise;
10. спільна шина SPI: TFT і microSD, порядок ініціалізації та арбітраж (§33.1);
11. separation між силовою та вимірювальною частинами.

Лише після цього зафіксувати фінальну schematic та PCB layout.
