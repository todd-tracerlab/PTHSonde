// =============================================================================
// Thermistor.h  -- NTC thermistor -> temperature (Steinhart-Hart)
//
// Conversion logic is exactly as specified. VREF is the divider supply /
// ADC full-scale reference and is configurable at runtime via THERM_VREF.
// Invalid/zero/out-of-range input is handled safely (returns ok=false).
// =============================================================================
#pragma once
#include <stdbool.h>

// Configurable reference voltage (volts). Default 3.30 V (board 3V3 rail).
extern float THERM_VREF;

// Series resistor in the divider (ohms). Edit if your hardware differs.
extern float THERM_SERIES_R;

// Convert measured thermistor-node voltage to degrees C.
// Returns degC; sets *ok=false (and a sentinel temperature) on bad input.
float thermistorTempC(float voltage, bool* ok);
