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

// ---- Init robustness --------------------------------------------------------
// The I2C sensors can NACK their power-on reset if probed too early (before the
// rail/sensor has settled). A single begin() with no delay and no retry left the
// device dead for the whole session -> pressure/SHT stuck red until a power cycle.
// Fix: settle before probing, retry begin() at boot, and SELF-HEAL in the update
// loop -- any sensor that is not up (or that wedges mid-flight) is re-begun on its
// own, so it comes green within a second without a power cycle.
#define SENSOR_SETTLE_MS   60     // wait after Wire.begin() before the first probe
#define SENSOR_BEGIN_TRIES 5      // begin() attempts at boot
#define SENSOR_BEGIN_GAP   25     // ms between boot attempts
#define SENSOR_RETRY_MS    1000   // re-begin cadence for a sensor that is still down
#define SENSOR_FAIL_LIMIT  8      // consecutive read failures -> force a re-init

static Adafruit_SHT4x s_sht;
static MS5611         s_ms(I2C_ADDR_MS5611);
static bool     s_sht_ready = false;
static bool     s_ms_ready  = false;
static uint32_t s_shtRetryMs = 0, s_msRetryMs = 0;   // last re-begin attempt
static uint8_t  s_shtFails   = 0, s_msFails   = 0;    // consecutive read failures

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

// (Re)initialise one sensor. Returns true on success and applies its config.
static bool beginSHT() {
  if (!s_sht.begin(&Wire)) return false;
  s_sht.setPrecision(SHT4X_HIGH_PRECISION);
  s_sht.setHeater(SHT4X_NO_HEATER);
  return true;
}
static bool beginMS() {
  if (!s_ms.begin()) return false;
  s_ms.setOversampling(OSR_STANDARD);   // keep read() fast (non-blocking-ish)
  return true;
}

void sensorsBegin() {
  // setPins() first so that if a sensor library calls a no-arg Wire.begin()
  // internally (Adafruit drivers do), it still uses GPIO4/GPIO5 and not the
  // variant default pins.
  Wire.setPins(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  delay(SENSOR_SETTLE_MS);   // let the sensors finish their power-on reset

  // Retry each device a few times -- a cold-boot NACK on the first probe is common.
  for (int i = 0; i < SENSOR_BEGIN_TRIES && !s_sht_ready; i++) {
    s_sht_ready = beginSHT();
    if (!s_sht_ready) delay(SENSOR_BEGIN_GAP);
  }
  for (int i = 0; i < SENSOR_BEGIN_TRIES && !s_ms_ready; i++) {
    s_ms_ready = beginMS();
    if (!s_ms_ready) delay(SENSOR_BEGIN_GAP);
  }
}

void sensorsUpdate(SensorData* d) {
  const uint32_t now = millis();

  // ---- self-heal: re-probe anything not up yet, at most once a second --------
  if (!s_sht_ready && (uint32_t)(now - s_shtRetryMs) >= SENSOR_RETRY_MS) {
    s_shtRetryMs = now; s_sht_ready = beginSHT(); if (s_sht_ready) s_shtFails = 0;
  }
  if (!s_ms_ready && (uint32_t)(now - s_msRetryMs) >= SENSOR_RETRY_MS) {
    s_msRetryMs = now; s_ms_ready = beginMS(); if (s_ms_ready) s_msFails = 0;
  }

  // ---- SHT41 ---------------------------------------------------------------
  d->sht_ok = false;
  d->sht_temp_c = 0.0f;
  d->sht_rh = 0.0f;
  if (s_sht_ready) {
    sensors_event_t hum, temp;
    if (s_sht.getEvent(&hum, &temp) &&
        !isnan(temp.temperature) && !isnan(hum.relative_humidity)) {
      d->sht_temp_c = temp.temperature;
      d->sht_rh     = hum.relative_humidity;
      d->sht_ok     = true;
      s_shtFails    = 0;
    } else if (++s_shtFails >= SENSOR_FAIL_LIMIT) {
      s_sht_ready = false; s_shtFails = 0;   // wedged -> let self-heal re-init
    }
  }

  // ---- MS5611 --------------------------------------------------------------
  d->ms_ok = false;
  d->ms_press_pa = 0.0f;
  d->ms_temp_c = 0.0f;
  if (s_ms_ready) {
    if (s_ms.read() == MS5611_READ_OK) {           // performs the conversion
      d->ms_press_pa = s_ms.getPressure() * 100.0f; // hPa -> Pa
      d->ms_temp_c   = s_ms.getTemperature();
      d->ms_ok       = true;
      s_msFails      = 0;
    } else if (++s_msFails >= SENSOR_FAIL_LIMIT) {
      s_ms_ready = false; s_msFails = 0;            // wedged -> let self-heal re-init
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
