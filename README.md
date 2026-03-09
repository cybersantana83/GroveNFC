# GroveNFC Reference Demo (AtomS3 + M5Stick Series)

Reference firmware demo for **Grove NFC module** on **AtomS3 / M5StickS3 / M5StickC Plus1.1 / M5StickC Plus2**.

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

- AtomS3 / M5StickS3 UI and single-button interaction flow
- Reader / NDEF / Emulator / Diagnose pages
- Serial logging, heartbeat, and boot debug flow
- NDEF read and text parsing for demo usage
- Optional app-level behaviors (for example, network onboarding) implemented by firmware, not by GroveNFC hardware itself

## Hardware

Current board I2C pins (auto-selected in `src/main.cpp` by build target):

- AtomS3: SDA `GPIO2`, SCL `GPIO1`
- M5StickS3: SDA `GPIO9`, SCL `GPIO10`
- M5StickC Plus1.1 / Plus2: SDA `GPIO32`, SCL `GPIO33`
- I2C address: `0x48`

Pin mapping note:

- AtomS3 Grove `G1/G2` corresponds to StickC Grove `G33/G32` (thus `SCL=33`, `SDA=32`).

Audio note:

- M5StickC Plus1.1 / Plus2 use **PWM buzzer** (no full speaker path).

If your wiring uses different pins, update `kSdaPin` and `kSclPin`.

For M5StickS3 target, firmware enables `EXT_5V` power output (`M5.Power.setExtOutput(true)`) at boot so Grove port can power external modules.

## UI and Button Control

Single main button: `BtnA`

### Home page

- **Single click**: rotate feature card (`Diagnose -> Reader -> Read NDEF -> Emulator`)
- **Long press (~1s)**: enter current feature

### Reader page

- Auto-polls and displays detected card protocol/ID
- Plays protocol-dependent tone after detecting a new card
- **Single click**: force immediate scan
- **Long press (~1s)**: go back to Home

### Read NDEF page

- Auto-polls NDEF at intervals
- Plays success tone after successful read (Wi-Fi payload uses a 3-note tone)
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
3. Build/Upload with one environment:
  - `m5stack-atoms3`
  - `m5stack-sticks3`
  - `m5stack-stickcplus`
  - `m5stack-stickcplus2`
4. Open Serial Monitor at `115200`

Example:

```bash
pio run -e m5stack-atoms3 -t upload
# or
pio run -e m5stack-sticks3 -t upload
# or
pio run -e m5stack-stickcplus -t upload
# or
pio run -e m5stack-stickcplus2 -t upload
pio device monitor -b 115200
```

## Notes

- Reader polling order is: `ISO14443A -> ISO14443B -> ISO15693 -> FeliCa`.
- For ISO14443B, the displayed ID is PUPI-style identifier from anti-collision.
- Emulation images are preloaded in firmware (NTAG/MIFARE/ISO15693 demo data).
- Wi-Fi popup/connect logic belongs to the demo firmware application layer.