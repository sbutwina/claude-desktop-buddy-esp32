// src/hw/display.h
#pragma once
#include <Arduino_GFX_Library.h>
#include "hw/pins.h"   // provides BOARD_HW_W, BOARD_HW_H, BOARD_SAFE_INSET

constexpr int HW_W       = BOARD_HW_W;
constexpr int HW_H       = BOARD_HW_H;

// Logical-canvas safe-draw region for the rounded/circular AMOLED bezel.
constexpr int SAFE_INSET = BOARD_SAFE_INSET;
constexpr int SAFE_L     = SAFE_INSET;
constexpr int SAFE_T     = SAFE_INSET;
constexpr int SAFE_R     = HW_W - SAFE_INSET;
constexpr int SAFE_B     = HW_H - SAFE_INSET;
constexpr int SAFE_W     = HW_W - 2 * SAFE_INSET;
constexpr int SAFE_H     = HW_H - 2 * SAFE_INSET;

bool hwDisplayInit();
void hwDisplayPush();
void hwDisplayBrightness(uint8_t lvl_0_4);
void hwDisplaySleep(bool off);
// Runtime panel rotation: 0=normal 1=90CW 2=180 3=90CCW. MADCTL bytes match
// what BOARD_CO5300_MADCTL already used at compile time; no-op on SH8601
// boards (rotation not wired for that panel driver yet).
void hwDisplaySetRotation(uint8_t idx);
Arduino_Canvas* hwCanvas();

// Fresh-install default so first boot matches whatever BOARD_CO5300_MADCTL
// used to hardcode at compile time — avoids flipping the screen orientation
// out from under an existing device on first update.
#if BOARD_CO5300_MADCTL == 0x60
  #define BOARD_ROTATION_DEFAULT 1
#elif BOARD_CO5300_MADCTL == 0xC0
  #define BOARD_ROTATION_DEFAULT 2
#elif BOARD_CO5300_MADCTL == 0xA0
  #define BOARD_ROTATION_DEFAULT 3
#else
  #define BOARD_ROTATION_DEFAULT 0
#endif
