// =============================================================================
// WindAverage.h  -- time-windowed VECTOR average of wind (speed + heading)
//
// Wind cannot be averaged as a scalar heading: averaging 359 deg and 1 deg must
// give 0 deg, not 180 deg. So each sample is decomposed into u/v components
// (speed * sin/cos of heading), the components are averaged over the trailing
// window (default 5 s), and the result is recombined into an averaged speed and
// heading (circular mean). This is the standard meteorological mean wind.
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

void windAvgBegin(uint32_t window_ms);                 // e.g. 5000 for 5 s
void windAvgAddSample(float dir_deg, float speed_mps, uint32_t now_ms);

// Average over the trailing window. Returns false if the window is empty.
bool windAvgGet(float* avg_dir_deg, float* avg_speed_mps);
