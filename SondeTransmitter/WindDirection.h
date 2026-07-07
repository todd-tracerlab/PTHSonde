// =============================================================================
// WindDirection.h  -- wind direction derived from the sonde's GPS motion
//
// This is a RADIOSONDE: there is no wind vane. A free-floating sonde drifts
// WITH the surrounding air mass, so its GPS velocity vector is the wind vector.
//   * wind DIRECTION = GPS course-over-ground (optionally flipped to the
//                      meteorological "direction the wind comes from")
//   * wind SPEED     = GPS ground speed  (already sent as speed_cms)
//
// Kept as its own configurable conversion, completely separate from the
// thermistor math, so it can be recalibrated independently. Output is always
// wrapped to 0.00 .. 359.99 degrees.
// =============================================================================
#pragma once
#include <stdbool.h>

// Convert GPS course-over-ground + ground speed into a wind direction (degrees).
// Returns 0..359.99; sets *ok=false when ground speed is below the configured
// threshold (GPS course is just noise when the sonde is nearly stationary).
float windDirectionDeg(float gpsCourseDeg, float groundSpeed_mps, bool* ok);
