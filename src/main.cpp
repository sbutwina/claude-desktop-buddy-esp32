// Arduino_GFX's font headers reference an undefined U8G2_FONT_SECTION
// macro — provide a no-op so the const array compiles. The font symbol
// itself is gated on U8G2_USE_LARGE_FONTS (set in build_flags).
#define U8G2_FONT_SECTION(name)
#include <Arduino_GFX_Library.h>

#include "hw/hw.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_mac.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// TFT_eSPI used to define these named colors; Arduino_GFX uses
// RGB565_*. Keep the names so existing UI code compiles unchanged.
#define GREEN  0x07E0
#define RED    0xF800
#define BLUE   0x001F
#define YELLOW 0xFFE0
#define WHITE  0xFFFF
#define BLACK  0x0000

// spr is a thin alias for hwCanvas() — keeps existing UI code unchanged
#define spr (*hwCanvas())

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#include "session_dots.h"
const int W = HW_W;
const int H = HW_H;
const int CX = W / 2;
const int CY_BASE = 120;
// LED replaced by AMOLED border-flash via hwBorderAlert() — no GPIO LED.

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
// Battery-only: dims (not sleeps) the screen when Claude is busy and idle,
// so the last frame stays glanceable instead of going fully blank. Kept
// separate from `napping` — that one feeds statsOnNapEnd()/energy-recharge
// in stats.h, and this isn't a real face-down nap.
bool     busyDimmed = false;
bool     swallowBtnB = false;
bool     swallowBtnBoot = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void prevPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → last species
    buddyMode = true;
    buddySetSpeciesIdx(n - 1);
    speciesIdxSave(n - 1);
  } else if (buddySpeciesIdx() == 0 && gifAvailable) {      // first species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyPrevSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS    = 30UL * 1000UL;        // 30s on battery, non-clock idle
const uint32_t CLOCK_OFF_MS_BAT = 5UL  * 60UL * 1000UL; // 5min on battery, clock visible

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { hwDisplayBrightness(brightLevel); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    hwDisplaySleep(false);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
  if (busyDimmed) { applyBrightness(); busyDimmed = false; }
}
// wake() arms a 12 s window that forces baseState to P_SLEEP so the buddy is
// seen waking up. That read well when P_SLEEP drew a sleeping buddy; with the
// dotsOnly render path P_SLEEP paints a blank screen instead, which is
// indistinguishable from the panel still being off. A wake the user asked for
// by pressing a button should show the UI immediately, so skip the transition.
static inline void wakeForUser() {
  wake();
  wakeTransitionUntil = 0;
}

bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) hwBeep(freq, dur);
}

// Press-start snapshot, read on justReleased to classify a stationary tap
// (small Δx/Δy, short Δt) vs. a drag — touch only reacts to the former.
static int16_t  _tpStartX = 0, _tpStartY = 0;
static uint32_t _tpStartMs = 0;

// After a user interaction in clock mode (pet tap or species swipe), keep the
// buddy awake for this long — otherwise the time-of-day logic snaps back to
// P_SLEEP the instant the one-shot animation expires.
static const uint32_t PLAYFUL_MS = 3UL * 60UL * 1000UL;
static uint32_t _playfulUntil = 0;

// ─── Serial instrumentation for `pio device monitor` ────────────────────
// Enabled by -DBTN_DEBUG (set in platformio.ini). Emits two line types, both
// timestamped in ms so they interleave in causal order:
//
//   [   12345] BTN PWR tap held=87 -> screen OFF
//   [   12346] ST  active=sleep base=sleep dots=1 off=0 nap=0 ...
//
// BTN lines record every button edge *and* the action it dispatched; ST lines
// print the render/persona tuple only when it changes. Together they answer
// "I pressed X and the screen went black" — grep ^.*BTN or ^.*ST.
//
// NOTE: -DBRIDGE_DEBUG echoes every incoming JSON heartbeat and will bury
// these. Comment it out in platformio.ini while doing button runs.
#ifdef BTN_DEBUG
  #define BTNLOG(fmt, ...) Serial.printf("[%8lu] BTN " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
#else
  #define BTNLOG(fmt, ...) do {} while (0)
#endif

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 7;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 6;

// Long-press thresholds. PWR's is deliberately longer than the others —
// it powers the device off, so it should be hard to trigger by accident.
const uint32_t LONG_MS     = 600;
const uint32_t PWR_LONG_MS = 1200;

// At 0 rotation PWR sits below KEY (physical layout flips), so PWR should
// drive the cursor down and KEY up there; 180/270 keep PWR up, KEY down.
static inline bool pwrIsCursorUp() { return settings().rotation != 0; }

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillScreen(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "volume", "sound", "bluetooth", "led", "transcript", "clock rot", "ascii pet", "uptime", "rotation", "reset", "back" };
const uint8_t SETTINGS_N = 12;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1:
      s.volume = (uint8_t)((s.volume + 20) % 120);   // 0,20,40,60,80,100,0,...
      hwAudioSetVolume(s.volume);
      beep(1800, 40);   // audible feedback at the new level
      break;
    case 2: s.sound = !s.sound; break;
    case 3:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: s.showUptime = !s.showUptime; break;
    case 9: {
      // 90CW dropped — it puts the buttons on the bottom edge, not practical.
      static const uint8_t ROT_CYCLE[] = { 0, 2, 3 };
      uint8_t i = (s.rotation == 2) ? 1 : (s.rotation == 3) ? 2 : 0;
      s.rotation = ROT_CYCLE[(i + 1) % 3];
      hwDisplaySetRotation(s.rotation);
      break;
    }
    case 10: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 11: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "▲▼ up/down   ▶ select" with pixel
// triangles. Panels add MENU_HINT_H to height and call this at bottom. Text
// is fixed — every panel means the same two actions, so no per-call labels.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy) {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + 8;
  spr.fillTriangle(x, hy + 5, x + 6, hy + 5, x + 3, hy, p.textDim);       // ▲ up
  spr.fillTriangle(x + 9, hy, x + 15, hy, x + 12, hy + 5, p.textDim);     // ▼ down
  x += 20;
  spr.setCursor(x, hy); spr.print("up/down");
  x = mx + mw / 2 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);          // ▶ select
  x += 9;
  spr.setCursor(x, hy); spr.print("select");
}

// Floating arrow glyphs drawn next to each physical button's current screen
// edge, so the on-screen hint tracks the button no matter how the display is
// rotated. The button strip is fixed to the case; only which edge it lines
// up with (and, for PWR/KEY, which direction each one drives) changes with
// settings().rotation. BOOT never changes meaning — always "select".
enum { HINT_UP, HINT_DOWN, HINT_SELECT };

static void drawHintGlyph(int x, int y, uint8_t glyph, uint16_t col) {
  switch (glyph) {
    case HINT_UP:     spr.fillTriangle(x - 5, y + 4, x + 5, y + 4, x, y - 5, col); break;
    case HINT_DOWN:   spr.fillTriangle(x - 5, y - 4, x + 5, y - 4, x, y + 5, col); break;
    case HINT_SELECT: spr.fillTriangle(x - 4, y - 5, x - 4, y + 5, x + 5, y, col); break;
  }
}

static void drawButtonHints() {
  const Palette& p = characterPalette();
  uint8_t pwrGlyph = pwrIsCursorUp() ? HINT_UP : HINT_DOWN;
  uint8_t keyGlyph = pwrIsCursorUp() ? HINT_DOWN : HINT_UP;

  // slot[0..2] = glyph nearest-edge-corner -> farthest, per the physical
  // layout confirmed against the case (rotation 0 vs 2/3 differ; 2 and 3
  // share the same order since pwrIsCursorUp() agrees for both).
  uint8_t slot[3];
  if (settings().rotation == 0) { slot[0] = keyGlyph; slot[1] = pwrGlyph; slot[2] = HINT_SELECT; }
  else                          { slot[0] = HINT_SELECT; slot[1] = pwrGlyph; slot[2] = keyGlyph; }

  const int MARGIN = 12;   // eyeball against the physical case; adjust if off
  if (settings().rotation == 3) {
    for (int i = 0; i < 3; i++) drawHintGlyph(W / 4 + i * (W / 4), MARGIN, slot[i], p.textDim);
  } else {
    int x = (settings().rotation == 0) ? MARGIN : W - MARGIN;
    for (int i = 0; i < 3; i++) drawHintGlyph(x, H / 4 + i * (H / 4), slot[i], p.textDim);
  }
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i == 1) {
      spr.printf("%u", s.volume);
    } else if (i >= 2 && i <= 5) {
      spr.setTextColor(vals[i-2] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-2] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    } else if (i == 8) {
      spr.setTextColor(s.showUptime ? GREEN : p.textDim, PANEL);
      spr.print(s.showUptime ? " on" : "off");
    } else if (i == 9) {
      spr.print(s.rotation == 0 ? "0" : s.rotation == 2 ? "180" : "270");
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: hwPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Portrait-only clock on AMOLED port (landscape removed — 368×448 is
// near-square; rotating doesn't change the layout meaningfully).
static HwTime  _clkTm;
uint32_t       _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool    _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = hwBattery().usbPresent;
  hwRtcRead(&_clkTm);
}

// Clock face: shown when charging on USB with nothing else going on.
// Paints the upper ~110px to the canvas; pet renders below.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkTm.dow % 7; }
// Manual centered-text helper (Arduino_GFX has no setTextDatum). Default
// font is 6 px wide × 8 px tall; multiply by textSize for placement.
static void drawCenteredText(const char* s, int cx, int cy, int sz, uint16_t fg, uint16_t bg) {
  int w = (int)strlen(s) * 6 * sz;
  int h = 8 * sz;
  spr.setTextSize(sz);
  spr.setTextColor(fg, bg);
  spr.setCursor(cx - w/2, cy - h/2);
  spr.print(s);
}
static void drawClock() {
  const Palette& p = characterPalette();
  char hms[12]; snprintf(hms, sizeof(hms), "%02u:%02u:%02u", _clkTm.H, _clkTm.M, _clkTm.S);
  uint8_t mi = (_clkTm.Mo >= 1 && _clkTm.Mo <= 12) ? _clkTm.Mo - 1 : 0;
  char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkTm.D);

  // Compact clock: single-line HH:MM:SS plus date below. Clears only
  // y >= 140 so the buddy at full home scale (reaches y≈126) fits
  // entirely above. Wider canvas + portrait orientation has plenty of
  // horizontal room for HH:MM:SS at size 3 (8 chars × 18 = 144 px).
  spr.fillRect(0, 140, W, H - 140, p.bg);
  drawCenteredText(hms, CX, 160, 3, p.text,    p.bg);
  drawCenteredText(dl,  CX, SAFE_B - 21, 1, p.textDim, p.bg);
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 1)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SAFE_L, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_R - 24, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(SAFE_L, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillScreen(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(SAFE_L, SAFE_B - 32); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(SAFE_L, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("PWR");
    spr.setTextColor(p.textDim, p.bg); ln("    screen on/off");
    ln("    hold: power off"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("+/KEY");
    spr.setTextColor(p.textDim, p.bg); ln("    stats page");
    ln("    hold: menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("BOOT");
    spr.setTextColor(p.textDim, p.bg); ln("    info pages");
    ln("    hold: mute"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("in menu");
    spr.setTextColor(p.textDim, p.bg);
    ln(pwrIsCursorUp() ? "    PWR up, KEY down" : "    PWR down, KEY up");
    ln("    BOOT selects"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("on prompt");
    spr.setTextColor(p.textDim, p.bg); ln("    KEY yes, BOOT no");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    HwBattery hb = hwBattery();
    int vBat_mV  = hb.mV;
    int vBus_mV  = hb.usbPresent ? 5000 : 0;
    int pct      = hb.pct;
    bool usb     = hb.usbPresent;
    bool charging = hb.charging;
    bool full    = usb && vBat_mV > 4100 && !charging;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(SAFE_L, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    if (settings().showUptime) {
      uint32_t last = stats().lastSessionSeconds;
      ln("  last     %luh %02lum", last / 3600, (last / 60) % 60);
    }
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    if (settings().showUptime) {
      uint32_t up = millis() / 1000;
      ln("  session  %luh %02lum", up / 3600, (up / 60) % 60);
    }
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)hwBattery().tempC);

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    ln("%s", linked ? "linked" : (settings().bt ? "discover" : "off"));
    y += 4;

    spr.setTextColor(p.text, p.bg);
    ln("%s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("%02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 4;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln("Open Claude desktop");
      ln("> Developer");
      ln("> Hardware Buddy");
      y += 4;
      ln("auto-connects via BLE");
    }

  } else if (infoPage == 5) {
    _infoHeader(p, y, "STATS", infoPage);
    spr.setTextColor(p.body, p.bg);    ln("MOOD");
    spr.setTextColor(p.textDim, p.bg); ln(" approve fast = up");
    ln(" deny lots = down"); y += 4;
    spr.setTextColor(p.body, p.bg);    ln("FED");
    spr.setTextColor(p.textDim, p.bg); ln(" 50K tokens =");
    ln(" level up + confetti"); y += 4;
    spr.setTextColor(p.body, p.bg);    ln("ENERGY");
    spr.setTextColor(p.textDim, p.bg); ln(" face-down to nap");
    ln(" refills to full");

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware adaptation");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("yadong");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln(BOARD_MODEL_LINE1);
    ln(BOARD_MODEL_LINE2);
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
// UTF-8 continuation byte = 0b10xxxxxx. Pull `take` back so we never
// land mid-codepoint when hard-breaking long Chinese sentences.
static uint8_t _utf8SafeTake(const char* w, uint8_t take, uint8_t wlen) {
  if (take == 0 || take >= wlen) return take;
  while (take > 0 && ((uint8_t)w[take] & 0xC0) == 0x80) take--;
  return take;
}

static uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit, on UTF-8 char boundaries
    while (wlen > width - col) {
      uint8_t take = _utf8SafeTake(w, width - col, wlen);
      if (take == 0) take = 1;                 // safety: avoid infinite loop
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(SAFE_L, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(SAFE_L, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(SAFE_L, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(SAFE_R - 48, SAFE_B - 12);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(SAFE_L, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(SAFE_L, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(SAFE_L, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(SAFE_L + 5, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SAFE_L, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(SAFE_L, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(SAFE_L, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(SAFE_L, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  drawPetStats(p);

  // Header on top — just the name; single page now, so no counter
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SAFE_L, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // chill7 font: glyphs ~7 px tall but baseline-positioned (setCursor
  // is the baseline, not the top). Allow ~10 px line spacing, ~22 byte
  // budget per line — Chinese chars are ~7 px wide, ASCII ~5 px, so a
  // mixed line of 22 bytes (~7 Chinese OR 22 ASCII) fits W=184.
  const int SHOW = 3, LH = 10, WIDTH = 22;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);

  // Menu/settings/reset should hide the HUD strip underneath — panels are
  // centered and don't cover the bottom 34 px on their own.
  if (menuOpen || settingsOpen || resetOpen) return;

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  // buddy/character ticks leave textsize at 2 (home scale); without
  // pinning it here the CJK font alternates between 1× and 2× every tick.
  spr.setTextSize(1);
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(SAFE_L, SAFE_B - 4);
    spr.print(tama.msg);
    spr.setFont((const GFXfont*)NULL);
    return;
  }

  static char disp[32][48];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(SAFE_L, H - AREA + 8 + i * LH);   // +8 = baseline offset for 7-px font
    spr.print(disp[row]);
  }

  spr.setFont((const GFXfont*)NULL);

  if (msgScroll > 0) {
    spr.setTextSize(1);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(SAFE_R - 18, SAFE_B - 10);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  hwInit();                  // Wire + expander + display + power + input + IMU + RTC + audio
  startBt();                 // BLE stays always-on
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  hwDisplaySetRotation(settings().rotation);
  hwAudioSetVolume(settings().volume);
  petNameLoad();
  buddyInit();

  characterInit(nullptr);    // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF).
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      drawCenteredText(line,      W/2, H/2 - 12, 2, p.text, p.bg);
      drawCenteredText(petName(), W/2, H/2 + 12, 2, p.body, p.bg);
    } else {
      drawCenteredText("Hello!",          W/2, H/2 - 12, 2, p.body,    p.bg);
      drawCenteredText("a buddy appears", W/2, H/2 + 12, 1, p.textDim, p.bg);
    }
    spr.setTextSize(1);
    hwDisplayPush();
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

#ifdef BTN_DEBUG
// Prints only on change, so an idle device stays quiet and every line marks a
// real transition. dotsOnly is the one to watch: dots=1 with off=0 means the
// panel is powered and being deliberately painted blank + dots.
static void stateLog(bool dotsOnly, bool clocking, bool inPrompt) {
  static const char* const MODE[] = { "NORM", "PET", "INFO" };
  static char prev[192] = "";
  char cur[192];
  snprintf(cur, sizeof(cur),
           "active=%s base=%s dots=%d off=%d dim=%d bdim=%d nap=%d "
           "mode=%s pg=%u clk=%d prompt=%d menu=%d set=%d rst=%d",
           stateNames[activeState], stateNames[baseState],
           dotsOnly, screenOff, dimmed, busyDimmed, napping,
           MODE[displayMode < DISP_COUNT ? displayMode : 0], infoPage,
           clocking, inPrompt, menuOpen, settingsOpen, resetOpen);
  if (strcmp(cur, prev) == 0) return;
  Serial.printf("[%8lu] ST  %s\n", (unsigned long)millis(), cur);
  strncpy(prev, cur, sizeof(prev) - 1);
  prev[sizeof(prev) - 1] = 0;
}
#else
static inline void stateLog(bool, bool, bool) {}
#endif

void loop() {
  hwInputUpdate();
  ;
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  sessionDots::update(tama.sessionsRunning, tama.sessionsWaiting);
  sessionDots::tick();

  // "A session finished" — watches the raw running-count, not the derived
  // busy/celebrate state. With several sessions running, baseState stays
  // P_BUSY the whole time (derive() only checks sessionsRunning >= 1), so
  // this fires on the count decreasing at all, not just hitting zero —
  // otherwise one of three sessions returning would give no signal until
  // the last one finished.
  {
    static uint8_t prevSessionsRunning = 0;
    if (tama.sessionsRunning < prevSessionsRunning) {
      beep(400, 95);
      beep(480, 35);
      if (busyDimmed) wake();
    }
    prevSessionsRunning = tama.sessionsRunning;
  }

  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // Edge-triggered (not level) so this fires once per transition, not every
  // frame P_CELEBRATE/P_BUSY happens to hold. Attention already wakes via
  // the prompt-arrival hook below; this is a safety net for the case where
  // sessionsWaiting flips without a promptId change, plus the busy/celebrate
  // audio cues so state is readable without looking at the screen.
  {
    static PersonaState prevBaseState = P_SLEEP;
    if (baseState != prevBaseState) {
      if (baseState == P_BUSY)           beep(1000, 40);
      else if (baseState == P_CELEBRATE) beep(2000, 100);
      if ((baseState == P_ATTENTION || baseState == P_CELEBRATE) && busyDimmed) wake();
      prevBaseState = baseState;
    }
  }

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Attention indicator: AMOLED red border flash (replaces M5StickC LED).
  hwBorderAlert(activeState == P_ATTENTION && settings().led
                && (now / 400) % 2 == 0);

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // ── Buttons: PWR / +/KEY / BOOT- ──────────────────────────────────────
  //   PWR   tap  = screen on/off, or cursor-up while a list is open
  //         hold = power off (everywhere)
  //   +/KEY tap  = approve / cursor-down / toggle buddy stats
  //         hold = open menu, or step back one level inside one
  //   BOOT  tap  = deny / select item / cycle info pages
  //         hold = mute/unmute (everywhere)
  if (hwBtnA().wasPressed)    BTNLOG("PWR  down");
  if (hwBtnB().wasPressed)    BTNLOG("KEY  down");
  if (hwBtnBoot().wasPressed) BTNLOG("BOOT down");

  // PWR is excluded here: its own tap handler decides wake-vs-sleep from
  // screenOff directly, so it must not be pre-woken by this block.
  if (hwBtnB().isPressed || hwBtnBoot().isPressed) {
    if (screenOff) {
      if (hwBtnB().isPressed)    swallowBtnB    = true;
      if (hwBtnBoot().isPressed) swallowBtnBoot = true;
    }
    wakeForUser();
  }

  static bool pwrLong = false;
  if (hwBtnA().pressedFor(PWR_LONG_MS) && !pwrLong) {
    pwrLong = true;
    BTNLOG("PWR  hold -> POWER OFF");
    beep(800, 80);
    hwPowerOff();
  }
  if (hwBtnA().wasReleased) {
    uint32_t held = millis() - hwBtnA().pressedAt;
    if (!pwrLong) {
      bool up = pwrIsCursorUp();
      if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + (up ? RESET_N - 1 : 1)) % RESET_N;
        resetConfirmIdx = 0xFF;
        BTNLOG("PWR  tap held=%lu -> reset cursor %s (%u)", (unsigned long)held, up ? "up" : "down", resetSel);
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + (up ? SETTINGS_N - 1 : 1)) % SETTINGS_N;
        BTNLOG("PWR  tap held=%lu -> settings cursor %s (%u)", (unsigned long)held, up ? "up" : "down", settingsSel);
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + (up ? MENU_N - 1 : 1)) % MENU_N;
        BTNLOG("PWR  tap held=%lu -> menu cursor %s (%u)", (unsigned long)held, up ? "up" : "down", menuSel);
      } else if (screenOff || dimmed || busyDimmed) {
        wakeForUser();
        BTNLOG("PWR  tap held=%lu -> screen ON", (unsigned long)held);
      } else {
        hwDisplaySleep(true);
        screenOff = true;
        BTNLOG("PWR  tap held=%lu -> screen OFF", (unsigned long)held);
      }
    }
    pwrLong = false;
  }

  static bool keyLong = false;
  if (hwBtnB().pressedFor(LONG_MS) && !keyLong && !swallowBtnB) {
    keyLong = true;
    beep(800, 60);
    if (resetOpen) {
      resetOpen = false;
      BTNLOG("KEY  hold -> leave reset");
    } else if (settingsOpen) {
      settingsOpen = false;
      characterInvalidate();
      BTNLOG("KEY  hold -> leave settings");
    } else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
      BTNLOG("KEY  hold -> menu %s", menuOpen ? "OPEN" : "CLOSE");
    }
  }
  if (hwBtnB().wasReleased) {
    uint32_t held = millis() - hwBtnB().pressedAt;
    if (!keyLong && !swallowBtnB) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
        BTNLOG("KEY  tap held=%lu -> APPROVE (%lus)", (unsigned long)held, (unsigned long)tookS);
      } else if (resetOpen) {
        bool up = !pwrIsCursorUp();
        beep(1800, 30);
        resetSel = (resetSel + (up ? RESET_N - 1 : 1)) % RESET_N;
        resetConfirmIdx = 0xFF;
        BTNLOG("KEY  tap held=%lu -> reset cursor %s (%u)", (unsigned long)held, up ? "up" : "down", resetSel);
      } else if (settingsOpen) {
        bool up = !pwrIsCursorUp();
        beep(1800, 30);
        settingsSel = (settingsSel + (up ? SETTINGS_N - 1 : 1)) % SETTINGS_N;
        BTNLOG("KEY  tap held=%lu -> settings cursor %s (%u)", (unsigned long)held, up ? "up" : "down", settingsSel);
      } else if (menuOpen) {
        bool up = !pwrIsCursorUp();
        beep(1800, 30);
        menuSel = (menuSel + (up ? MENU_N - 1 : 1)) % MENU_N;
        BTNLOG("KEY  tap held=%lu -> menu cursor %s (%u)", (unsigned long)held, up ? "up" : "down", menuSel);
      } else {
        beep(1800, 30);
        displayMode = (displayMode == DISP_PET) ? DISP_NORMAL : DISP_PET;
        applyDisplayMode();
        BTNLOG("KEY  tap held=%lu -> mode %s", (unsigned long)held,
               displayMode == DISP_PET ? "PET" : "NORMAL");
      }
    } else {
      BTNLOG("KEY  tap held=%lu -> swallowed (long=%d wake=%d)",
             (unsigned long)held, keyLong, swallowBtnB);
    }
    keyLong = false;
    swallowBtnB = false;
  }

  static bool bootLong = false;
  if (hwBtnBoot().pressedFor(LONG_MS) && !bootLong && !swallowBtnBoot) {
    bootLong = true;
    applySetting(2);                     // toggles settings().sound
    if (settings().sound) beep(1800, 80);  // can only be heard un-muting
    BTNLOG("BOOT hold -> sound %s", settings().sound ? "ON" : "MUTED");
  }
  if (hwBtnBoot().wasReleased) {
    uint32_t held = millis() - hwBtnBoot().pressedAt;
    if (!bootLong && !swallowBtnBoot) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
        beep(600, 60);
        BTNLOG("BOOT tap held=%lu -> DENY", (unsigned long)held);
      } else if (resetOpen) {
        beep(2400, 30);
        BTNLOG("BOOT tap held=%lu -> reset select (%u)", (unsigned long)held, resetSel);
        applyReset(resetSel);
      } else if (settingsOpen) {
        beep(2400, 30);
        BTNLOG("BOOT tap held=%lu -> settings select (%u)", (unsigned long)held, settingsSel);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        BTNLOG("BOOT tap held=%lu -> menu select (%u)", (unsigned long)held, menuSel);
        menuConfirm();
      } else if (displayMode == DISP_INFO) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
        BTNLOG("BOOT tap held=%lu -> info page %u/%u", (unsigned long)held,
               infoPage + 1, INFO_PAGES);
      } else {
        beep(2400, 30);
        displayMode = DISP_INFO;
        applyDisplayMode();
        BTNLOG("BOOT tap held=%lu -> mode INFO (page %u)", (unsigned long)held, infoPage + 1);
      }
    } else {
      BTNLOG("BOOT tap held=%lu -> swallowed (long=%d wake=%d)",
             (unsigned long)held, bootLong, swallowBtnBoot);
    }
    bootLong = false;
    swallowBtnBoot = false;
  }

  // ─── Touch (additive — buttons above already handled everything else) ──
  // Touch does exactly one thing: any stationary tap → heart reaction.
  // Buttons already cover approve/deny, menu nav, page cycling, transcript
  // scroll, and species change, so touch no longer needs to be position- or
  // rotation-aware — dropping it was the deliberate trade to make runtime
  // screen rotation (Settings.rotation) simple: no coordinate remap needed.
  const HwTouch& tp = hwTouch();
  if (tp.justPressed) { _tpStartX = tp.x; _tpStartY = tp.y; _tpStartMs = millis(); }

  if (tp.justReleased
      && !inPrompt && !menuOpen && !settingsOpen && !resetOpen
      && !napping && !screenOff && !busyDimmed) {
    int dx = (int)tp.x - _tpStartX;
    int dy = (int)tp.y - _tpStartY;
    uint32_t dt = millis() - _tpStartMs;
    if (abs(dx) < 12 && abs(dy) < 12 && dt < 800) {
      triggerOneShot(P_HEART, 2000);
      _playfulUntil = millis() + PLAYFUL_MS;
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      beep(2400, 50);
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // Clock shows when Claude is idle and the RTC is synced — regardless
  // of USB power. On battery the screen still auto-offs after a longer
  // timeout (CLOCK_OFF_MS_BAT) so it doesn't drain forever.
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid();
  // Portrait-only clock on AMOLED port; landscape was removed.
  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) {
      // GIFs are tall (up to 140 px) — must shrink to fit above clock.
      // ASCII buddy at scale 2 reaches y≈126; clock starts at y=140
      // (compact single-line layout) so peek isn't needed and the pet
      // gets to keep its full size.
      characterSetPeek(true);
      buddySetPeek(false);
      // Clear the full canvas once on entry: buddy/clock both update
      // partial regions every frame, so any stale ink left behind from
      // the previous mode would persist forever.
      const Palette& cp = characterPalette();
      spr.fillScreen(cp.bg);
    } else {
      applyDisplayMode();
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  // Skip the time-of-day mood logic while a one-shot animation
  // (shake → dizzy, level-up → celebrate, fast-approve → heart) is
  // active — otherwise it would overwrite activeState immediately.
  if (clocking && (int32_t)(now - oneShotUntil) >= 0) {
    if ((int32_t)(now - _playfulUntil) < 0) {
      // Recently interacted with (pet tap / species swipe) — rotate through
      // awake animations instead of falling back to the time-of-day logic
      // that mostly picks P_SLEEP. Decays to normal after PLAYFUL_MS.
      static const PersonaState PLAYFUL[] = {
        P_IDLE, P_IDLE, P_HEART, P_IDLE, P_CELEBRATE, P_IDLE
      };
      activeState = PLAYFUL[(now / 5000) % 6];
    } else {
      // Ambient rhythm is SLEEP↔IDLE only. Emotional states (HEART, CELEBRATE,
      // DIZZY) are reactions — they fire from real events (shake, fast-approve,
      // level-up, pet tap, species swipe) via triggerOneShot / playful window,
      // never spontaneously from wall-clock mood.
      uint8_t h = _clkTm.H;
      if (h < 7 || h >= 22) activeState = (now/15000 % 8 == 0) ? P_IDLE  : P_SLEEP;
      else                  activeState = (now/12000 % 6 == 0) ? P_SLEEP : P_IDLE;
    }
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Dots-only replaces the usual animation for face-down nap, battery
  // busy-dim, and the ambient nighttime P_SLEEP state — "only the dots
  // show" while asleep. screenOff is the one exception: the panel is
  // truly powered off there (hwDisplaySleep(true)), so nothing can show
  // regardless of what's pushed to the framebuffer.
  bool dotsOnly = napping || busyDimmed || activeState == P_SLEEP;

  if (screenOff) {
    // skip canvas render — panel truly powered off
  } else if (dotsOnly) {
    spr.fillScreen(characterPalette().bg);
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(SAFE_L, 90);
      spr.print("installing");
      spr.setCursor(SAFE_L, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(SAFE_L, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(SAFE_L + 1, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(SAFE_L, 100);
      spr.print("no character loaded");
    }
  }
  if (!screenOff) {
    if (dotsOnly) {
      sessionDots::draw();
    } else {
      if (blePasskey()) drawPasskey();
      else if (clocking) drawClock();
      else if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      else if (settings().hud) drawHUD();
      if (resetOpen) drawReset();
      else if (settingsOpen) drawSettings();
      else if (menuOpen) drawMenu();
      if (resetOpen || settingsOpen || menuOpen) drawButtonHints();
      // Dots on every screen. They sit at the very top-right (y = SAFE_T+2),
      // which clears Info's content region (starts at y=70) and both the menu
      // and reset panels. Only the 13-row settings panel reaches that high,
      // and a 2px dot over its top border beats losing session state.
      sessionDots::draw();
    }
    hwDisplayPush();
  }

  stateLog(dotsOnly, clocking, inPrompt);

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    hwDisplayBrightness(0);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // Auto-off rules:
  //   USB plugged: never (clock can stay visible indefinitely)
  //   Battery + clock visible: 5 min (CLOCK_OFF_MS_BAT)
  //   Battery + non-clock idle: 30 s (SCREEN_OFF_MS)
  //   Battery + busy (Claude still working): dim instead of sleep — last
  //   frame stays frozen on-screen rather than going fully blank.
  if (!screenOff && !busyDimmed && !inPrompt && !_onUsb) {
    uint32_t idleMs    = millis() - lastInteractMs;
    uint32_t threshold = clocking ? CLOCK_OFF_MS_BAT : SCREEN_OFF_MS;
    if (idleMs > threshold) {
      if (baseState == P_BUSY) {
        hwDisplayBrightness(0);
        busyDimmed = true;
      } else {
        hwDisplaySleep(true);
        screenOff = true;
      }
    }
  }

  // AMOLED burn-in mitigation: every 5 min force a full canvas redraw.
  // OLED pixels degrade where they stay lit at constant value; redrawing
  // (rather than incremental updates) at least exercises every pixel for
  // a frame. A more aggressive 1-px shimmy could shift the whole canvas
  // each cycle, but this minimum is a safe baseline.
  static uint32_t lastShimmy = 0;
  if (millis() - lastShimmy > 5UL * 60UL * 1000UL) {
    lastShimmy = millis();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }

  // LTPO-lite: vary loop cadence by what's happening. Animations tick on
  // wall-clock (buddy.cpp TICK_MS=200) and redraws are gated, so slowing the
  // loop during ambient SLEEP↔IDLE costs no frames — just fewer MCU wakes.
  // Fast rate only where latency is felt: input, interactive UI, one-shots,
  // nap-exit, transfer progress, BLE pairing.
  uint32_t loopMs;
  if (screenOff) {
    loopMs = 200;
  } else if (napping
          || hwTouch().down
          || hwBtnA().isPressed || hwBtnB().isPressed
          || hwBtnBoot().isPressed
          || inPrompt || menuOpen || settingsOpen || resetOpen
          || (int32_t)(now - oneShotUntil) < 0
          || xferActive()
          || blePasskey()) {
    loopMs = 16;
  } else {
    loopMs = 100;
  }
  // Slice the idle sleep so a touch-down IRQ (edge-triggered) or a button
  // press breaks out within ~8ms instead of waiting the full loopMs. Without
  // this, first-tap latency during idle felt sluggish.
  if (loopMs <= 16) {
    delay(loopMs);
  } else {
    uint32_t slept = 0;
    while (slept < loopMs) {
      uint32_t slice = (loopMs - slept > 8) ? 8 : (loopMs - slept);
      delay(slice);
      slept += slice;
      if (hwTouchIrqPending()) break;
      if (digitalRead(PIN_KEY1) == LOW) break;
    }
  }
}
