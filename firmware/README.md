# SmartWatchFirmware

Independent Arduino/Watchy firmware for the BBWF watch.

## Features

- Watch face, system menu, e-book reader, reader menu, TOC, font size, font select, WiFi hotspot, BLE config, alarm, settings, storage.
- TXT book scanning, reading progress save/reset, chapter jump.
- WiFi AP: `BBWF` / `12345678` / `192.168.1.5`.
- Web manager can upload/delete TXT books, edit alarms, set time from phone browser, and save WiFi credentials.
- BLE manager can upload/view/delete TXT books, edit alarms, save WiFi credentials, and set watch time from the phone.
- Alarm data persists on SD and triggers beeps plus vibration.
- BMA423 step counter with daily reset.
- MAX30102 heart-rate/SpO2 sampling attempt every 10 minutes.

## SD Card

- `/books`: TXT novels.
- `/progress`: reading progress files.
- `/fonts`: Chinese 16x16 dot-matrix font files.
- `/alarms.csv`: alarm storage.
- `/watch_state.txt`: app state storage.

## Chinese Text

Chinese reading needs a 16x16 HZK/GBK dot-matrix font file on the SD card:

```text
/fonts/HZK16.hzk
```

The file can also use `.bin` or `.fon`, but it must be a GBK/GB2312 compatible 16x16 dot-matrix library. The firmware auto-selects the first font found in `/fonts`.

The Android app converts UTF-8 TXT novels to GBK before BLE upload. For manual SD copy or WiFi web upload, use GBK/ANSI encoded TXT for best Chinese display.

If only English and numbers display correctly, check:

```text
1. SD has /fonts/HZK16.hzk
2. Settings > Change Font selects that font
3. TXT is GBK/ANSI, or upload it through the Android app
```

## BLE Protocol

Device name: `BBWF Watch`

Service UUID: `6f4d0001-6f6f-4f20-8677-626277660001`

- Status characteristic: `6f4d0002-6f6f-4f20-8677-626277660001`, read/notify, returns JSON.
- Command characteristic: `6f4d0003-6f6f-4f20-8677-626277660001`, write.

Commands:

```text
WIFI:ssid|password
ALARM_ADD:07:30
ALARM_TOGGLE:0
ALARM_DELETE:0
SET_TIME:2026-05-10 20:30:00
BOOK_REFRESH
BOOK_DELETE:0
BOOK_UPLOAD_BEGIN:novel.txt|12345
BOOK_UPLOAD_CHUNK:<base64>
BOOK_UPLOAD_END
STATUS
```

## Arduino IDE

Open:

```text
E:\esp32\SmartWatchFirmware_local\SmartWatchFirmware_local.ino
```

Board settings:

```text
Board: ESP32S3 Dev Module
Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
```

Android app:

```text
E:\esp32\BLE_android
E:\esp32\BLE_android\app\build\outputs\apk\debug\app-debug.apk
```
