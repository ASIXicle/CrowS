# CrowS — Chatter Redesigned OS with Substance

 <img alt="CrowS" src="docs/featherS.png" />


A custom FOSS operating system for the [CircuitMess Chatter 2.0](https://circuitmess.com/pages/chatter-2-0/) hardware. CrowS replaces the stock firmware entirely with a from-scratch OS: app launcher, games, utilities, and **real end-to-end encrypted LoRa messaging**.

## Why "with Substance"?

The stock Chatter 2.0 firmware markets itself as "encrypted messaging." In practice, the encryption is XOR with a hardcoded 7-byte key (`{1, 2, 3, 4, 5, 6, 7}`) — identical on every device ever shipped. That's security theater, not security.

CrowS replaces this with actual cryptography:

|                    | Stock Firmware         | CrowS                                       |
| ------------------ | ---------------------- | ------------------------------------------- |
| **Key exchange**   | None (hardcoded key)   | X25519 ECDH (per-pair shared secrets)       |
| **Encryption**     | XOR, 7-byte static key | XChaCha20-Poly1305 (256-bit, authenticated) |
| **Nonce handling** | N/A                    | 24-byte random per message                  |
| **Auth tag**       | None                   | 16-byte Poly1305 MAC (tamper detection)     |
| **Trust model**    | None                   | TOFU (Trust On First Use) with key pinning  |
| **Library**        | None                   | Monocypher 4.0.2                            |

## Features

### Encrypted Messaging
- **End-to-end encryption** via X25519 key exchange + XChaCha20-Poly1305 AEAD (Monocypher 4.0.2)
- **Automatic key exchange** — KEX beacons broadcast public keys over LoRa
- **Peer table** — up to 8 peers with TOFU key pinning, persisted in NVS
- **Encrypted by default** — compose auto-encrypts for known peers, falls back to plaintext
- **Packet format:** `ECROWS:sender:nonce_b64:mac+ciphertext_b64`

### Rescue Mode (SOS)
- **Panic Mode** — hold `*` for 5 seconds from any screen to broadcast SOS
- **Receiving devices lock into rescue mode** — Shine Finder proximity tracker with audio tone, locked UI, auto-exit after 30s without signal
- **Emergency Clear** — hold `X` (BACK) for 7 seconds to wipe all data, keys, and identity

### OS
- **Feather boot splash** — hand-drawn pixel art crow feather, purple liquid fill animation
- **Two-level category menu** — Games → Apps → Messaging → System
- **Status bar** — username (theme color) + battery %
- **Identity system** — NVS-backed callsign with first-run setup wizard
- **T9 keyboard input** — multi-tap cycling with 800ms timeout
- **Runtime theme** — purple normally, red during Panic Mode
- **Message persistence** — inbox stored in NVS, survives power cycles (20-slot buffer)
- **WiFi/BT disabled at boot** — frees ~30KB RAM, reduces attack surface

### LoRa Radio
- LLCC68 (SX1262-compatible), 868 MHz, SF9, 125 kHz BW
- Interrupt-driven receive, carrier sense before TX
- Protocol prefixes: `CROWS:` (plaintext), `ECROWS:` (encrypted), `KEX:` (key exchange), `PANIC:` (SOS), `SHINE:` (proximity beacon)

### Apps

| Category  | App            | Description                                            |
| --------- | -------------- | ------------------------------------------------------ |
| Games     | ChatterTris    | Full Tetris — ghost piece, wall kicks, scoring, pause  |
| Apps      | Ghost Detector | Hall sensor, 32-sample calibration, pitch-scaled buzzer|
| Apps      | Music          | Non-blocking piezo, 4 songs, scrolling list, progress  |
| Messaging | Messages       | LoRa send/receive, T9 compose, E2E encryption, inbox   |
| System    | Settings       | Battery, heap, uptime, CPU freq, Reset Name            |
| (hidden)  | Shine Finder   | LoRa proximity detector — activated only via rescue    |

## Hardware

| Component   | Detail                                          |
| ----------- | ----------------------------------------------- |
| MCU         | ESP32-D0WD rev v1.1 (WT32-S1), 4MB flash       |
| Display     | ST7735S 160×128 color TFT (landscape)           |
| Input       | 74HC165 shift register (4 nav + 12 keypad)      |
| Radio       | LLCC68 on HSPI, 868 MHz                         |
| Buzzer      | Piezo on GPIO 19 (LEDC PWM)                     |
| Battery     | 3×AAA                                           |
| Hall Sensor | ESP32 built-in                                  |

## Building

### Requirements

- [PlatformIO](https://platformio.org/) (via VSCodium/VS Code extension or CLI)
- [platform-circuitmess-esp32](https://github.com/ASIXicle/platform-circuitmess-esp32) — custom PlatformIO platform (see its README for install instructions)
- CircuitMess ESP32 board package v1.8.3 installed in Arduino IDE (the platform symlinks to it)
- USB serial connection to Chatter 2.0 (`/dev/ttyUSB0` on Linux)

### Build & Flash

```bash
cd CrowS
pio run -t clean && pio run -t upload && pio device monitor
```

### Project Structure

```
CrowS/
├── platformio.ini              # build config + compiler defines
├── src/
│   └── main.cpp                # CrowS OS (single-file, ~3580 lines)
└── lib/
    ├── CircuitOS/              # CircuitMess OS primitives (patched LovyanGFX_setup.h)
    ├── LovyanGFX/              # display driver
    ├── RadioLib/               # LoRa radio driver (v5.1.0)
    └── Monocypher/             # crypto library (v4.0.2)
```

**Library note:** CrowS uses `Chatter-Library` (via `#include <Chatter.h>`) from the CircuitMess framework, not from `lib/`. The framework also ships `Chatter2-Library` — this is blocked via `lib_ignore` in `platformio.ini` because its display init path skips the panel configuration required by the ST7735S, resulting in a white screen. See the [platform README](https://github.com/ASIXicle/platform-circuitmess-esp32#critical-display-configuration-st7735s-white-screen-fix) for details.

## Controls

| Context         | Controls                                                   |
| --------------- | ---------------------------------------------------------- |
| Menu            | UP/DOWN navigate, ENTER select, BACK return                |
| Compose         | T9 keypad to type, ENTER send, BACK backspace              |
| Inbox           | UP/DOWN scroll messages, BACK exit                         |
| ChatterTris     | 1=left, 2=rotate, 3=right, 4=hard drop, 6=soft drop       |
| Music Player    | UP/DOWN select, ENTER play, BACK stop/exit                 |
| Ghost Detector  | ENTER recalibrate, BACK exit                               |
| Panic Mode      | Hold `*` 5s to activate, hold `9` 5s to deactivate        |
| Emergency Clear | Hold BACK 7s on main menu — **wipes everything**          |

## Build Stats (v0.6.0)

```
RAM:   [=         ]   8.3%  (27,316 / 327,680 bytes)
Flash: [===       ]  31.7%  (664,675 / 2,097,152 bytes)
```

## AI Disclosure

Built with the assistance of Claude Opus 4.6 (Anthropic)
## License

MIT License — see [LICENSE](LICENSE) for details.

CrowS uses libraries from [CircuitMess](https://github.com/CircuitMess) (MIT), [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (FreeBSD), [RadioLib](https://github.com/jgromes/RadioLib) (MIT), and [Monocypher](https://monocypher.org/) (BSD-2-Clause + CC-0).

## Acknowledgments

Built for the CircuitMess Chatter 2.0. Button map, buzzer GPIO, and Hall sensor behavior all verified empirically — the stock library pin definitions are partially incorrect.
