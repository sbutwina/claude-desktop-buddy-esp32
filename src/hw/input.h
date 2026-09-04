#pragma once
#include <stdint.h>

struct HwBtn {
  bool isPressed;
  bool wasPressed;
  bool wasReleased;
  uint32_t pressedAt;
  bool pressedFor(uint32_t ms);
};

struct HwTouch {
  bool down;
  int16_t x, y;
  bool justPressed;
  bool justReleased;
};

bool hwInputInit();
void hwInputUpdate();

HwBtn& hwBtnA();          // Key1 — PWR on 3-key boards, BOOT/GPIO0 on 2-key boards
HwBtn& hwBtnB();          // Key2 (+/KEY) on 3-key boards, AXP short-press on 2-key boards
HwBtn& hwBtnBoot();       // BOOT/- key — 3-key boards only (BOARD_BTN_THIRD)
uint8_t hwAxpBtnEvent();  // 0 / 0x02 / 0x04 — caller consumes 0x04 (2-key boards only)

const HwTouch& hwTouch();
bool hwTouchIrqPending();  // peek at IRQ flag without consuming; for break-early sleep
