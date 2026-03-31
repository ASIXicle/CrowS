# CrowS — Chatter Redesigned OS with Substance

A custom FOSS operating system for the [CircuitMess Chatter 2.0](https://circuitmess.com/pages/chatter-2-0/) hardware. CrowS replaces the stock firmware entirely with a from-scratch app launcher, games, utilities, and (soon) real encrypted LoRa messaging.

## Why "with Substance"?

The stock Chatter 2.0 firmware markets itself as "encrypted messaging." In practice, the encryption is XOR with a hardcoded 7-byte key (`{1, 2, 3, 4, 5, 6, 7}`) — identical on every device ever shipped. That's security theater, not security.

CrowS aims to replace this with actual cryptography:

|                    | Stock Firmware         | CrowS (planned)                             |
| ------------------ | ---------------------- | ------------------------------------------- |
| **Key exchange**   | None (hardcoded key)   | X25519 (per-pair ephemeral keys)            |
| **Encryption**     | XOR, 7-byte static key | XChaCha20-Poly1305 (256-bit, authenticated) |
| **Nonce handling** | N/A                    | 24-byte random (safe without counter sync)  |
| **Auth tag**       | None                   | 16-byte Poly1305 (tamper detection)         |
| **Library**        | None                   | libsodium                                   |
| **Overhead**       | 0 bytes                | 40 bytes (24B nonce + 16B tag)              |
| **Usable payload** | ~255 bytes             | ~215 bytes                                  |

## Features

- **Matrix-style boot splash** — falling green characters with fade-in title (~3.2s)
- **App launcher** — scrolling menu with status bar (version + battery %)
- **App lifecycle framework** — function-pointer structs with start/tick/button/back/stop callbacks
- **ChatterTris** — full Tetris game with ghost piece, wall kicks, scoring, and pause
- **Music Player** — non-blocking piezo playback with scrolling song list and progress bar
- **Magnet Detector** — ESP32 Hall effect sensor with auto-calibration and pitch-scaled buzzer feedback
- **Settings** — live display of battery, free heap, uptime, CPU frequency

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-D0WD rev v1.1, 4MB flash |
| Display | ST7735S 160×128 color TFT (landscape) |
| Input | 74HC165 shift register (4 nav + 12 keypad buttons) |
| Radio | LLCC68 (SX1262-compatible) LoRa |
| Buzzer | Piezo on GPIO 19 (LEDC PWM) |
| Battery | 3×AAA via onboard Battery class |
| Hall Sensor | ESP32 built-in |

## Building

### Requirements

- [PlatformIO](https://platformio.org/) (via VSCodium/VS Code extension or CLI)
- [platform-circuitmess-esp32](https://github.com/ASIXicle/platform-circuitmess-esp32) — custom PlatformIO platform (see its README for install instructions)
- CircuitMess ESP32 board package v1.8.3 installed in Arduino IDE (the platform symlinks to it)
- USB serial connection to Chatter 2.0 (`/dev/ttyUSB0` on Linux)

### Build & Flash

```bash
cd CrowS
pio run              # compile
pio run -t upload    # compile and flash
pio device monitor   # serial monitor (115200 baud)
```

### Project Structure

```
CrowS/
├── platformio.ini              # build config + compiler defines
├── src/
│   └── main.cpp                # CrowS OS (single-file)
└── lib/
    ├── CircuitOS/              # CircuitMess OS primitives (patched LovyanGFX_setup.h)
    ├── LovyanGFX/              # display driver
    └── RadioLib/               # LoRa radio driver
```

**Library note:** CrowS uses `Chatter-Library` (via `#include <Chatter.h>`) which comes from the CircuitMess framework, not from the project's `lib/` directory. The framework also ships `Chatter2-Library` — this is blocked via `lib_ignore` in `platformio.ini` because its display init path skips the panel configuration required by the ST7735S, resulting in a white screen. See the [platform README](https://github.com/ASIXicle/platform-circuitmess-esp32#critical-display-configuration-st7735s-white-screen-fix) for the full technical explanation.

## Controls

**Launcher:** UP/DOWN navigate, ENTER select

**ChatterTris:** 1=left, 2=rotate, 3=right, 4=hard drop, 6=soft drop, ENTER=start, BACK=pause/exit

**Music Player:** UP/DOWN select song, ENTER play, BACK stop/exit

**Magnet Detector:** ENTER recalibrate, BACK exit

## Adding Apps

Every CrowS app is a `CrowSApp` struct with five function pointers:

```c
typedef struct {
  const char* name;
  uint16_t    color;
  void (*onStart)();
  void (*onTick)();
  void (*onButton)(uint8_t id);
  bool (*onBack)();
  void (*onStop)();
} CrowSApp;
```

Write five functions, add one entry to the `apps[]` array, bump `APP_COUNT`.

## Roadmap

- [x] Base OS (boot splash, launcher, app lifecycle, settings)
- [x] ChatterTris
- [x] Music Player + Magnet Detector
- [x] PlatformIO build system ([custom platform](https://github.com/ASIXicle/platform-circuitmess-esp32))
- [ ] Sub-menu categories (Games, Apps, Messaging, System)
- [ ] LoRa radio — basic send/receive between two devices
- [ ] Encrypted messaging (X25519 + XChaCha20-Poly1305 via libsodium)
- [ ] T9-style text input
- [ ] More games (Snake, Pong)
- [ ] Mesh networking

## License

MIT License — see [LICENSE](LICENSE) for details.

CrowS uses libraries from [CircuitMess](https://github.com/CircuitMess) (MIT) and [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (FreeBSD). [RadioLib](https://github.com/jgromes/RadioLib) is MIT licensed.

## Acknowledgments

Built for the CircuitMess Chatter 2.0. Button map, buzzer GPIO, and Hall sensor behavior all verified empirically — the stock library pin definitions are partially incorrect.
