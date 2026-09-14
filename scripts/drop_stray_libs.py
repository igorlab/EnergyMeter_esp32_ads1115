"""Прибирає бібліотеки, які PlatformIO тягне з реєстру без нашої згоди.

Adafruit ST7735/ST7789 оголошує в library.properties:

    depends=Adafruit GFX Library, Adafruit seesaw Library, SD

PlatformIO слухняно встановлює обидві транзитивні залежності в libdeps. Але
реєстрова SD 1.3.0 (Arduino/SparkFun, писана під AVR) **заступає вбудовану в
ESP32**, бо libdeps має вищий пріоритет за бібліотеки фреймворку. Збірка
падає в Sd2PinMap.h на «Architecture or board not supported».

`lib_ignore = SD` тут не працює: під це ім'я підпадає і вбудована, і тоді
SD.h не знаходиться взагалі. Тому прибираємо каталог фізично, перед збіркою.

seesaw заодно: вона тут не потрібна, а місце й час на встановлення забирає.
"""
import os
import shutil

Import("env")  # noqa: F821  — надає PlatformIO

STRAY = ("SD", "Adafruit seesaw Library")

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # noqa: F821
envname = env.subst("$PIOENV")               # noqa: F821
envdir = os.path.join(libdeps, envname)

for name in STRAY:
    path = os.path.join(envdir, name)
    if not os.path.isdir(path):
        continue
    # SD прибираємо лише реєстрову: вбудована в фреймворк лежить не тут,
    # але перевірка за характерним файлом захищає від несподіванок.
    if name == "SD" and not os.path.exists(os.path.join(path, "src", "utility", "Sd2PinMap.h")):
        continue
    shutil.rmtree(path, ignore_errors=True)
    print(f"drop_stray_libs: прибрано {name} з libdeps/{envname}")
