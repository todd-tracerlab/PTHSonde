#include "BatteryMonitor.h"
#include "BoardPins.h"
#include <Adafruit_INA219.h>   // Library Manager: "Adafruit INA219"
#include <Wire.h>
#include <math.h>

static Adafruit_INA219 s_ina(I2C_ADDR_INA219);
static bool s_ready = false;

bool batteryBegin() {
  // begin() returns true on success in current Adafruit_INA219 versions.
  s_ready = s_ina.begin(&Wire);
  if (s_ready) {
    // Default 32V / 2A calibration is applied by begin(); make it explicit.
    s_ina.setCalibration_32V_2A();
  }
  return s_ready;
}

BatteryReading batteryRead() {
  BatteryReading r = {0.0f, 0.0f, 0.0f, false};
  if (!s_ready) return r;

  float bus_v   = s_ina.getBusVoltage_V();      // load-side voltage
  float shunt_mv = s_ina.getShuntVoltage_mV();  // across the shunt
  float curr_ma = s_ina.getCurrent_mA();

  if (isnan(bus_v) || isnan(curr_ma)) return r;

  // Supply (battery) voltage = bus + shunt drop.
  r.voltage_v  = bus_v + (shunt_mv / 1000.0f);
  r.current_ma = curr_ma;
  r.power_mw   = r.voltage_v * curr_ma;         // V * mA = mW
  r.ok = true;
  return r;
}
