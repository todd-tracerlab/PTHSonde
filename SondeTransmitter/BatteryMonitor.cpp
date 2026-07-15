#include "BatteryMonitor.h"
#include "BoardPins.h"
#include <Adafruit_INA219.h>   // Library Manager: "Adafruit INA219"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// Same init robustness as Sensors.cpp: settle + boot retry + in-loop self-heal,
// plus wedge detection. A cold-boot I2C NACK used to strand the INA219 dead for
// the whole flight, and a mid-flight wedge returns 0 V / 0 mA (not NaN), which
// would ship as valid zeros -- both are guarded below.
#define BATT_BEGIN_TRIES 5
#define BATT_BEGIN_GAP   25      // ms between boot attempts
#define BATT_RETRY_MS    1000    // re-begin cadence while down
#define BATT_FAIL_LIMIT  8       // consecutive bad reads -> force a re-init

static Adafruit_INA219 s_ina(I2C_ADDR_INA219);
static bool     s_ready    = false;
static uint32_t s_retryMs  = 0;
static uint8_t  s_fails    = 0;

static bool beginINA() {
  if (!s_ina.begin(&Wire)) return false;
  s_ina.setCalibration_32V_2A();   // make the default 32V/2A cal explicit
  return true;
}

bool batteryBegin() {
  for (int i = 0; i < BATT_BEGIN_TRIES && !s_ready; i++) {
    s_ready = beginINA();
    if (!s_ready) delay(BATT_BEGIN_GAP);
  }
  return s_ready;
}

BatteryReading batteryRead() {
  BatteryReading r = {0.0f, 0.0f, 0.0f, false};

  // self-heal: re-probe if the boot init failed or the sensor later wedged
  if (!s_ready && (uint32_t)(millis() - s_retryMs) >= BATT_RETRY_MS) {
    s_retryMs = millis(); s_ready = beginINA(); if (s_ready) s_fails = 0;
  }
  if (!s_ready) return r;

  float bus_v    = s_ina.getBusVoltage_V();      // load-side voltage
  float shunt_mv = s_ina.getShuntVoltage_mV();   // across the shunt
  float curr_ma  = s_ina.getCurrent_mA();

  // A wedged INA219 / dead I2C returns 0 (not NaN). A live battery is never
  // exactly 0.000 V, so treat that -- and any NaN -- as a failed read.
  if (isnan(bus_v) || isnan(curr_ma) || bus_v <= 0.05f) {
    if (++s_fails >= BATT_FAIL_LIMIT) { s_ready = false; s_fails = 0; }
    return r;
  }
  s_fails = 0;

  // Supply (battery) voltage = bus + shunt drop.
  r.voltage_v  = bus_v + (shunt_mv / 1000.0f);
  r.current_ma = curr_ma;
  r.power_mw   = r.voltage_v * curr_ma;          // V * mA = mW
  r.ok = true;
  return r;
}
