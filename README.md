# Domyos EB520: pannello e ponte FTMS

Il firmware attuale per ESP32-4848S040 mostra i dati della bike, controlla la
resistenza dal touch e inoltra potenza, cadenza, velocità e distanza al T-Rex 3 Pro
come unica periferica `Domyos FTMS`. Funzionamento confermato sul watch
con firmware 6.4.16.3.

Compilazione predefinita: `panel_ftms`. Istruzioni in [PANEL.md](PANEL.md),
protocollo in [FINDINGS.md](FINDINGS.md), ponte e verifiche in [AMAZFIT.md](AMAZFIT.md).

## Programma originale di indagine

ESP32 BLE central probe. Tested compilation: PlatformIO espressif32 6.8.1,
Arduino ESP32 2.0.17, ESP32-D0WD-V3 revision 3.1.

## Firmware

`src/main.cpp` performs a 12-second active BLE scan at startup. Via serial
(115200 baud, newline-terminated commands):

- `scan`: repeat discovery while disconnected; indices may change.
- `connect INDEX`: connect to the selected scan result, enumerate GATT,
  read readable characteristics, request data notification subscriptions.
- `disconnect`: disconnect.
- `mark TEXT`: label a physical experiment in the serial log.

No application characteristic writes, workout commands, pairing requests or
resistance commands are implemented. Enabling notifications does write CCCDs.
This is a BLE central, not a passive sniffer of other apps' connections.
Bluetooth Classic is not investigated by this firmware.

Subscriptions use asynchronous ESP-IDF calls: `REGISTER_ACK` and `CCCD_ACK`
report status (0 means success). The generic Service Changed indication is
excluded after the synchronous library subscription blocked during discovery.
Firmware v2 subscribes only to Notify characteristics (5 on this console),
leaving unused control-point indications out to respect the SDK's limit of 5.
Reception of notifications is the evidence that a data stream works.
A zero-length read alone does not prove
successful access: consult library errors immediately before it.

## Build and upload

Original probe: `pio run -e esp32dev`. On this PC the existing PlatformIO environment
contains Python 3.13 packages while its interpreter resolves to Python 3.14;
the following command successfully reuses the installed pure Python packages:

```sh
pio run -e esp32dev
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 write-flash 0x1000 .pio/build/esp32dev/bootloader.bin 0x8000 .pio/build/esp32dev/partitions.bin 0x10000 .pio/build/esp32dev/firmware.bin
```

Flashing replaces the ESP32 application and partition table. The board was
provided reset for this experiment; no previous firmware backup was made.
To replace this diagnostic firmware later, flash the desired firmware normally.

## Capture

```sh
python3 tools/capture.py --reset --seconds 20 --output captures/scan.log
python3 tools/capture.py --command 'connect 3' --seconds 30 --output captures/gatt.log
python3 tools/capture.py --interactive --seconds 3600 --output captures/live.log
```

Only one process should own the serial port at once. Opening without `--reset`
sets RTS/DTR inactive before opening; some drivers may still briefly toggle
them. A reset is identifiable by boot messages in the capture.
Host times are UTC; ESP32 times are milliseconds since boot.
For an interactive experiment keep the process and port open, inspect the
latest scan, then type `connect INDEX`. The initial index must be checked after
each scan. On the tested USB interface, opening the port causes a board reset
even with RTS/DTR preset; sending a command immediately may use a new scan's
indices. Prefer interactive mode.

## Evidence quality

`captures/01-scan.log` is the initial diagnostic run. Its `ADV_RAW` fields are
INVALID: Arduino BLE stores a borrowed pointer to GAP event data which is no
longer valid after scanning. Parsed addresses, names and service UUIDs were
copied by the library and are separate from this issue. Current firmware copies
raw bytes inside the scan callback before the pointer expires.

Do not infer field meanings or physical control capability from UUIDs alone.
Compare actual notifications against display values during controlled trials.
