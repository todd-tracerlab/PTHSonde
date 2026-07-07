// =============================================================================
// Sensors.h  -- SHT41 (0x44), MS5611 (0x77), MCP3221 (0x4D) + derived values
//
// Owns the I2C bus init. Reads each sensor independently; one sensor failing
// never blocks the others. The thermistor temperature is derived from the
// MCP3221 voltage. (Wind direction is NOT a sensor here -- it is derived from
// GPS motion in the main sketch; see WindDirection.h.)
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  // SHT41
  float    sht_temp_c;
  float    sht_rh;
  bool     sht_ok;
  // MS5611
  float    ms_press_pa;
  float    ms_temp_c;
  bool     ms_ok;
  // MCP3221 ADC
  uint16_t mcp_raw;
  float    mcp_voltage;
  bool     mcp_ok;
  // derived thermistor temperature (from MCP3221)
  float    therm_temp_c;
  bool     therm_ok;
} SensorData;

void sensorsBegin();             // also calls Wire.begin()
void sensorsUpdate(SensorData* d);
