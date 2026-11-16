# ESP32 TV-OS (CRT Arcade)

A retro operating system / gaming console for CRT televisions, built on an ESP32.  
It outputs composite NTSC video and is controlled from a phone web remote (or optional physical buttons).

**Version:** 2.2  
**Author:** @ARPITKUMARR02  
**Developed with:** Claude, ChatGPT & Grok AI

---

## Features

- Full-screen composite NTSC video output (RGB332) on GPIO25
- Phone-based remote control (no app needed)
- Desktop launcher with icons
- 6 playable games + file manager, clock, settings, and about screen
- Multiple colour themes (Amber, Green, Blue, Mono)
- CRT text fixes (bold / soft white) to reduce rainbow fringing
- Screen position adjust (for overscan)
- Sound effects + background music (passive buzzer)
- High scores saved in flash
- Optional physical buttons
- Wi-Fi access point with captive-portal friendly responses

## Games & Apps

| App       | Description                              |
|-----------|------------------------------------------|
| **Snake**     | Classic snake – eat food, grow, avoid walls/tail |
| **Pong**      | Paddle vs CPU (Easy / Normal / Hard)     |
| **Pac-Man**   | Maze, pellets, power pellets, 4 ghosts   |
| **Doomlet**   | Small raycasting 3D shooter (6 demons)   |
| **Runner**    | Side-scrolling jump & run                |
| **Bricks**    | Breakout-style brick breaker             |
| **Files**     | Virtual file browser                     |
| **Clock**     | Analog uptime clock                      |
| **Settings**  | Themes, sound, speed, screen adjust, etc.|
| **About**     | System information                       |

## Hardware

- **ESP32** (tested on NodeMCU-32S / ESP32-D0WD-V3)
- CRT television with composite (AV) input
- Passive buzzer on **GPIO18** (+ to pin, − to GND) — optional but recommended
- Optional push buttons (set `ENABLE_PHYSICAL_BUTTONS` to 1 and wire to GND):
  - GPIO32 = Up
  - GPIO33 = Down
  - GPIO26 = Left
  - GPIO27 = Right
  - GPIO14 = Select
  - GPIO13 = Back

**Video output:** GPIO25 (composite NTSC)

## Software

- Arduino IDE 1.8.19 (or compatible)
- Board: NodeMCU-32S (or any ESP32)
- Libraries:
  - **ESP_8_BIT_GFX** (and its dependency Adafruit GFX Library)
  - WiFi / WebServer / Preferences (included in ESP32 Arduino core)

## How to Use

1. Flash the sketch to the ESP32.
2. Connect GPIO25 to the composite video input of your CRT TV.
3. (Optional) Connect a passive buzzer to GPIO18.
4. Power on the ESP32.
5. On your phone, join the Wi-Fi network:

   - **SSID:** `ESP32-TV-OS`  
   - **Password:** `CRTremote8`

6. Open a browser and go to:  
   **http://192.168.4.1**

7. Use the on-screen D-pad + Select / Back buttons to navigate and play.

### Controls (Remote)

- **D-pad** — Move / navigate  
- **Select** — Open, confirm, pause, shoot, jump, etc.  
- **Back** — Go back / exit  
- Hold a direction for auto-repeat (menus)

## Settings

Accessible from the desktop:

- Theme, Text fix (for CRT), Screen adjust (X/Y shift)
- Sound (Off / SFX only / SFX + Music)
- Menu music, Music pitch, Volume
- Snake speed, Pong difficulty
- FPS counter
- Reset high scores

All settings are saved automatically.

## Project Status

Working console with games, sound, themes, and phone remote.  
Further improvements (more games, physical controller, real filesystem, etc.) are planned.

## Credits

Developed by **@ARPITKUMARR02** with assistance from:

- **Claude** — initial code and structure  
- **ChatGPT** — debugging and improvements  
- **Grok** — additional ideas and fixes  

Built through lots of testing on real CRT hardware.

## License

Open source. Feel free to use, modify, and share.
