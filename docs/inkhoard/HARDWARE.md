# InkHoard Hardware Baseline (Plan 006)

Baseline Xteink X4 unit. Do **not** burn eFuses.

**Branch:** `inkhoard/006-hardware-baseline`  
**Firmware artifact:** plan 005 CI `firmware.bin` (5,248,320 bytes; SHA256
`BF20F6EB1D3C6E49A0EA79B401BE51E7FF3C017A348072A9D8D34536E5D4B5E9`) from
upstream tip `a4306130` / fork CI run
[29057987363](https://github.com/Bwb234/crosspoint-inkhoard/actions/runs/29057987363).  
**Completed:** 2026-07-09

## Hardware Compatibility Table

| Field | Value |
| --- | --- |
| Unit label | Baseline X4 #1 |
| Device model | Xteink X4 (ESP32-C3) |
| Chip | ESP32-C3 QFN32 revision v0.4 |
| MAC | `14:63:93:f4:06:90` |
| Flash | 16 MB (manufacturer `85`, device `2018`) |
| Crystal | 40 MHz |
| USB mode | USB-Serial/JTAG |
| Host OS | Windows 10/11 |
| USB ID | `USB\VID_303A&PID_1001` → **COM5** when awake |
| Secure boot (`SECURE_BOOT_EN`) | **False** |
| Flash encryption (`SPI_BOOT_CRYPT_CNT`) | **Disable** |
| Download mode (`DIS_DOWNLOAD_MODE`) | **False** (available) |
| USB-lock / Unlocker | **Not required** |
| Booted firmware (serial) | `1.4.1-dev-inkhoard/007-credentials-b333e08` (plan 007 flash) |

## Pre-Flash Gate — PASSED

Secure boot off, flash encryption off, COM5 opens with esptool. Safe to flash
fork CI / self-built firmware.

## Flash And Recovery

| Flash | Result |
| --- | --- |
| #1 baseline | Verified write + hash; operator confirmed healthy UI |
| #2 recovery proof | Verified write + hash again after healthy first boot |

**Download / recovery sequence on this unit:** wake the X4, keep USB-C connected
so COM5 is Started, then:

```bash
python -m esptool --chip esp32c3 --port COM5 --baud 921600 write-flash 0x10000 path\to\firmware.bin
```

esptool `--before default-reset` via USB-Serial/JTAG is enough; no manual BOOT
button sequence was required.

### Smoke Checklist

- [x] Boots to CrossPoint home menu
- [x] Reads SD card (`SD card detected`; recent books)
- [x] Wi‑Fi STA path (WifiSelection + web server at `192.168.1.240` / `crosspoint.local`)
- [x] Opens an EPUB (`EpubReader` — *Harrison Squared*)
- [x] Network file receive (WebSocket upload of EPUB ~389 KB completed)

## OTA / Dual Slots

| Item | Record |
| --- | --- |
| Dual OTA slots | **Yes** — `app0`/`app1` each `0x640000` in `partitions.csv` |
| Live OTA install on this unit | Not exercised in plan 006 (defer to plan 014 rehearsal) |
| Rollback | Partition scheme supports dual-slot OTA; failed-OTA bootloader flip **not observed** yet |

## Baseline Heap (firmware `a4306130` / `4efce05`)

| Metric | Free heap | Min free (session) | Max alloc |
| --- | --- | --- | --- |
| Idle home menu | **150488** (~147 KB) | 150244 | 114676 |
| Wi‑Fi STA + selection | **~108–110 KB** | ~103–107 KB | ~102–106 KB |
| Web server up (STA) | **~84–87 KB** | **61056** (~60 KB) during WS upload | ~74–82 KB |
| EPUB reader open | **~98–100 KB** (steady); spikes to ~154–159 KB between sections | **93328** (~91 KB) during open/render | 77812 |

**STOP threshold (~40 KB during TLS):** not hit. Lowest Min Free observed was
**~55–61 KB** during WebSocket upload with Wi‑Fi + local web server — still
above the gate. Plans 008/009 should budget against ~60 KB network floor and
~90 KB reader floor on this unit.

## Plan 007 Flash Smoke (2026-07-09)

| Item | Result |
| --- | --- |
| Artifact | CI run [29060825109](https://github.com/Bwb234/crosspoint-inkhoard/actions/runs/29060825109) `firmware.bin` **5,263,344** bytes |
| SHA256 | `03888EE3D4B13477342545F51DD66E5BD7D79EF166CD9AEB77A4F95B78E28420` |
| Flash | Verified write to `0x10000` on COM5; hard reset |
| Boot | Home menu; `SD card detected`; version `1.4.1-dev-inkhoard/007-credentials-b333e08` |
| Idle heap | Free **149840**; Min Free **149476**; MaxAlloc **114676** |
| Token leak | Boot serial capture: **no** `ink_dev_` substring |
| Interactive connection-test states | Operator: Settings → System → InkHoard (URL/token/test/clear). Automated host cannot drive the e-ink UI. |

## Status

**DONE** — hardware decision gate cleared for firmware feature work (007+).
Plan 007 flash/boot smoke recorded above. OTA install/rollback left for plan 014.
