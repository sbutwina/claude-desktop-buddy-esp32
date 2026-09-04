#include "hw/input.h"
#include "hw/pins.h"
#include "hw/expander.h"
#include "hw/power.h"
#include <Arduino.h>
#include <Wire.h>
#include "TouchDrvCSTXXX.hpp"

static HwBtn   s_a, s_b;
static HwTouch s_tp;

static TouchDrvCST92xx s_cst;
static volatile bool   s_tpIrqFlag = false;

static void IRAM_ATTR onTouchIrq() { s_tpIrqFlag = true; }

bool HwBtn::pressedFor(uint32_t ms) {
  return isPressed && (millis() - pressedAt) >= ms;
}

bool hwInputInit() {
  pinMode(PIN_KEY1, INPUT_PULLUP);   // GPIO0 has external pullup; INPUT_PULLUP is harmless
  pinMode(PIN_KEY2, INPUT_PULLUP);   // External R18 10K already pulls high; INPUT_PULLUP is harmless
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);   // External R8 10K already pulls high; INPUT_PULLUP is harmless

  // CST92xx @ 0x5A via SensorLib. Reset is handled by hwExpanderResetSequence(),
  // so pass rstPin=-1 to skip the driver's internal reset.
  s_cst.setPins(-1, PIN_TP_INT);
  if (!s_cst.begin(Wire, 0x5A, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwInput: CST92xx init failed");
    return false;
  }
  Serial.printf("hwInput: CST92xx model=%s\n", s_cst.getModelName());
  s_cst.setMaxCoordinates(LCD_W_PHYS, LCD_H_PHYS);
  s_cst.setMirrorXY(true, true);
  pinMode(PIN_TP_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchIrq, FALLING);
  return true;
}

static void scanKey1() {
  uint32_t now = millis();
  // Active-HIGH via the BSS138 inverter on the PWRON gate.
  bool pressed = digitalRead(PIN_KEY1) == HIGH;
  s_a.wasPressed  = pressed && !s_a.isPressed;
  s_a.wasReleased = !pressed && s_a.isPressed;
  if (s_a.wasPressed) s_a.pressedAt = now;
  s_a.isPressed = pressed;
}

static void scanKey2() {
  uint32_t now = millis();
  bool pressed = digitalRead(PIN_KEY2) == LOW;
  s_b.wasPressed  = pressed && !s_b.isPressed;
  s_b.wasReleased = !pressed && s_b.isPressed;
  if (s_b.wasPressed) s_b.pressedAt = now;
  s_b.isPressed = pressed;
}

// BOOT/- is a full button in its own right, scanned exactly like Key1/Key2.
// It used to forge a press/release into s_a (i.e. into hwBtnA/PWR) so the
// menu-open handler would fire — which meant a BOOT tap was indistinguishable
// from a PWR event downstream. It gets its own state now.
static HwBtn s_boot;
static void scanBootKey() {
  uint32_t now = millis();
  bool pressed = digitalRead(PIN_KEY_BOOT) == LOW;
  s_boot.wasPressed  = pressed && !s_boot.isPressed;
  s_boot.wasReleased = !pressed && s_boot.isPressed;
  if (s_boot.wasPressed) s_boot.pressedAt = now;
  s_boot.isPressed = pressed;
}

static void scanTouch() {
  // Poll when IRQ fires OR when a finger was down last frame — the CST92xx
  // only reliably IRQs on state edges, so a drag wouldn't advance x/y
  // without this.
  bool shouldPoll = s_tpIrqFlag || s_tp.down;
  s_tpIrqFlag = false;

  if (!shouldPoll) {
    s_tp.justPressed  = false;
    s_tp.justReleased = false;
    return;
  }

  int16_t x[2] = {0}, y[2] = {0};
  uint8_t n = s_cst.getPoint(x, y, s_cst.getSupportTouchPoint());
  if (n > 0) {
    s_tp.justPressed  = !s_tp.down;
    s_tp.justReleased = false;
    // Mirror of hwDisplayPush's letterbox scale: reverse (physical → canvas).
    // Use BOARD_* (raw macros from board header) since display.h's constexpr
    // wrappers aren't visible here.
    constexpr int OFF_X = (LCD_W_PHYS - BOARD_DISPLAY_DEST_W) / 2;
    constexpr int OFF_Y = (LCD_H_PHYS - BOARD_DISPLAY_DEST_H) / 2;
    int dx = x[0] - OFF_X;
    int dy = y[0] - OFF_Y;
    int tx = (dx * BOARD_HW_W) / BOARD_DISPLAY_DEST_W;
    int ty = (dy * BOARD_HW_H) / BOARD_DISPLAY_DEST_H;
    if (tx < 0) tx = 0; else if (tx >= BOARD_HW_W) tx = BOARD_HW_W - 1;
    if (ty < 0) ty = 0; else if (ty >= BOARD_HW_H) ty = BOARD_HW_H - 1;
    s_tp.x = tx;
    s_tp.y = ty;
    s_tp.down = true;
  } else {
    s_tp.justReleased = s_tp.down;
    s_tp.down = false;
    s_tp.justPressed  = false;
  }
}

void hwInputUpdate() {
  scanKey1();
  scanKey2();
  scanBootKey();
  scanTouch();
}

HwBtn& hwBtnA() { return s_a; }
HwBtn& hwBtnB() { return s_b; }
HwBtn& hwBtnBoot() { return s_boot; }

const HwTouch& hwTouch() { return s_tp; }
bool hwTouchIrqPending() { return s_tpIrqFlag; }
