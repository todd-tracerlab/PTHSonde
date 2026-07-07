#include "Thermistor.h"
#include <math.h>

// ---- Configurable constants -------------------------------------------------
float THERM_VREF     = 3.30f;     // divider supply / ADC reference, volts
float THERM_SERIES_R = 10000.0f;  // fixed series resistor, ohms

#define THERM_BAD_TEMP_C (-273.15f)  // sentinel returned on invalid input

float thermistorTempC(float voltage, bool* ok) {
  if (ok) *ok = false;

  // Safely reject zero, negative, NaN, or out-of-range voltages.
  // (voltage must be strictly between 0 and VREF for the divider math.)
  if (isnan(voltage) || voltage <= 0.0f || voltage >= THERM_VREF) {
    return THERM_BAD_TEMP_C;
  }

  const float VREF = THERM_VREF;

  // ---- EXACT thermistor conversion logic (do not alter) -------------------
  float r = THERM_SERIES_R * ((VREF / voltage) - 1.0);
  if (r <= 0.0f || isnan(r)) return THERM_BAD_TEMP_C;

  float lnR = log(r);

  float tempK = 1.0 / (
    0.8498083e-3 +
    2.608224e-4 * lnR +
    1.304578e-7 * pow(lnR, 3)
  );

  float tempC = tempK - 273.15;
  // -------------------------------------------------------------------------

  if (isnan(tempC) || isinf(tempC)) return THERM_BAD_TEMP_C;

  if (ok) *ok = true;
  return tempC;
}
