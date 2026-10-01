# Flashing the reTerminal Sticky

Prebuilt images are attached to this fork's GitHub Releases (`sticky-*` tags),
built from env `sticky-gh_release` (release logging, no wait-for-USB).

| File | Offset | Notes |
|---|---|---|
| `bootloader.bin` | `0x0` | |
| `partitions.bin` | `0x8000` | Same layout as upstream CrossMux |
| `boot_app0.bin` | `0xe000` | Resets OTA selection so this app boots |
| `firmware.bin` | `0x10000` | App |
| `crossmux-sticky-merged.bin` | `0x0` | All of the above in one image |

Each release lists the SHA-256 of its images.

Settings, Wi-Fi credentials and books live on the SD card (`/.crosspoint/`), so
coming from upstream CrossMux they carry over. Coming from Seeed's stock
firmware, this replaces it like any partner firmware. Back up the old firmware
first if you may want it back:

```bash
python -m esptool --chip esp32s3 --port COM5 --baud 921600 read-flash 0 ALL sticky-backup.bin
```

The backup contains the device's internal storage (saved Wi-Fi and Bluetooth
pairings, the lock-screen PIN hash): keep it private.

## Option A: esptool (USB-C cable)

Find the COM port (Device Manager → Ports, the Sticky's WCH/CH343 bridge), then:

```bash
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write-flash 0x0 crossmux-sticky-merged.bin
```

The Sticky stays awake after a cabled boot; if the screen still shows an old
image, press the AI button once.

## Option B: web flasher

Any ESP Web Tools / esptool-js page (e.g. https://espressif.github.io/esptool-js/):
connect, add `crossmux-sticky-merged.bin` at `0x0`, program.

## Option C: build from source (Windows)

```bash
powershell -ExecutionPolicy Bypass -File tools/sticky/build-windows.ps1 -Upload -Port COM5
```

See the comments in `build-windows.ps1` for why it mirrors the tree to a short
path. On Linux/macOS, plain `pio run -e sticky-gh_release -t upload` works.

## After flashing

Apps → Hermes → Setup: set the Hermes URL and API key (and a speech-to-text URL
for voice). See `docs/hermes-app.md`; the S3XY button is in `docs/s3xy-app.md`.
