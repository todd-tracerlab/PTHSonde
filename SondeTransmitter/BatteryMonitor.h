// =============================================================================
// BatteryMonitor.h  -- INA219 (0x40) voltage / current / power
// =============================================================================
#pragma once
#include <stdbool.h>

typedef struct {
  float voltage_v;   // supply voltage (bus + shunt), volts
  float current_ma;  // current, milliamps (signed)
  float power_mw;    // computed power, milliwatts
  bool  ok;
} BatteryReading;

// Wire.begin() must already have been called.
bool batteryBegin();
BatteryReading batteryRead();
