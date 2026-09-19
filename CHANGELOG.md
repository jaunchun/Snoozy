# Snoozy — Changelog & Revision History

All notable changes and hardware specifications for **Snoozy: a sleepy-cat ESP32 OLED console** are documented in this file.

---

## Versioning Scheme

Revisions follow a **`[Letter][Number]`** convention:
- **Major (Letter)**: Significant architectural milestones, new subsystems, or hardware revisions (e.g., `A` -> `B`).
- **Minor (Number)**: Tweaks, game balance adjustments, and bug fixes (e.g., `A` -> `A1` -> `A2`).

Set the active version in [`snoozy.ino`](file:///c:/Users/kajtg/Documents/vsc/snoozy/rev%20a%20beta/snoozy/snoozy.ino):
```c
#define REV_MAJOR "A"
#define REV_MINOR 0     // 0 = "Rev A", 1 = "Rev A1"
```

> [!NOTE]
> The active revision displays on the boot splash sequence and on **Stats Page 1**.

---

## Hardware & Wiring

Everything runs on **3.3V** with a single **common ground (GND)**.

| Component | Pin / Signal | ESP32 GPIO | Description |
|---|---|---|---|
| **OLED SSD1306** (128x64) | SDA | **GPIO 4** | I2C Data (`0x3C`) |
| | SCK / SCL | **GPIO 5** | I2C Clock |
| | VCC / GND | **3V3 / GND** | 3.3V Power & Ground |
| **Analog Joystick** | VRx (Horizontal) | **GPIO 34** | ADC1 input (steer, scroll, pet) |
| | VRy (Vertical) | **GPIO 35** | ADC1 input (confirm, jump, back) |
| | VCC / GND | **3V3 / GND** | 3.3V Power & Ground |
| **Buzzer** | + (Signal) | **GPIO 26** | Audio / Beeps |
| | GND | **GND** | Common Ground |
| **SD Card SPI** *(Optional)* | CS | **GPIO 15** | Chip Select (cartridge engine) |
| | SCK | **GPIO 18** | VSPI Clock |
| | MOSI | **GPIO 23** | VSPI Master Out |
| | MISO | **GPIO 19** | VSPI Master In |

> [!WARNING]
> **Never feed 5V to the joystick.** GPIO 34 and GPIO 35 are ESP32 ADC inputs with a strict **3.3V maximum limit**. Supplying 5V will damage the ESP32.

> [!TIP]
> The joystick pushbutton (`SW`) is not needed — all console navigation and interactions rely entirely on smooth analog stick gestures:
> - **X axis (Left / Right)**: Move, scroll menu, steer, pet the cat.
> - **Y forward**: Confirm, enter, jump.
> - **Y backward**: Back, cancel, exit.

---

## [Rev A] — Core Release

> Initial stable core release. Standalone single-file sketch (no SD card dependency).

### Cat & Mood Engine
- **Procedural Animations**: Realistic breathing cycle, eyelid blinks, curious eye glances, tail wagging, ear drooping, curl-up sleep pose, floating `z`s, and purr squiggles.
- **Dynamic Idle Mood Chain**:
  - `8s` idle: **Bored** (glances around).
  - `16s` idle: **Sleepy** (eyelids droop).
  - `26s` idle: **Asleep** (curls up, `z`s float away).
  - `60s` idle: **Screen Off** (display enters power-save nap).
  - `+34s` idle: **Light Sleep** (ESP32 enters low-power light-sleep bursts).
- **Instant Wakeup**: Any stick nudge wakes the console with a stretch-and-yawn animation.

### Interface & System Features
- **Boot Splash**: Animated walk-in, stretch, title reveal sequence, and revision tag.
- **Card Carousel Menu**: Horizontally sliding card carousel with custom procedurally drawn icons.
- **Persistent RTC Memory**: Gameplay statistics, pet counters, and high scores persist across sleep cycles using ESP32 RTC slow memory.
- **Settings Screen**: In-game adjustments for sound, screen brightness, sleep timeouts, and stats reset.

### Built-in Games
Six full mini-games with per-game high score tracking:

| # | Game | Controls & Gameplay |
|:---:|:---|:---|
| 1 | **Cat Jump** | Endless runner — push **Y forward** to leap over incoming obstacles. |
| 2 | **Mouse Catch** | Basket catching — move **X** to catch falling mice while dodging bombs. |
| 3 | **Yarn Maze** | Procedural labyrinth — navigate 5 distinct maze layouts to reach the yarn ball. |
| 4 | **Paw Pong** | Arcade classic — keep the ball in play while your paddle progressively shrinks. |
| 5 | **Copy Cat** | Memory sequence — watch the paws and replicate the gesture pattern. |
| 6 | **Whisker Whack** | Reflex test — tilt toward holes to bop mice as they pop up. |

- **Bug Fixes**:
  - Resolved mixed-type truncation in `map()` math.
  - Fixed diagonal stick movement swallowing inputs in Copy Cat.
