# GroveNFC Reference Demo (AtomS3)

Reference firmware demo for **Grove NFC module** on **M5Stack AtomS3 (current demo target)**.

This project demonstrates one implementation path. GroveNFC capability can be adapted to other hardware platforms in the future via **I2C/UART communication paths**.

## Capability Boundary

### GroveNFC hardware/module layer

- Card-size hardware form factor
- NFC communication interface (platform integration via I2C/UART)
- Multi-protocol card/tag interaction:
  - ISO14443A
  - ISO14443B
  - ISO15693
  - FeliCa
- Tag emulation data path used by demo modes:
  - MIFARE 1K
  - NTAG213 / NTAG215 / NTAG216
  - ISO14443B (China II)
  - ISO15693
- Low-level diagnostic operations:
  - device communication check
  - HW/FW version readback
  - mode register R/W check
  - RF on/off control check

### Reference firmware layer (this repo)

- AtomS3 UI and single-button interaction flow
- Reader / NDEF / Emulator / Diagnose pages
- Serial logging, heartbeat, and boot debug flow
- NDEF read and text parsing for demo usage
- Optional app-level behaviors (for example, network onboarding) implemented by firmware, not by GroveNFC hardware itself

## Hardware

Current AtomS3 demo I2C pins (editable in `src/main.cpp`):

- SDA: `GPIO2`
- SCL: `GPIO1`
- I2C address: `0x48`

If your AtomS3 base uses different pins, update `kSdaPin` and `kSclPin`.

## UI and Button Control

Single button: `BtnA`

### Home page

- **Single click**: rotate feature card (`Diagnose -> Reader -> Read NDEF -> Emulator`)
- **Long press (~1s)**: enter current feature

### Reader page

- Auto-polls and displays detected card protocol/ID
- **Single click**: force immediate scan
- **Long press (~1s)**: go back to Home

### Read NDEF page

- Auto-polls NDEF at intervals
- **Single click**: manual NDEF scan now
- **Long press (~1s)**: go back to Home

### Emulator page

- Menu actions: `Back`, `Start`, `Type`, `Slot`
- **Single click**: move to next action/item
- **Long press (~1s)**: confirm current action/item
- Slot range: `0` to `7`

### Wi-Fi popup (from NDEF)

This is an **example firmware behavior** in the current demo, not a GroveNFC hardware feature.

When NDEF text contains a Wi-Fi payload like:

`WIFI:T:WPA;S:YourSSID;P:YourPassword;;`

- **Single click**: cancel
- **Long press (~700ms)**: connect

Connection result is shown on the NDEF page (IP is shown on success).

## Boot Debug Flow

With `kAutoBootDebug = true` (default):

- Runs one diagnose cycle at boot
- Tries one card read + one NDEF read
- Prints `[BOOT]` logs to serial
- Returns to Reader page after the boot debug display window

Set `kAutoBootDebug` to `false` in `src/main.cpp` to disable it.

## Build and Upload (PlatformIO)

1. Install PlatformIO extension in VS Code
2. Open this project folder
3. Build/Upload with environment: `m5stack-atoms3`
4. Open Serial Monitor at `115200`

Example:

```bash
pio run -t upload
pio device monitor -b 115200
```

## Notes

- Reader polling order is: `ISO14443A -> ISO14443B -> ISO15693 -> FeliCa`.
- For ISO14443B, the displayed ID is PUPI-style identifier from anti-collision.
- Emulation images are preloaded in firmware (NTAG/MIFARE/ISO15693 demo data).
- Wi-Fi popup/connect logic belongs to the demo firmware application layer.
