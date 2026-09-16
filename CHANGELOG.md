# Snoozy — revision log

Scheme: **letter = big change**, **number = small change**.
`A` → `A1` → `A2` → `B` → `B1` …

Bump it in `snoozy.ino`:
```c
#define REV_MAJOR "A"
#define REV_MINOR 0     // 0 prints "Rev A", 1 prints "Rev A1"
```
The revision shows on the boot splash and on Stats page 1.

---

## Rev A — core system
First clean core release. Single file, no SD card.

- Procedurally drawn cat: breathing, blinking, glancing, tail wag,
  ear droop, curl-up sleep pose, floating `z`s, purr squiggles
- Idle mood chain: awake → bored → sleepy → asleep → screen off →
  ESP32 light-sleep bursts; any stick nudge wakes it with a stretch
- Boot splash with walk-in, stretch, title reveal and revision tag
- Sliding card carousel menu with drawn icons
- Six built-in games with per-game best scores
- Stats over two pages, Settings (sound, brightness, sleep delay, reset)
- Stats stored in RTC memory so they survive sleep

### Games
| # | Game | Play |
|---|------|------|
| 1 | Cat Jump | endless runner, Y to hop |
| 2 | Mouse Catch | basket the mice, dodge bombs |
| 3 | Yarn Maze | five mazes, reach the yarn |
| 4 | Paw Pong | keep the ball up, paddle shrinks |
| 5 | Copy Cat | repeat the paw sequence |
| 6 | Whisker Whack | bop mice as they pop up |

### Notes
- Every custom type is declared at the top with explicit prototypes.
  The IDE hoists auto-generated prototypes above your definitions,
  which is what causes `'X' does not name a type`. Keep new enums and
  structs in that header block.
- Verified by compiling against stubbed Arduino APIs, clean under
  `-Wall -Wextra`. Two input bugs were caught and fixed this way:
  a mixed-type `map()` call, and Copy Cat swallowing an input when
  the stick moved diagonally.
