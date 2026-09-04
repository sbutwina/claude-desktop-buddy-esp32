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

HwBtn& hwBtnA();          // PWR key, middle (PIN_KEY1)
HwBtn& hwBtnB();          // +/KEY key, left (PIN_KEY2)
HwBtn& hwBtnBoot();       // BOOT/- key, right (PIN_KEY_BOOT)

const HwTouch& hwTouch();
bool hwTouchIrqPending();  // peek at IRQ flag without consuming; for break-early sleep
