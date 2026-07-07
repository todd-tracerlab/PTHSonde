#include "Sensors.h"
#include "BoardPins.h"
#include "Thermistor.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>   // Library Manager: "Adafruit SHT4x"
#include <MS5611.h>           // Library Manager: "MS5611" (Rob Tillaart)
#include <math.h>

// ---- MCP3221 ----------------------------------------------------------------
// 12-bit ADC, ratiometric to its supply. Output is 12 bits MSB-first across two
// bytes: byte0 = D11..D4, byte1 = D3..D0 in the upper nibble.
#define MCP3221_FULLSCALE 4096.0f
#define MCP3221_VREF      3.30f   // MCP3221 supply (defines volts-per-code)

static Adafruit_SHT4x s_sht;
static MS5611         s_ms(I2C_ADDR_MS5611);
static bool s_sht_ready = false;
static bool s_ms_ready  = false;

// Direct, dependency-free MCP3221 read (no public Arduino lib needed).
// The MCP3221 is RIGHT-justified: it returns 2 bytes where the high byte's
// upper 4 bits are padding -> value = ((MSB & 0x0F) << 8) | LSB  (0..4095).
static bool mcpRead(uint16_t* raw) {
  uint8_t n = Wire.requestFrom((uint8_t)I2C_ADDR_MCP3221, (uint8_t)2);
  if (n < 2) return false;
  uint8_t b0 = Wire.read();   // high byte (upper nibble = 0)
  uint8_t b1 = Wire.read();   // low byte
  *raw = (((uint16_t)b0 & 0x0F) << 8) | b1;   // 0..4095
  return true;
}

void sensorsBegin() {
  // setPins() first so that if a sensor library calls a no-arg Wire.begin()
  // internally (Adafruit drivers do), it still uses GPIO4/GPIO5 and not the
  // variant default pins.
  Wire.setPins(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  // SHT41
  s_sht_ready = s_sht.begin(&Wire);
  if (s_sht_ready) {
    s_sht.setPrecision(SHT4X_HIGH_PRECISION);
    s_sht.setHeater(SHT4X_NO_HEATER);
  }

  // MS5611 (keep oversampling modest so read() never blocks long)
  s_ms_ready = s_ms.begin();
  if (s_ms_ready) {
    s_ms.setOversampling(OSR_STANDARD);
  }
}

void sensorsUpdate(SensorData* d) {
  // ---- SHT41 ---------------------------------------------------------------
  d->sht_ok = false;
  d->sht_temp_c = 0.0f;
  d->sht_rh = 0.0f;
  if (s_sht_ready) {
    sensors_event_t hum, temp;
    if (s_sht.getEvent(&hum, &temp)) {
      if (!isnan(temp.temperature) && !isnan(hum.relative_humidity)) {
        d->sht_temp_c = temp.temperature;
        d->sht_rh     = hum.relative_humidity;
        d->sht_ok     = true;
      }
    }
  }

  // ---- MS5611 --------------------------------------------------------------
  d->ms_ok = false;
  d->ms_press_pa = 0.0f;
  d->ms_temp_c = 0.0f;
  if (s_ms_ready) {
    int rc = s_ms.read();                 // performs the conversion
    if (rc == MS5611_READ_OK) {
      d->ms_press_pa = s_ms.getPressure() * 100.0f;  // hPa -> Pa
      d->ms_temp_c   = s_ms.getTemperature();
      d->ms_ok = true;
    }
  }

  // ---- MCP3221 + thermistor ------------------------------------------------
  d->mcp_ok = false;
  d->mcp_raw = 0;
  d->mcp_voltage = 0.0f;
  d->therm_ok = false;
  d->therm_temp_c = 0.0f;
  uint16_t raw;
  if (mcpRead(&raw)) {
    d->mcp_raw = raw;
    d->mcp_voltage = (raw / MCP3221_FULLSCALE) * MCP3221_VREF;
    d->mcp_ok = true;

    bool tok = false;
    float tc = thermistorTempC(d->mcp_voltage, &tok);
    d->therm_temp_c = tok ? tc : 0.0f;
    d->therm_ok = tok;
  }
}
