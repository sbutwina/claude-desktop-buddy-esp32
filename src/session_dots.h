#pragma once
#include <Arduino_GFX_Library.h>
#include "hw/display.h"

// Top-right "active sessions" dots. The BLE wire protocol only ever sends
// session *counts* (REFERENCE.md), never a per-session id, so a dot slot
// is just a position in a row — not a specific tracked session. Slots ease
// toward their target x each tick so a freed (grey, expired) slot's
// neighbours slide over instead of jumping.
namespace sessionDots {

constexpr int      MAX_DOTS = 6;
constexpr int      DOT_R    = 2;      // radius, px
constexpr int      DOT_GAP  = 7;      // centre-to-centre spacing, px
constexpr uint32_t GREY_MS  = 20000;  // how long a freed slot lingers grey
constexpr float    EASE     = 0.3f;   // slide speed per tick, 0..1

constexpr uint16_t GREEN_C = 0x07E0;
constexpr uint16_t AMBER_C = 0xFDA0;
constexpr uint16_t GREY_C  = 0x6B6D;

enum SlotState : uint8_t { EMPTY, RUNNING, WAITING, EXPIRING };

struct Slot {
  SlotState state     = EMPTY;
  uint32_t  greySince  = 0;
  float     x          = 0;
  bool      xInit      = false;
};

static Slot _slots[MAX_DOTS];

// Pull occupied slots to the front, preserving order — index == position
// in the row (0 = nearest the corner). Frees the rest.
static void _compact() {
  int w = 0;
  for (int r = 0; r < MAX_DOTS; r++) {
    if (_slots[r].state != EMPTY) {
      if (w != r) _slots[w] = _slots[r];
      w++;
    }
  }
  for (int i = w; i < MAX_DOTS; i++) _slots[i] = Slot{};
}

// Idempotent — safe to call every loop() iteration with the latest counts.
inline void update(uint8_t running, uint8_t waiting) {
  uint32_t now = millis();

  bool expired = false;
  for (auto& s : _slots) {
    if (s.state == EXPIRING && now - s.greySince > GREY_MS) { s.state = EMPTY; expired = true; }
  }
  if (expired) _compact();

  int desired = running + waiting;
  if (desired > MAX_DOTS) desired = MAX_DOTS;

  // Grey (EXPIRING) slots don't count as "colored" — update() runs every
  // loop iteration, not just on a count change, so this must be idempotent:
  // once the right number of slots are grey, repeat calls with the same
  // counts must not grey any more of them.
  int colored = 0;
  for (auto& s : _slots) if (s.state == RUNNING || s.state == WAITING) colored++;

  if (desired > colored) {
    int need = desired - colored;
    // Reclaim grey slots first — a session count rising back into one
    // before it expires reads as "that one resumed".
    for (auto& s : _slots) {
      if (need == 0) break;
      if (s.state == EXPIRING) { s.state = RUNNING; need--; }
    }
    for (auto& s : _slots) {
      if (need == 0) break;
      if (s.state == EMPTY) { s.state = RUNNING; s.xInit = false; need--; }
    }
  } else if (desired < colored) {
    // Grey the front-most (nearest-corner, oldest-by-allocation) colored
    // slots. Greying the *trailing* ones instead would never produce a
    // gap with anything behind it to slide — nothing would ever move.
    int excess = colored - desired;
    for (int i = 0; i < MAX_DOTS && excess > 0; i++) {
      if (_slots[i].state == RUNNING || _slots[i].state == WAITING) {
        _slots[i].state     = EXPIRING;
        _slots[i].greySince = now;
        excess--;
      }
    }
  }

  // Recolor: first `running` occupied non-grey slots run green, the rest amber.
  int g = running;
  for (auto& s : _slots) {
    if (s.state == RUNNING || s.state == WAITING) {
      s.state = (g > 0) ? RUNNING : WAITING;
      if (g > 0) g--;
    }
  }
}

// Call every render tick regardless of display mode, so positions keep
// easing even while dots-only or off-screen.
inline void tick() {
  for (int i = 0; i < MAX_DOTS; i++) {
    Slot& s = _slots[i];
    if (s.state == EMPTY) continue;
    float target = SAFE_R - DOT_R - i * DOT_GAP;
    if (!s.xInit) { s.x = target; s.xInit = true; }
    else          { s.x += (target - s.x) * EASE; }
  }
}

inline void draw() {
  Arduino_Canvas* canvas = hwCanvas();
  for (auto& s : _slots) {
    if (s.state == EMPTY) continue;
    uint16_t c = s.state == RUNNING ? GREEN_C : s.state == WAITING ? AMBER_C : GREY_C;
    canvas->fillCircle((int)(s.x + 0.5f), SAFE_T + DOT_R, DOT_R, c);
  }
}

} // namespace sessionDots
