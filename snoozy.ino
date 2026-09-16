/* ============================================================
   SNOOZY  -  a sleepy-cat ESP32 OLED console
   ============================================================
   REVISION A
     A   first clean core release: cat, sleep system, menu,
         six built-in games, stats, settings.  No SD yet.

   Revision scheme
     letter  = big change   (new subsystem, format change)
     number  = small change (tweak, fix, balance)
     so A -> A1 -> A2 -> B -> B1 ...
   ------------------------------------------------------------
   Hardware  (everything on 3V3, one common ground)
     OLED SSD1306 128x64   SDA -> GPIO 4    SCK/SCL -> GPIO 5
     Joystick (analog)     VRx -> GPIO 34   VRy -> GPIO 35
     Buzzer                +   -> GPIO 26
   ------------------------------------------------------------
   Controls everywhere
     X axis      move / scroll / steer
     Y forward   confirm / enter / jump
     Y backward  back / exit
   ============================================================ */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_sleep.h>

#define REV_MAJOR "A"
#define REV_MINOR 0          // 0 means plain "Rev A", 1 means "Rev A1"

// ---------- pins ----------
#define OLED_SDA   4
#define OLED_SCL   5
#define OLED_ADDR  0x3C
#define JOY_X_PIN  34
#define JOY_Y_PIN  35
#define BUZZER_PIN 26

#define W 128
#define H 64

Adafruit_SSD1306 oled(W, H, &Wire, -1);

// ---------- joystick orientation ----------
const bool JOY_X_INVERT = true;
const bool JOY_Y_INVERT = false;

// ============================================================
// ALL CUSTOM TYPES FIRST.
// The Arduino IDE hoists auto-generated prototypes above
// everything, so a type used in any signature must exist here
// or the sketch fails with "does not name a type".
// ============================================================
enum Screen {
  SCR_BOOT, SCR_HOME, SCR_MENU, SCR_GAMELIST,
  SCR_PLAY_JUMP, SCR_PLAY_CATCH, SCR_PLAY_MAZE,
  SCR_PLAY_PONG, SCR_PLAY_COPY, SCR_PLAY_WHACK,
  SCR_STATS, SCR_SETTINGS, SCR_SLEEP, SCR_DEEPSLEEP
};

enum CatMood { MOOD_AWAKE, MOOD_HAPPY, MOOD_BORED, MOOD_SLEEPY, MOOD_ASLEEP };

struct Star  { int8_t x, y; uint8_t phase; };
struct Zzz   { float x, y; uint8_t life; bool on; };
struct Spark { float x, y, vx, vy; uint8_t life; bool on; };

#define GAME_COUNT 6

// ---------- explicit forward declarations ----------
void beep(int ms, int reps = 1, int gap = 40);
float axis(int pin, bool invert);
float ax(); float ay();
void markInput(); unsigned long idleMs();
bool gestureFwd(); bool gestureBack(); int gestureX();
void sparkAt(float x, float y, uint8_t n); void sparkStep(); void sparkClear();
void catResetAnim(); void zzzSpawn(float x, float y); void zzzStep();
void catTail(int bx, int by, CatMood m);
void catEars(int hx, int hy, CatMood m);
void catFace(int hx, int hy, CatMood m);
void drawCat(int cx, int cy, CatMood m);
void catTick(); void petTheCat();
void starsInit(); void starsDraw();
void titleBar(const char* t); void hintBar(const char* t);
void pawCursor(int x, int y); void revString(char* out);
void menuIcon(uint8_t which, int cx, int cy, bool big);
void screenBoot(); void screenHome(); void screenMenu(); void screenGameList();
bool gameOverCard(const char* title, uint32_t sc, uint8_t slot);
void cjReset();  void screenCatJump();
void mcReset();  void screenMouseCatch();
void mzBuild();  void mzReset(); void screenYarnMaze();
void ppReset();  void screenPawPong();
void ccReset();  void screenCopyCat();
void wwReset();  void screenWhiskerWhack();
void resetAllGames();
void screenStats(); void screenSettings();
void screenSleep(); void screenDeepSleep();

// ---------- global state ----------
Screen screen = SCR_BOOT;
Screen lastScreen = SCR_BOOT;
CatMood mood = MOOD_AWAKE;

unsigned long lastInputMs = 0;
unsigned long frame = 0;
bool soundOn = true;
uint8_t brightness = 200;

// idle thresholds (ms)
unsigned long tBored  = 8000;
unsigned long tSleepy = 16000;
unsigned long tAsleep = 26000;
unsigned long tDeep   = 60000;

// stats live in RTC memory so they survive sleep
RTC_DATA_ATTR uint32_t statPlays = 0;
RTC_DATA_ATTR uint32_t statNaps  = 0;
RTC_DATA_ATTR uint32_t statPets  = 0;
RTC_DATA_ATTR uint32_t statBest[GAME_COUNT] = {0, 0, 0, 0, 0, 0};

// ---------- tiny helpers ----------
void beep(int ms, int reps, int gap) {
  if (!soundOn) return;
  for (int i = 0; i < reps; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(ms);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < reps - 1) delay(gap);
  }
}

void revString(char* out) {
  if (REV_MINOR == 0) sprintf(out, "Rev %s", REV_MAJOR);
  else                sprintf(out, "Rev %s%d", REV_MAJOR, REV_MINOR);
}

float axis(int pin, bool invert) {
  float n = (analogRead(pin) - 2048) / 2048.0f;
  if (invert) n = -n;
  if (fabs(n) < 0.12f) n = 0;            // deadzone
  return n;
}
float ax() { return axis(JOY_X_PIN, JOY_X_INVERT); }
float ay() { return axis(JOY_Y_PIN, JOY_Y_INVERT); }

void markInput() { lastInputMs = millis(); }
unsigned long idleMs() { return millis() - lastInputMs; }

bool gestureFwd() {
  static bool prev = false;
  bool now = ay() > 0.6f;
  bool fired = now && !prev;
  prev = now;
  if (fired) markInput();
  return fired;
}
bool gestureBack() {
  static bool prev = false;
  bool now = ay() < -0.6f;
  bool fired = now && !prev;
  prev = now;
  if (fired) markInput();
  return fired;
}
int gestureX() {
  static unsigned long last = 0;
  float v = ax();
  if (fabs(v) < 0.6f) return 0;
  if (millis() - last < 190) return 0;
  last = millis();
  markInput();
  return v > 0 ? 1 : -1;
}

// ---------- sparkle pool ----------
const uint8_t MAXSPARK = 14;
Spark sparks[MAXSPARK];

void sparkAt(float x, float y, uint8_t n) {
  for (uint8_t k = 0; k < n; k++)
    for (uint8_t i = 0; i < MAXSPARK; i++)
      if (!sparks[i].on) {
        float a = random(0, 628) / 100.0f;
        float s = random(6, 20) / 10.0f;
        sparks[i] = {x, y, cosf(a) * s, sinf(a) * s, (uint8_t)random(8, 18), true};
        break;
      }
}
void sparkStep() {
  for (uint8_t i = 0; i < MAXSPARK; i++) {
    if (!sparks[i].on) continue;
    sparks[i].x += sparks[i].vx;
    sparks[i].y += sparks[i].vy;
    sparks[i].vy += 0.08f;
    if (--sparks[i].life == 0) { sparks[i].on = false; continue; }
    oled.drawPixel((int)sparks[i].x, (int)sparks[i].y, SSD1306_WHITE);
  }
}
void sparkClear() { for (uint8_t i = 0; i < MAXSPARK; i++) sparks[i].on = false; }
// ============================================================
// THE CAT  -  drawn procedurally so every pose can animate
// ============================================================
Zzz zzzs[4];
unsigned long nextBlink = 0;
bool blinking = false;
unsigned long blinkEnd = 0;
int8_t lookDir = 0;              // -1 left, 0 centre, +1 right
unsigned long nextLook = 0;
bool purring = false;
unsigned long purrUntil = 0;

void catResetAnim() {
  for (uint8_t i = 0; i < 4; i++) zzzs[i].on = false;
  blinking = false;
  nextBlink = millis() + random(1200, 3200);
  nextLook  = millis() + random(1800, 4200);
  lookDir = 0;
}

void zzzSpawn(float x, float y) {
  for (uint8_t i = 0; i < 4; i++)
    if (!zzzs[i].on) { zzzs[i] = {x, y, 40, true}; return; }
}

void zzzStep() {
  for (uint8_t i = 0; i < 4; i++) {
    if (!zzzs[i].on) continue;
    zzzs[i].y -= 0.28f;
    zzzs[i].x += sinf(zzzs[i].life * 0.18f) * 0.35f;
    if (--zzzs[i].life == 0) { zzzs[i].on = false; continue; }
    uint8_t sz = zzzs[i].life > 26 ? 1 : 2;
    oled.setTextSize(sz);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor((int)zzzs[i].x, (int)zzzs[i].y);
    oled.print('z');
  }
  oled.setTextSize(1);
}

// tail wag / curl, drawn as a short arc of pixels
void catTail(int bx, int by, CatMood m) {
  if (m == MOOD_ASLEEP) {                     // curled around the body
    for (int a = 200; a <= 340; a += 10) {
      float r = a * 0.01745f;
      oled.drawPixel(bx + 13 + cosf(r) * 11, by + 4 + sinf(r) * 6, SSD1306_WHITE);
    }
    return;
  }
  float wag = sinf(frame * (m == MOOD_HAPPY ? 0.34f : 0.12f)) * (m == MOOD_HAPPY ? 7 : 3);
  for (int i = 0; i < 10; i++) {
    float t = i / 9.0f;
    int x = bx + 14 + i;
    int y = by + 6 - (int)(t * t * 9) + (int)(wag * t);
    oled.drawPixel(x, y, SSD1306_WHITE);
    oled.drawPixel(x, y + 1, SSD1306_WHITE);
  }
}

void catEars(int hx, int hy, CatMood m) {
  int droop = (m == MOOD_SLEEPY) ? 2 : (m == MOOD_ASLEEP ? 3 : 0);
  // left ear
  oled.drawLine(hx + 2, hy + 3, hx + 4, hy - 4 + droop, SSD1306_WHITE);
  oled.drawLine(hx + 4, hy - 4 + droop, hx + 9, hy + 1, SSD1306_WHITE);
  // right ear
  oled.drawLine(hx + 20, hy + 3, hx + 18, hy - 4 + droop, SSD1306_WHITE);
  oled.drawLine(hx + 18, hy - 4 + droop, hx + 13, hy + 1, SSD1306_WHITE);
}

void catFace(int hx, int hy, CatMood m) {
  int ex = lookDir;                          // pupil offset
  bool shut = (m == MOOD_ASLEEP) || blinking;

  if (shut) {                                // closed, happy arcs
    oled.drawLine(hx + 5, hy + 9, hx + 7, hy + 7, SSD1306_WHITE);
    oled.drawLine(hx + 7, hy + 7, hx + 9, hy + 9, SSD1306_WHITE);
    oled.drawLine(hx + 13, hy + 9, hx + 15, hy + 7, SSD1306_WHITE);
    oled.drawLine(hx + 15, hy + 7, hx + 17, hy + 9, SSD1306_WHITE);
  } else if (m == MOOD_SLEEPY) {             // half-lidded
    oled.fillRect(hx + 5, hy + 8, 5, 2, SSD1306_WHITE);
    oled.fillRect(hx + 13, hy + 8, 5, 2, SSD1306_WHITE);
  } else {
    oled.fillCircle(hx + 7 + ex, hy + 8, 2, SSD1306_WHITE);
    oled.fillCircle(hx + 15 + ex, hy + 8, 2, SSD1306_WHITE);
    if (m == MOOD_HAPPY) {                   // sparkle glint
      oled.drawPixel(hx + 8 + ex, hy + 7, SSD1306_BLACK);
      oled.drawPixel(hx + 16 + ex, hy + 7, SSD1306_BLACK);
    }
  }

  // nose
  oled.fillRect(hx + 10, hy + 12, 2, 2, SSD1306_WHITE);
  // mouth
  if (m == MOOD_HAPPY) {
    oled.drawLine(hx + 8, hy + 14, hx + 11, hy + 16, SSD1306_WHITE);
    oled.drawLine(hx + 11, hy + 16, hx + 14, hy + 14, SSD1306_WHITE);
  } else if (m == MOOD_BORED) {
    oled.drawFastHLine(hx + 8, hy + 15, 6, SSD1306_WHITE);
  } else if (m != MOOD_ASLEEP) {
    oled.drawLine(hx + 9, hy + 15, hx + 11, hy + 16, SSD1306_WHITE);
    oled.drawLine(hx + 11, hy + 16, hx + 13, hy + 15, SSD1306_WHITE);
  }

  // whiskers
  oled.drawFastHLine(hx - 4, hy + 12, 5, SSD1306_WHITE);
  oled.drawFastHLine(hx - 3, hy + 15, 4, SSD1306_WHITE);
  oled.drawFastHLine(hx + 21, hy + 12, 5, SSD1306_WHITE);
  oled.drawFastHLine(hx + 21, hy + 15, 4, SSD1306_WHITE);
}

// main cat renderer. cx,cy = top-left of the body block
void drawCat(int cx, int cy, CatMood m) {
  // breathing: body rises and falls, slower when asleep
  float rate = (m == MOOD_ASLEEP) ? 0.055f : (m == MOOD_SLEEPY ? 0.09f : 0.16f);
  int breathe = (int)(sinf(frame * rate) * 1.6f);

  if (m == MOOD_ASLEEP) {
    // curled up: low oval body, head tucked to the left
    oled.fillRoundRect(cx, cy + 6 + breathe, 30, 14, 7, SSD1306_WHITE);
    oled.fillRoundRect(cx + 2, cy + 8 + breathe, 26, 10, 5, SSD1306_BLACK);
    catTail(cx, cy + breathe, m);
    int hx = cx - 2, hy = cy + 2 + breathe;
    oled.fillRoundRect(hx, hy, 22, 18, 6, SSD1306_BLACK);
    oled.drawRoundRect(hx, hy, 22, 18, 6, SSD1306_WHITE);
    catEars(hx, hy, m);
    catFace(hx, hy, m);
    return;
  }

  // sitting pose
  oled.fillRoundRect(cx + 1, cy + 14 + breathe, 26, 18, 8, SSD1306_BLACK);
  oled.drawRoundRect(cx + 1, cy + 14 + breathe, 26, 18, 8, SSD1306_WHITE);
  // front paws
  oled.fillRoundRect(cx + 4, cy + 28 + breathe, 7, 4, 2, SSD1306_WHITE);
  oled.fillRoundRect(cx + 17, cy + 28 + breathe, 7, 4, 2, SSD1306_WHITE);
  catTail(cx, cy + 14 + breathe, m);

  int hx = cx + 3, hy = cy + breathe;
  oled.fillRoundRect(hx, hy, 22, 18, 6, SSD1306_BLACK);
  oled.drawRoundRect(hx, hy, 22, 18, 6, SSD1306_WHITE);
  catEars(hx, hy, m);
  catFace(hx, hy, m);

  if (purring && millis() < purrUntil) {      // purr squiggles
    for (int i = 0; i < 3; i++) {
      int py = cy + 2 + i * 5;
      int off = (int)(sinf(frame * 0.4f + i) * 2);
      oled.drawPixel(cx + 30 + off, py, SSD1306_WHITE);
      oled.drawPixel(cx + 32 + off, py, SSD1306_WHITE);
    }
  }
}

// mood + idle animation bookkeeping, called every frame
void catTick() {
  unsigned long idle = idleMs();
  CatMood want;
  if (idle > tAsleep)      want = MOOD_ASLEEP;
  else if (idle > tSleepy) want = MOOD_SLEEPY;
  else if (idle > tBored)  want = MOOD_BORED;
  else if (purring && millis() < purrUntil) want = MOOD_HAPPY;
  else want = MOOD_AWAKE;

  if (want != mood) {
    if (want == MOOD_ASLEEP) { statNaps++; beep(20); }
    mood = want;
  }

  if (mood == MOOD_ASLEEP) {
    if (frame % 55 == 0) zzzSpawn(72, 26);
  } else {
    if (millis() > nextBlink) {               // natural blinking
      blinking = true;
      blinkEnd = millis() + 120;
      nextBlink = millis() + random(1400, 3600);
    }
    if (blinking && millis() > blinkEnd) blinking = false;

    if (millis() > nextLook) {                // glance around when idle
      lookDir = (mood == MOOD_BORED) ? (int8_t)random(-1, 2) : 0;
      nextLook = millis() + random(1400, 3800);
    }
  }
  if (purring && millis() > purrUntil) purring = false;
}

void petTheCat() {
  purring = true;
  purrUntil = millis() + 2200;
  statPets++;
  sparkAt(58, 24, 6);
  beep(12, 2, 30);
}

// ============================================================
// SHARED UI BITS
// ============================================================
Star stars[10];

void starsInit() {
  for (uint8_t i = 0; i < 10; i++)
    stars[i] = {(int8_t)random(0, W), (int8_t)random(0, 40), (uint8_t)random(0, 8)};
}
void starsDraw() {
  for (uint8_t i = 0; i < 10; i++) {
    uint8_t ph = (frame / 6 + stars[i].phase) % 8;
    if (ph < 5) oled.drawPixel(stars[i].x, stars[i].y, SSD1306_WHITE);
    if (ph == 0) {
      oled.drawPixel(stars[i].x - 1, stars[i].y, SSD1306_WHITE);
      oled.drawPixel(stars[i].x + 1, stars[i].y, SSD1306_WHITE);
    }
  }
}

void titleBar(const char* t) {
  oled.fillRoundRect(0, 0, W, 13, 3, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  int len = strlen(t);
  oled.setCursor((W - len * 6) / 2, 3);
  oled.print(t);
  oled.setTextColor(SSD1306_WHITE);
}

void hintBar(const char* t) {
  oled.drawFastHLine(0, 53, W, SSD1306_WHITE);
  oled.setCursor(2, 56);
  oled.setTextSize(1);
  oled.print(t);
}

void pawCursor(int x, int y) {
  int bob = (frame / 8) % 2;
  oled.fillCircle(x, y + bob, 2, SSD1306_WHITE);
  oled.fillCircle(x - 3, y - 2 + bob, 1, SSD1306_WHITE);
  oled.fillCircle(x, y - 3 + bob, 1, SSD1306_WHITE);
  oled.fillCircle(x + 3, y - 2 + bob, 1, SSD1306_WHITE);
}

// ============================================================
// BOOT SPLASH  -  cat walks in, stretches, title reveals
// ============================================================
unsigned long bootStart = 0;

void screenBoot() {
  unsigned long t = millis() - bootStart;
  oled.clearDisplay();
  starsDraw();

  if (t < 1500) {
    int x = -34 + (int)(t * 0.058f);
    int hop = (int)(fabs(sinf(t * 0.012f)) * 3);
    drawCat(x, 24 - hop, MOOD_AWAKE);
  } else if (t < 2300) {
    drawCat(52, 24, MOOD_HAPPY);
    oled.setCursor(92, 30);
    oled.print("~");
  } else {
    drawCat(52, 24, MOOD_HAPPY);
    int rw = min((int)((t - 2300) / 6), 128);
    oled.fillRoundRect(64 - rw / 2, 2, rw, 16, 3, SSD1306_WHITE);
    if (rw > 60) {
      oled.setTextColor(SSD1306_BLACK);
      oled.setTextSize(2);
      oled.setCursor(26, 4);
      oled.print("SNOOZY");
      oled.setTextColor(SSD1306_WHITE);
      oled.setTextSize(1);
    }
    if (rw > 120) {                       // revision tag slides up
      char rv[10]; revString(rv);
      oled.setCursor(98, 54);
      oled.print(rv);
    }
  }
  sparkStep();
  oled.display();

  if (t > 3600) { screen = SCR_HOME; markInput(); catResetAnim(); beep(15, 2, 60); }
}

// ============================================================
// HOME
// ============================================================
void screenHome() {
  catTick();
  oled.clearDisplay();

  if (mood == MOOD_ASLEEP || mood == MOOD_SLEEPY) starsDraw();

  drawCat(48, 20, mood);
  zzzStep();
  sparkStep();

  oled.setTextSize(1);
  oled.setCursor(2, 2);
  const char* label =
      mood == MOOD_ASLEEP ? "zzz..." :
      mood == MOOD_SLEEPY ? "sleepy" :
      mood == MOOD_BORED  ? "bored"  :
      mood == MOOD_HAPPY  ? "purr!"  : "hello!";
  oled.print(label);

  if (mood != MOOD_ASLEEP) {
    unsigned long left = tAsleep > idleMs() ? tAsleep - idleMs() : 0;
    int pips = (int)((left * 10UL) / tAsleep);
    if (pips > 10) pips = 10;
    for (int i = 0; i < pips; i++) oled.drawPixel(2 + i * 3, 12, SSD1306_WHITE);
  }

  hintBar(mood == MOOD_ASLEEP ? "move stick to wake" : "X pet   Y menu");
  oled.display();

  if (gestureX() != 0) { if (mood != MOOD_ASLEEP) petTheCat(); }
  if (gestureFwd())    { screen = SCR_MENU; beep(18); sparkClear(); }
}

// ============================================================
// MAIN MENU  -  sliding card carousel
// ============================================================
const uint8_t MENU_N = 4;
const char* menuTitle[MENU_N] = {"Play", "Stats", "Settings", "Nap"};
int menuIdx = 0;
float menuSlide = 0;

void menuIcon(uint8_t which, int cx, int cy, bool big) {
  int s = big ? 2 : 1;
  switch (which) {
    case 0:
      oled.drawFastVLine(cx, cy - 3 * s, 6 * s, SSD1306_WHITE);
      oled.fillCircle(cx, cy - 4 * s, 2 * s, SSD1306_WHITE);
      oled.fillRoundRect(cx - 5 * s, cy + 3 * s, 10 * s, 4 * s, 2, SSD1306_WHITE);
      break;
    case 1:
      oled.fillRect(cx - 5 * s, cy + 1 * s, 3 * s, 4 * s, SSD1306_WHITE);
      oled.fillRect(cx - 1 * s, cy - 2 * s, 3 * s, 7 * s, SSD1306_WHITE);
      oled.fillRect(cx + 3 * s, cy - 5 * s, 3 * s, 10 * s, SSD1306_WHITE);
      break;
    case 2:
      oled.drawCircle(cx, cy, 5 * s, SSD1306_WHITE);
      oled.fillCircle(cx, cy, 2 * s, SSD1306_WHITE);
      for (int a = 0; a < 360; a += 60) {
        float r = a * 0.01745f;
        oled.drawPixel(cx + cosf(r) * 7 * s, cy + sinf(r) * 7 * s, SSD1306_WHITE);
      }
      break;
    case 3:
      oled.fillCircle(cx, cy, 6 * s, SSD1306_WHITE);
      oled.fillCircle(cx + 3 * s, cy - 2 * s, 5 * s, SSD1306_BLACK);
      break;
  }
}

void screenMenu() {
  float target = menuIdx;
  menuSlide += (target - menuSlide) * 0.28f;

  oled.clearDisplay();
  titleBar("MENU");

  for (uint8_t i = 0; i < MENU_N; i++) {
    float rel = i - menuSlide;
    if (fabs(rel) > 1.8f) continue;
    int cx = 64 + (int)(rel * 52);
    bool sel = (fabs(rel) < 0.4f);
    int cw = sel ? 46 : 34, ch = sel ? 34 : 26;
    int cy = 32 + (sel ? 0 : 3);

    oled.drawRoundRect(cx - cw / 2, cy - ch / 2, cw, ch, 5, SSD1306_WHITE);
    if (sel) oled.drawRoundRect(cx - cw / 2 - 2, cy - ch / 2 - 2, cw + 4, ch + 4, 6, SSD1306_WHITE);
    menuIcon(i, cx, cy - 3, sel);

    if (sel) {
      int len = strlen(menuTitle[i]);
      oled.setCursor(cx - len * 3, cy + ch / 2 - 8);
      oled.print(menuTitle[i]);
    }
  }

  for (uint8_t i = 0; i < MENU_N; i++) {
    int dx = 52 + i * 7;
    if (i == menuIdx) oled.fillCircle(dx, 50, 2, SSD1306_WHITE);
    else oled.drawCircle(dx, 50, 1, SSD1306_WHITE);
  }

  hintBar("X pick  Yf ok  Yb home");
  sparkStep();
  oled.display();

  int dx = gestureX();
  if (dx) { menuIdx = (menuIdx + dx + MENU_N) % MENU_N; beep(8); }
  if (gestureBack()) { screen = SCR_HOME; beep(12); }
  if (gestureFwd()) {
    beep(18);
    sparkAt(64, 32, 8);
    switch (menuIdx) {
      case 0: screen = SCR_GAMELIST; break;
      case 1: screen = SCR_STATS;    break;
      case 2: screen = SCR_SETTINGS; break;
      case 3: screen = SCR_SLEEP;    break;
    }
  }
}

// ============================================================
// GAME LIST  -  scrolling list, 6 entries
// ============================================================
const char* gameName[GAME_COUNT] = {
  "Cat Jump", "Mouse Catch", "Yarn Maze",
  "Paw Pong", "Copy Cat", "Whisker Whack"
};
int gameIdx = 0;

void screenGameList() {
  oled.clearDisplay();
  titleBar("PLAY");

  uint8_t first = 0;
  if (gameIdx > 2) first = gameIdx - 2;
  if (first > GAME_COUNT - 4) first = GAME_COUNT - 4;

  for (uint8_t i = first; i < GAME_COUNT && i < first + 4; i++) {
    int y = 17 + (i - first) * 9;
    bool sel = (i == gameIdx);
    if (sel) {
      oled.fillRoundRect(8, y - 1, 104, 9, 2, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    }
    oled.setCursor(22, y);
    oled.print(gameName[i]);
    oled.setTextColor(SSD1306_WHITE);
    if (sel) pawCursor(14, y + 4);
  }

  // scrollbar
  int barH = 36 / GAME_COUNT + 4;
  int barY = 17 + (gameIdx * (36 - barH)) / (GAME_COUNT - 1);
  oled.drawRect(120, 17, 4, 36, SSD1306_WHITE);
  oled.fillRect(121, barY, 2, barH, SSD1306_WHITE);

  hintBar("X move  Yf play  Yb back");
  oled.display();

  int dx = gestureX();
  if (dx) { gameIdx = (gameIdx + dx + GAME_COUNT) % GAME_COUNT; beep(8); }
  if (gestureBack()) { screen = SCR_MENU; beep(12); }
  if (gestureFwd()) {
    beep(18);
    switch (gameIdx) {
      case 0: screen = SCR_PLAY_JUMP;  break;
      case 1: screen = SCR_PLAY_CATCH; break;
      case 2: screen = SCR_PLAY_MAZE;  break;
      case 3: screen = SCR_PLAY_PONG;  break;
      case 4: screen = SCR_PLAY_COPY;  break;
      case 5: screen = SCR_PLAY_WHACK; break;
    }
  }
}

// shared end-of-game card
bool gameOverCard(const char* title, uint32_t sc, uint8_t slot) {
  bool isNew = (sc > statBest[slot]);
  if (isNew) statBest[slot] = sc;
  oled.clearDisplay();
  titleBar(title);
  oled.setTextSize(2);
  oled.setCursor(30, 20);
  oled.print(sc);
  oled.setTextSize(1);
  if (isNew && sc > 0) {
    oled.setCursor(76, 26);
    if ((frame / 8) % 2) oled.print("NEW!");
  }
  oled.setCursor(24, 40);
  oled.print("best ");
  oled.print(statBest[slot]);
  hintBar("Yf retry   Yb exit");
  sparkStep();
  oled.display();
  return isNew;
}

// ============================================================
// GAME 1  -  CAT JUMP
// ============================================================
float cjY, cjV; bool cjGround; uint32_t cjScore; float cjSpeed;
float cjObX[2]; uint8_t cjObH[2];
bool cjDead = false;
const int CJ_GROUND = 46;

void cjReset() {
  cjY = CJ_GROUND - 12; cjV = 0; cjGround = true;
  cjScore = 0; cjSpeed = 1.6f; cjDead = false;
  cjObX[0] = 140; cjObX[1] = 220;
  cjObH[0] = random(6, 13); cjObH[1] = random(6, 13);
  sparkClear();
}

void screenCatJump() {
  if (cjDead) {
    gameOverCard("CAT JUMP", cjScore, 0);
    if (gestureFwd())  { cjReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  if (ay() > 0.6f && cjGround) { cjV = -3.4f; cjGround = false; beep(10); markInput(); }
  cjV += 0.22f; cjY += cjV;
  if (cjY >= CJ_GROUND - 12) {
    cjY = CJ_GROUND - 12; cjV = 0;
    if (!cjGround) sparkAt(24, CJ_GROUND, 4);
    cjGround = true;
  }

  cjSpeed += 0.0012f;
  for (uint8_t i = 0; i < 2; i++) {
    cjObX[i] -= cjSpeed;
    if (cjObX[i] < -10) {
      cjObX[i] = 128 + random(20, 70);
      cjObH[i] = random(6, 13);
      cjScore++;
      sparkAt(4, 20, 3);
    }
    if (cjObX[i] < 30 && cjObX[i] > 8 && cjY + 12 > CJ_GROUND - cjObH[i]) {
      cjDead = true; beep(150); statPlays++;
    }
  }

  oled.clearDisplay();
  for (int i = 0; i < 3; i++) {
    int hx = (int)(-fmodf(frame * 0.35f + i * 55, 165)) + i * 55;
    oled.drawLine(hx, 40, hx + 16, 28, SSD1306_WHITE);
    oled.drawLine(hx + 16, 28, hx + 32, 40, SSD1306_WHITE);
  }
  oled.drawFastHLine(0, CJ_GROUND, W, SSD1306_WHITE);
  for (int i = 0; i < 6; i++) {
    int dx = (int)(-fmodf(frame * cjSpeed * 1.4f + i * 22, 132)) + i * 22;
    oled.drawFastHLine(dx, CJ_GROUND + 4, 7, SSD1306_WHITE);
  }

  int by = (int)cjY;
  oled.fillRoundRect(12, by + 4, 16, 8, 3, SSD1306_WHITE);
  oled.fillRoundRect(22, by, 10, 9, 3, SSD1306_WHITE);
  oled.drawPixel(30, by + 3, SSD1306_BLACK);
  oled.drawLine(23, by, 21, by - 4, SSD1306_WHITE);
  oled.drawLine(29, by, 31, by - 4, SSD1306_WHITE);
  if (cjGround) {
    int lp = (frame / 4) % 2;
    oled.drawFastVLine(15, by + 12, lp ? 3 : 1, SSD1306_WHITE);
    oled.drawFastVLine(24, by + 12, lp ? 1 : 3, SSD1306_WHITE);
  } else {
    oled.drawFastHLine(8, by + 6, 5, SSD1306_WHITE);
  }
  oled.drawLine(12, by + 5, 6, by + 1, SSD1306_WHITE);

  for (uint8_t i = 0; i < 2; i++)
    oled.fillRoundRect((int)cjObX[i], CJ_GROUND - cjObH[i], 7, cjObH[i], 2, SSD1306_WHITE);

  oled.setCursor(2, 2); oled.print(cjScore);
  sparkStep();
  oled.display();

  if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
}

// ============================================================
// GAME 2  -  MOUSE CATCH
// ============================================================
float mcX; float mcMX[3], mcMY[3]; bool mcIsBomb[3];
uint32_t mcScore; uint8_t mcLives; bool mcDead;

void mcReset() {
  mcX = 60; mcScore = 0; mcLives = 3; mcDead = false;
  for (uint8_t i = 0; i < 3; i++) {
    mcMX[i] = random(8, 118); mcMY[i] = -random(0, 70);
    mcIsBomb[i] = random(0, 100) < 22;
  }
  sparkClear();
}

void screenMouseCatch() {
  if (mcDead) {
    gameOverCard("MOUSE CATCH", mcScore, 1);
    if (gestureFwd())  { mcReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  float v = ax();
  if (v != 0) { mcX += v * 3.2f; markInput(); }
  mcX = constrain(mcX, 10.0f, 118.0f);

  oled.clearDisplay();
  oled.setCursor(2, 2);  oled.print(mcScore);
  for (uint8_t i = 0; i < mcLives; i++) oled.fillCircle(110 + i * 6, 5, 2, SSD1306_WHITE);

  for (uint8_t i = 0; i < 3; i++) {
    mcMY[i] += 0.9f + mcScore * 0.012f;
    if (mcMY[i] > 64) {
      if (!mcIsBomb[i]) {
        if (--mcLives == 0) { mcDead = true; beep(150); statPlays++; }
        else beep(50);
      }
      mcMY[i] = -random(6, 50); mcMX[i] = random(8, 118);
      mcIsBomb[i] = random(0, 100) < 22;
    }
    if (mcMY[i] > 44 && mcMY[i] < 54 && fabs(mcMX[i] - mcX) < 11) {
      if (mcIsBomb[i]) {
        if (--mcLives == 0) { mcDead = true; beep(150); statPlays++; }
        else beep(90);
      } else { mcScore++; sparkAt(mcMX[i], mcMY[i], 5); beep(8); }
      mcMY[i] = -random(6, 50); mcMX[i] = random(8, 118);
      mcIsBomb[i] = random(0, 100) < 22;
    }

    int x = (int)mcMX[i], y = (int)mcMY[i];
    if (mcIsBomb[i]) {
      oled.drawCircle(x, y, 4, SSD1306_WHITE);
      oled.drawLine(x + 3, y - 3, x + 6, y - 6, SSD1306_WHITE);
      if ((frame / 4) % 2) oled.drawPixel(x + 7, y - 7, SSD1306_WHITE);
    } else {
      oled.fillRoundRect(x - 4, y - 3, 9, 6, 3, SSD1306_WHITE);
      oled.fillCircle(x - 4, y - 4, 2, SSD1306_WHITE);
      oled.drawLine(x + 5, y, x + 9, y + (int)(sinf(frame * 0.3f + i) * 2), SSD1306_WHITE);
    }
  }

  int bx = (int)mcX;
  oled.drawRoundRect(bx - 11, 48, 22, 8, 3, SSD1306_WHITE);
  oled.drawFastHLine(bx - 8, 52, 16, SSD1306_WHITE);

  hintBar("X move   Yb exit");
  sparkStep();
  oled.display();

  if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
}

// ============================================================
// GAME 3  -  YARN MAZE
// ============================================================
const uint8_t MZ_W = 15, MZ_H = 9;
uint8_t maze[MZ_H][MZ_W];
int mzPX, mzPY, mzGX, mzGY;
uint32_t mzMoves; uint8_t mzLevel; bool mzWin;

void mzBuild() {
  for (uint8_t r = 0; r < MZ_H; r++)
    for (uint8_t c = 0; c < MZ_W; c++)
      maze[r][c] = (r == 0 || c == 0 || r == MZ_H - 1 || c == MZ_W - 1) ? 1
                 : (random(0, 100) < 26 ? 1 : 0);
  mzPX = 1; mzPY = 1;
  maze[1][1] = 0; maze[1][2] = 0; maze[2][1] = 0;
  mzGX = MZ_W - 2; mzGY = MZ_H - 2;
  maze[mzGY][mzGX] = 0; maze[mzGY - 1][mzGX] = 0; maze[mzGY][mzGX - 1] = 0;
}
void mzReset() { mzBuild(); mzMoves = 0; mzLevel = 1; mzWin = false; sparkClear(); }

void screenYarnMaze() {
  if (mzWin) {
    gameOverCard("YARN MAZE", mzLevel - 1, 2);
    if (gestureFwd())  { mzReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  static unsigned long lastStep = 0;
  if (millis() - lastStep > 150) {
    int dx = 0, dy = 0;
    float vx = ax(), vy = ay();
    if (fabs(vx) > 0.55f || fabs(vy) > 0.55f) {
      if (fabs(vx) > fabs(vy)) dx = vx > 0 ? 1 : -1;
      else                     dy = vy > 0 ? -1 : 1;   // stick fwd = up
      markInput();
    }
    if (dx || dy) {
      lastStep = millis();
      int nx = mzPX + dx, ny = mzPY + dy;
      if (maze[ny][nx] == 0) { mzPX = nx; mzPY = ny; mzMoves++; beep(4); }
      else beep(3);
      if (mzPX == mzGX && mzPY == mzGY) {
        mzLevel++;
        sparkAt(mzGX * 8 + 4, mzGY * 6 + 10, 10);
        beep(20, 3, 40);
        if (mzLevel > 5) { mzWin = true; statPlays++; }
        else mzBuild();
      }
    }
  }

  oled.clearDisplay();
  oled.setCursor(2, 1);  oled.print("Lv"); oled.print(mzLevel);
  oled.setCursor(92, 1); oled.print(mzMoves);

  for (uint8_t r = 0; r < MZ_H; r++)
    for (uint8_t c = 0; c < MZ_W; c++)
      if (maze[r][c]) oled.fillRect(c * 8 + 4, r * 6 + 9, 7, 5, SSD1306_WHITE);

  int gx = mzGX * 8 + 7, gy = mzGY * 6 + 11;
  oled.drawCircle(gx, gy, 2, SSD1306_WHITE);
  oled.drawPixel(gx + 3, gy + (int)(sinf(frame * 0.25f) * 1.5f), SSD1306_WHITE);

  int px = mzPX * 8 + 7, py = mzPY * 6 + 11;
  oled.fillCircle(px, py, 2, SSD1306_WHITE);
  oled.drawPixel(px - 2, py - 3, SSD1306_WHITE);
  oled.drawPixel(px + 2, py - 3, SSD1306_WHITE);

  sparkStep();
  oled.display();

  if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
}

// ============================================================
// GAME 4  -  PAW PONG
// Ball bounces off the walls, you keep it up with the paddle.
// Every few hits the ball speeds up and the paddle shrinks.
// ============================================================
float ppBX, ppBY, ppVX, ppVY, ppPX;
uint8_t ppW; uint32_t ppScore; uint8_t ppLives; bool ppDead;

void ppReset() {
  ppBX = 64; ppBY = 20;
  ppVX = (random(0, 2) ? 1.3f : -1.3f); ppVY = 1.5f;
  ppPX = 64; ppW = 26;
  ppScore = 0; ppLives = 3; ppDead = false;
  sparkClear();
}

void screenPawPong() {
  if (ppDead) {
    gameOverCard("PAW PONG", ppScore, 3);
    if (gestureFwd())  { ppReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  float v = ax();
  if (v != 0) { ppPX += v * 3.6f; markInput(); }
  ppPX = constrain(ppPX, (float)(ppW / 2), (float)(W - ppW / 2));

  ppBX += ppVX; ppBY += ppVY;

  if (ppBX < 3)      { ppBX = 3;      ppVX = -ppVX; beep(4); }
  if (ppBX > W - 4)  { ppBX = W - 4;  ppVX = -ppVX; beep(4); }
  if (ppBY < 16)     { ppBY = 16;     ppVY = -ppVY; beep(4); }

  // paddle
  const int PADY = 54;
  if (ppBY > PADY - 3 && ppBY < PADY + 3 && ppVY > 0) {
    if (fabs(ppBX - ppPX) < ppW / 2 + 2) {
      ppVY = -fabs(ppVY);
      float off = (ppBX - ppPX) / (ppW / 2.0f);     // english off the paddle
      ppVX += off * 0.7f;
      ppVX = constrain(ppVX, -2.8f, 2.8f);
      ppScore++;
      sparkAt(ppBX, PADY, 4);
      beep(8);
      if (ppScore % 4 == 0) {                        // ramp difficulty
        ppVY *= 1.09f;
        if (ppW > 12) ppW -= 2;
        beep(6, 2, 30);
      }
    }
  }

  if (ppBY > 64) {
    if (--ppLives == 0) { ppDead = true; beep(150); statPlays++; }
    else {
      beep(90);
      ppBX = 64; ppBY = 20;
      ppVX = (random(0, 2) ? 1.3f : -1.3f); ppVY = 1.5f;
    }
  }

  oled.clearDisplay();
  oled.setCursor(2, 2);  oled.print(ppScore);
  for (uint8_t i = 0; i < ppLives; i++) oled.fillCircle(110 + i * 6, 5, 2, SSD1306_WHITE);

  oled.drawFastHLine(0, 14, W, SSD1306_WHITE);
  for (int y = 16; y < 64; y += 6) {                 // side rails
    oled.drawPixel(0, y, SSD1306_WHITE);
    oled.drawPixel(W - 1, y, SSD1306_WHITE);
  }

  // ball is a little yarn ball with a wiggling thread
  oled.fillCircle((int)ppBX, (int)ppBY, 3, SSD1306_WHITE);
  oled.drawPixel((int)ppBX + 4, (int)ppBY + (int)(sinf(frame * 0.3f) * 2), SSD1306_WHITE);

  // paddle drawn as a paw
  int px = (int)ppPX;
  oled.fillRoundRect(px - ppW / 2, 54, ppW, 5, 2, SSD1306_WHITE);
  oled.fillCircle(px - ppW / 3, 52, 2, SSD1306_WHITE);
  oled.fillCircle(px,            51, 2, SSD1306_WHITE);
  oled.fillCircle(px + ppW / 3,  52, 2, SSD1306_WHITE);

  sparkStep();
  oled.display();

  if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
}

// ============================================================
// GAME 5  -  COPY CAT
// The cat plays a sequence of paw taps, you repeat it.
// Left / Right with X, Up with Y forward, Down with Y back.
// ============================================================
uint8_t ccSeq[32]; uint8_t ccLen; uint8_t ccStep;
uint8_t ccPhase;            // 0 show, 1 your turn, 2 correct, 3 wrong
uint8_t ccShowIdx;
unsigned long ccTimer;
uint32_t ccScore; bool ccDead;
int8_t ccFlash = -1;

void ccReset() {
  ccLen = 1; ccStep = 0; ccPhase = 0; ccShowIdx = 0;
  ccScore = 0; ccDead = false; ccFlash = -1;
  for (uint8_t i = 0; i < 32; i++) ccSeq[i] = random(0, 4);
  ccTimer = millis() + 600;
  sparkClear();
}

// pad 0 left, 1 right, 2 up, 3 down
void ccPad(uint8_t i, bool lit) {
  int cx = 64, cy = 34;
  int x = cx, y = cy;
  if (i == 0) x = cx - 34;
  if (i == 1) x = cx + 34;
  if (i == 2) y = cy - 15;
  if (i == 3) y = cy + 15;
  if (lit) {
    oled.fillRoundRect(x - 13, y - 8, 26, 16, 4, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
  } else {
    oled.drawRoundRect(x - 13, y - 8, 26, 16, 4, SSD1306_WHITE);
    oled.setTextColor(SSD1306_WHITE);
  }
  oled.setCursor(x - 8, y - 3);
  oled.print(i == 0 ? "<" : i == 1 ? ">" : i == 2 ? "Yf" : "Yb");
  oled.setTextColor(SSD1306_WHITE);
}

void screenCopyCat() {
  if (ccDead) {
    gameOverCard("COPY CAT", ccScore, 4);
    if (gestureFwd())  { ccReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  // ---- input ----
  // Poll every gesture each frame so their edge-detect state stays
  // fresh, then pick one. Without this a diagonal flick could fire two
  // gestures and the first would be silently overwritten.
  int  gx = gestureX();
  bool gf = gestureFwd();
  bool gb = gestureBack();

  int8_t pressed = -1;
  if (ccPhase == 1) {
    if      (gx < 0) pressed = 0;
    else if (gx > 0) pressed = 1;
    else if (gf)     pressed = 2;
    else if (gb)     pressed = 3;
  } else {
    // allow bailing out during playback
    if (gb) { screen = SCR_GAMELIST; beep(12); return; }
  }

  // ---- state machine ----
  if (ccPhase == 0) {                          // cat plays the sequence
    if (millis() > ccTimer) {
      if (ccShowIdx < ccLen) {
        ccFlash = ccSeq[ccShowIdx];
        beep(18);
        ccShowIdx++;
        ccTimer = millis() + 420;
      } else {
        ccFlash = -1;
        ccPhase = 1;
        ccStep = 0;
      }
    }
    if (ccFlash >= 0 && millis() > ccTimer - 170) ccFlash = -1;
  }
  else if (ccPhase == 1 && pressed >= 0) {     // your turn
    ccFlash = pressed;
    if (pressed == ccSeq[ccStep]) {
      beep(12);
      ccStep++;
      if (ccStep >= ccLen) {
        ccScore++;
        sparkAt(64, 34, 8);
        ccPhase = 2;
        ccTimer = millis() + 550;
      }
    } else {
      beep(160);
      ccPhase = 3;
      ccTimer = millis() + 800;
    }
  }
  else if (ccPhase == 2 && millis() > ccTimer) {   // next round
    if (ccLen < 32) ccLen++;
    ccShowIdx = 0; ccPhase = 0;
    ccTimer = millis() + 400;
  }
  else if (ccPhase == 3 && millis() > ccTimer) {   // failed
    ccDead = true; statPlays++;
  }

  // ---- draw ----
  oled.clearDisplay();
  oled.setCursor(2, 2);  oled.print(ccScore);
  oled.setCursor(46, 2);
  oled.print(ccPhase == 0 ? "watch" : ccPhase == 1 ? "your turn" :
             ccPhase == 2 ? "nice!" : "oops");
  oled.setCursor(108, 2); oled.print(ccStep); oled.print('/'); oled.print(ccLen);

  for (uint8_t i = 0; i < 4; i++) ccPad(i, ccFlash == i);

  // the cat watches from the middle
  if (ccPhase == 2)      oled.setCursor(58, 30), oled.print(":3");
  else if (ccPhase == 3) oled.setCursor(58, 30), oled.print(":<");
  else                   oled.setCursor(58, 30), oled.print((frame / 16) % 2 ? "^v" : "^^");

  sparkStep();
  oled.display();
}

// ============================================================
// GAME 6  -  WHISKER WHACK
// Mice pop out of six holes. Move the paw over one and press
// Y forward to bop it. They get faster as you score.
// ============================================================
int8_t wwUp;                // which hole currently has a mouse, -1 none
uint8_t wwSel;              // paw position 0..5
unsigned long wwNext, wwGone;
uint32_t wwScore; uint8_t wwMiss; bool wwDead;
int wwPopMs;
int8_t wwHitFx = -1; unsigned long wwHitFxUntil = 0;

void wwReset() {
  wwUp = -1; wwSel = 0;
  wwScore = 0; wwMiss = 0; wwDead = false;
  wwPopMs = 1100;
  wwNext = millis() + 600;
  wwGone = 0;
  wwHitFx = -1;
  sparkClear();
}

void wwHoleXY(uint8_t i, int &x, int &y) {
  x = 22 + (i % 3) * 42;
  y = 26 + (i / 3) * 20;
}

void screenWhiskerWhack() {
  if (wwDead) {
    gameOverCard("WHISKER WHACK", wwScore, 5);
    if (gestureFwd())  { wwReset(); statPlays++; }
    if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
    return;
  }

  // move the paw
  int dx = gestureX();
  if (dx) { wwSel = (wwSel + dx + 6) % 6; beep(5); }

  // whack
  if (gestureFwd()) {
    if (wwUp >= 0 && wwSel == (uint8_t)wwUp) {
      wwScore++;
      int hx, hy; wwHoleXY(wwUp, hx, hy);
      sparkAt(hx, hy, 6);
      wwHitFx = wwUp; wwHitFxUntil = millis() + 220;
      beep(10);
      wwUp = -1;
      wwNext = millis() + random(260, 620);
      if (wwPopMs > 420) wwPopMs -= 28;          // speed up
    } else {
      beep(70);                                   // swing and a miss
    }
  }

  // pop a new mouse
  if (wwUp < 0 && millis() > wwNext) {
    wwUp = random(0, 6);
    wwGone = millis() + wwPopMs;
  }
  // mouse escaped
  if (wwUp >= 0 && millis() > wwGone) {
    wwUp = -1;
    wwNext = millis() + random(260, 620);
    if (++wwMiss >= 3) { wwDead = true; beep(150); statPlays++; }
    else beep(60);
  }
  if (wwHitFx >= 0 && millis() > wwHitFxUntil) wwHitFx = -1;

  oled.clearDisplay();
  oled.setCursor(2, 2);  oled.print(wwScore);
  oled.setCursor(52, 2); oled.print("miss ");
  for (uint8_t i = 0; i < 3; i++) {
    if (i < wwMiss) oled.fillCircle(84 + i * 7, 5, 2, SSD1306_WHITE);
    else            oled.drawCircle(84 + i * 7, 5, 2, SSD1306_WHITE);
  }

  for (uint8_t i = 0; i < 6; i++) {
    int hx, hy; wwHoleXY(i, hx, hy);
    oled.drawRoundRect(hx - 12, hy - 2, 24, 9, 4, SSD1306_WHITE);   // hole rim

    if (wwHitFx == i) {                          // bonk stars
      oled.setCursor(hx - 6, hy - 12); oled.print("*  *");
    } else if (wwUp == (int8_t)i) {              // mouse peeking, rises a bit
      float t = 1.0f - (float)(wwGone - millis()) / (float)wwPopMs;
      int rise = (int)(6 * sinf(constrain(t, 0.0f, 1.0f) * 3.14159f));
      int my = hy - rise;
      oled.fillRoundRect(hx - 5, my - 4, 11, 7, 3, SSD1306_WHITE);
      oled.fillCircle(hx - 5, my - 5, 2, SSD1306_WHITE);
      oled.drawPixel(hx + 2, my - 1, SSD1306_BLACK);
    }

    if (wwSel == i) {                            // the paw cursor
      int bob = (frame / 6) % 2;
      oled.fillRoundRect(hx - 7, hy + 8 + bob, 14, 5, 2, SSD1306_WHITE);
      oled.fillCircle(hx - 4, hy + 7 + bob, 1, SSD1306_WHITE);
      oled.fillCircle(hx,     hy + 6 + bob, 1, SSD1306_WHITE);
      oled.fillCircle(hx + 4, hy + 7 + bob, 1, SSD1306_WHITE);
    }
  }

  sparkStep();
  oled.display();

  if (gestureBack()) { screen = SCR_GAMELIST; beep(12); }
}

void resetAllGames() {
  cjReset(); mcReset(); mzReset();
  ppReset(); ccReset(); wwReset();
}

// ============================================================
// STATS  -  two pages, X flips between them
// ============================================================
uint8_t statPage = 0;

void screenStats() {
  oled.clearDisplay();
  titleBar(statPage == 0 ? "STATS" : "BEST SCORES");

  if (statPage == 0) {
    oled.setCursor(4, 18); oled.print("Games played "); oled.print(statPlays);
    oled.setCursor(4, 28); oled.print("Naps taken   "); oled.print(statNaps);
    oled.setCursor(4, 38); oled.print("Pets given   "); oled.print(statPets);
    char rv[10]; revString(rv);
    oled.setCursor(4, 48); oled.print("Snoozy "); oled.print(rv);
    drawCat(96, 26, MOOD_HAPPY);
  } else {
    for (uint8_t i = 0; i < GAME_COUNT; i++) {
      int col = i / 3, row = i % 3;
      int x = 4 + col * 64, y = 17 + row * 11;
      oled.setCursor(x, y);
      char sn[10];
      strncpy(sn, gameName[i], 8); sn[8] = 0;
      oled.print(sn);
      oled.setCursor(x + 50, y);
      oled.print(statBest[i]);
    }
  }

  hintBar("X page   Yb back");
  oled.display();

  if (gestureX()) { statPage ^= 1; beep(8); }
  if (gestureBack()) { screen = SCR_MENU; beep(12); }
}

// ============================================================
// SETTINGS
// ============================================================
const uint8_t SET_N = 4;
int setIdx = 0;

void screenSettings() {
  oled.clearDisplay();
  titleBar("SETTINGS");

  const char* rows[SET_N] = {"Sound", "Bright", "Sleep after", "Reset stats"};
  for (uint8_t i = 0; i < SET_N; i++) {
    int y = 17 + i * 9;
    bool sel = (i == setIdx);
    if (sel) {
      oled.fillRoundRect(2, y - 1, 124, 9, 2, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    }
    oled.setCursor(6, y);
    oled.print(rows[i]);
    oled.setCursor(80, y);
    if (i == 0) oled.print(soundOn ? "ON" : "OFF");
    if (i == 1) oled.print(brightness);
    if (i == 2) { oled.print(tAsleep / 1000); oled.print("s"); }
    if (i == 3) oled.print("Yf");
    oled.setTextColor(SSD1306_WHITE);
  }

  hintBar("X row  Yf change  Yb back");
  oled.display();

  int dx = gestureX();
  if (dx) { setIdx = (setIdx + dx + SET_N) % SET_N; beep(8); }
  if (gestureBack()) { screen = SCR_MENU; beep(12); }
  if (gestureFwd()) {
    beep(14);
    switch (setIdx) {
      case 0:
        soundOn = !soundOn;
        break;
      case 1:
        brightness = (brightness >= 250) ? 40 : brightness + 70;
        oled.ssd1306_command(SSD1306_SETCONTRAST);
        oled.ssd1306_command(brightness);
        break;
      case 2:
        tAsleep = (tAsleep >= 60000) ? 15000 : tAsleep + 15000;
        tSleepy = tAsleep * 6 / 10;
        tBored  = tAsleep * 3 / 10;
        tDeep   = tAsleep + 34000;
        break;
      case 3:
        statPlays = statNaps = statPets = 0;
        for (uint8_t i = 0; i < GAME_COUNT; i++) statBest[i] = 0;
        sparkAt(64, 32, 10);
        beep(20, 2, 60);
        break;
    }
  }
}

// ============================================================
// NAP  -  screen off, wakes on stick movement
// ============================================================
unsigned long napEnteredAt = 0;
bool napCurtain = false;

void screenSleep() {
  if (!napCurtain) {
    for (int i = 0; i <= 32; i += 2) {
      oled.clearDisplay();
      drawCat(48, 20, MOOD_ASLEEP);
      oled.fillRect(0, 0, W, i, SSD1306_BLACK);
      oled.fillRect(0, 64 - i, W, i, SSD1306_BLACK);
      oled.display();
      delay(18);
    }
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
    napCurtain = true;
    napEnteredAt = millis();
    mood = MOOD_ASLEEP;
    statNaps++;
  }

  if (fabs(ax()) > 0.5f || fabs(ay()) > 0.5f) {
    oled.ssd1306_command(SSD1306_DISPLAYON);
    napCurtain = false;
    markInput();
    catResetAnim();
    screen = SCR_HOME;
    beep(10, 2, 50);
    return;
  }

  if (millis() - napEnteredAt > tDeep) {
    napCurtain = false;
    screen = SCR_DEEPSLEEP;
    return;
  }
  delay(60);
}

// ============================================================
// DEEP SLEEP  -  light-sleep bursts, stick wakes it
// ============================================================
void screenDeepSleep() {
  // ADC pins cannot wake the chip, so nap in short bursts and
  // check the stick in between. Still saves a lot of power.
  esp_sleep_enable_timer_wakeup(250000);      // 250 ms
  esp_light_sleep_start();

  if (fabs(ax()) > 0.5f || fabs(ay()) > 0.5f) {
    oled.ssd1306_command(SSD1306_DISPLAYON);
    markInput();
    catResetAnim();
    for (int i = 0; i < 18; i++) {            // stretch and yawn
      oled.clearDisplay();
      drawCat(48, 20, i < 9 ? MOOD_SLEEPY : MOOD_AWAKE);
      oled.setCursor(30, 4);
      oled.print(i < 9 ? "mrrp?" : "hello!");
      oled.display();
      frame++;
      delay(45);
    }
    beep(10, 3, 40);
    screen = SCR_HOME;
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  analogReadResolution(12);
  randomSeed(analogRead(39) ^ esp_random());

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED not found - check SDA/SCL wiring"));
    while (true) delay(1000);
  }
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(brightness);
  oled.clearDisplay();
  oled.display();

  char rv[10]; revString(rv);
  Serial.print(F("Snoozy ")); Serial.println(rv);

  starsInit();
  catResetAnim();
  resetAllGames();
  bootStart = millis();
  lastInputMs = millis();
  screen = SCR_BOOT;
}

void loop() {
  frame++;

  if (screen != SCR_SLEEP && screen != SCR_DEEPSLEEP) {
    if (fabs(ax()) > 0.25f || fabs(ay()) > 0.25f) markInput();
  }

  // auto-nap from the home screen only
  if (screen == SCR_HOME && idleMs() > tDeep) screen = SCR_SLEEP;

  switch (screen) {
    case SCR_BOOT:       screenBoot();          break;
    case SCR_HOME:       screenHome();          break;
    case SCR_MENU:       screenMenu();          break;
    case SCR_GAMELIST:   screenGameList();      break;
    case SCR_PLAY_JUMP:  screenCatJump();       break;
    case SCR_PLAY_CATCH: screenMouseCatch();    break;
    case SCR_PLAY_MAZE:  screenYarnMaze();      break;
    case SCR_PLAY_PONG:  screenPawPong();       break;
    case SCR_PLAY_COPY:  screenCopyCat();       break;
    case SCR_PLAY_WHACK: screenWhiskerWhack();  break;
    case SCR_STATS:      screenStats();         break;
    case SCR_SETTINGS:   screenSettings();      break;
    case SCR_SLEEP:      screenSleep();         break;
    case SCR_DEEPSLEEP:  screenDeepSleep();     break;
  }

  // entering a game fresh resets it
  if (screen != lastScreen) {
    switch (screen) {
      case SCR_PLAY_JUMP:  cjReset(); break;
      case SCR_PLAY_CATCH: mcReset(); break;
      case SCR_PLAY_MAZE:  mzReset(); break;
      case SCR_PLAY_PONG:  ppReset(); break;
      case SCR_PLAY_COPY:  ccReset(); break;
      case SCR_PLAY_WHACK: wwReset(); break;
      default: break;
    }
    lastScreen = screen;
  }

  delay(16);                                    // ~60 fps ceiling
}
