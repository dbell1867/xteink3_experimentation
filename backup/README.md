# Xteink X3 factory firmware backup

- `factory.bin` — full 16 MB flash dump, taken 2026-10-05 from device MAC 84:c7:bb:af:e9:30
- Verified with `esptool verify-flash` (on-chip MD5 matched). SHA-256 in `factory.bin.sha256`.
- Read-only on purpose (chmod 444).

## Restore
```bash
sha256sum -c factory.bin.sha256
esptool --chip esp32c3 --port /dev/ttyACM0 write-flash 0 factory.bin
```

## How it was read (and why it was awkward)
The fast stub reader stalls ("The chip stopped responding") at fixed spots inside app0,
e.g. 0x424000 and 0x454000 (16 KB blocks), every time. The ROM reader (`--no-stub`, ~30 s/MB)
reads them fine. Method: 1 MB chunks with stub, ROM fallback for failing chunks
(0x400000 and 0x500000 needed it), concatenated, then verify-flash.

## Partition table (from 0x8000)
| name | type | offset | size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xE000 | 8 KB |
| app0 | app/ota_0 | 0x10000 | 7616 KB |
| app1 | app/ota_1 | 0x780000 | 7616 KB |
| spiffs | data/spiffs | 0xEF0000 | 1024 KB |
| coredump | data/coredump | 0xFF0000 | 64 KB |

App descriptor at 0x10000: project `arduino-lib-builder` → stock firmware is built on the Arduino-ESP32 core.
