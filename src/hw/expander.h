// src/hw/expander.h
#pragma once
#include "hw/pins.h"

bool hwExpanderInit();          // never fails: LCD_RESET / TP_RESET are direct GPIOs
void hwExpanderResetSequence(); // pull LCD_RESET + TP_RESET low → 20ms → high
