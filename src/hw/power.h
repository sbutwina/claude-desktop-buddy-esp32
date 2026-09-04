#pragma once
#include <stdint.h>

struct HwBattery {
  int   mV;          // battery voltage, millivolts
  int   mA;          // + discharging, − charging
  int   pct;         // 0..100, derived linearly from mV
  bool  usbPresent;  // VBUS > 4V
  bool  charging;
  int   tempC;
};

bool hwPowerInit();
HwBattery hwBattery();
void hwPowerOff();
// AXP2101 power-key IRQ helpers. Each call reads + clears the IRQ
// flag, returning true at most once per physical press. Use these
// instead of raw getIrqStatus() — the AXP register bit positions
// don't match the XPOWERS_PWR_BTN_* enum values.
bool hwAxpPekeyShortPress();
bool hwAxpPekeyLongPress();
// Single-pass version of the two above: reads the PWRON latches once and
// clears them together. Prefer this when you care about both — calling
// ShortPress() then LongPress() can drop the long bit, because each one
// calls clearIrqStatus(), which clears *every* latched IRQ.
// Returns bit0 = short press, bit1 = long press.
uint8_t hwAxpPekeyPoll();

#include <XPowersLib.h>
XPowersPMU* hwPmuRef();   // raw access for boards that need direct register / rail control
