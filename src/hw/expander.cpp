// src/hw/expander.cpp
#include "hw/expander.h"
#include "hw/pins.h"
#include <Arduino.h>

bool hwExpanderInit() {
  pinMode(PIN_LCD_RESET, OUTPUT);
  pinMode(PIN_TP_RESET, OUTPUT);
  return true;
}

void hwExpanderResetSequence() {
  digitalWrite(PIN_LCD_RESET, LOW);
  digitalWrite(PIN_TP_RESET, LOW);
  delay(20);
  digitalWrite(PIN_LCD_RESET, HIGH);
  digitalWrite(PIN_TP_RESET, HIGH);
  delay(20);
}
