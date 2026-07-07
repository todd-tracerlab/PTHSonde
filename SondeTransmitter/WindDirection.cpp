#include "WindDirection.h"
#include <math.h>

// #############################################################################
// #  WIND DERIVATION CALIBRATION  --  EDIT ONLY INSIDE THIS BLOCK             #
// #                                                                           #
// #  Wind is computed from the sonde's GPS track, not a sensor input. These   #
// #  constants are independent of the thermistor conversion.                  #
// #############################################################################

// true  -> report the direction the wind blows FROM (meteorological convention)
// false -> report the direction the sonde is moving TOWARD (raw GPS course)
static const bool  WIND_REPORT_FROM   = true;

// Fixed bearing offset applied after the convention (heading/site calibration).
static const float WIND_OFFSET_DEG    = 0.0f;

// Minimum ground speed for a trustworthy direction (m/s). Below this the GPS
// course is mostly noise, so the direction is flagged invalid.
static const float WIND_MIN_SPEED_MPS = 0.5f;
// #############################################################################
// #  END CALIBRATION BLOCK                                                    #
// #############################################################################

static float wrap360(float d) {
  d = fmodf(d, 360.0f);
  if (d < 0.0f) d += 360.0f;
  if (d >= 360.0f) d = 0.0f;
  return d;
}

float windDirectionDeg(float gpsCourseDeg, float groundSpeed_mps, bool* ok) {
  if (ok) *ok = false;
  if (isnan(gpsCourseDeg) || isnan(groundSpeed_mps)) return 0.0f;

  // Direction is undefined when the sonde is essentially not moving.
  if (groundSpeed_mps < WIND_MIN_SPEED_MPS) return 0.0f;

  float dir = gpsCourseDeg;
  if (WIND_REPORT_FROM) dir += 180.0f;          // direction wind comes FROM
  dir = wrap360(dir + WIND_OFFSET_DEG);

  if (ok) *ok = true;
  return dir;
}
