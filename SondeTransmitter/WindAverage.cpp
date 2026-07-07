#include "WindAverage.h"
#include <math.h>

#ifndef DEG2RAD
#define DEG2RAD (3.14159265358979323846f / 180.0f)
#define RAD2DEG (180.0f / 3.14159265358979323846f)
#endif

// Ring buffer of samples. At 1 Hz telemetry a 5 s window holds ~5 samples;
// 64 slots leaves plenty of headroom if you sample faster.
#define WIND_AVG_MAX 64

typedef struct { uint32_t t; float u; float v; } WindSample;

static WindSample s_buf[WIND_AVG_MAX];
static int        s_start  = 0;     // index of oldest sample
static int        s_count  = 0;     // number of valid samples
static uint32_t   s_window = 5000;

void windAvgBegin(uint32_t window_ms) {
  s_window = window_ms;
  s_start  = 0;
  s_count  = 0;
}

static void prune(uint32_t now) {
  // Drop samples older than the window (unsigned subtraction handles wrap).
  while (s_count > 0) {
    uint32_t age = now - s_buf[s_start].t;
    if (age <= s_window) break;
    s_start = (s_start + 1) % WIND_AVG_MAX;
    s_count--;
  }
}

void windAvgAddSample(float dir_deg, float speed_mps, uint32_t now_ms) {
  if (isnan(dir_deg) || isnan(speed_mps)) return;
  if (speed_mps < 0.0f) speed_mps = 0.0f;

  float r = dir_deg * DEG2RAD;
  WindSample s;
  s.t = now_ms;
  s.u = speed_mps * sinf(r);   // east component
  s.v = speed_mps * cosf(r);   // north component

  int idx = (s_start + s_count) % WIND_AVG_MAX;
  if (s_count < WIND_AVG_MAX) {
    s_buf[idx] = s;
    s_count++;
  } else {
    // Buffer full: overwrite the oldest and advance.
    s_buf[s_start] = s;
    s_start = (s_start + 1) % WIND_AVG_MAX;
  }

  prune(now_ms);
}

bool windAvgGet(float* avg_dir_deg, float* avg_speed_mps) {
  if (s_count <= 0) return false;

  float sumU = 0.0f, sumV = 0.0f;
  for (int i = 0; i < s_count; i++) {
    int idx = (s_start + i) % WIND_AVG_MAX;
    sumU += s_buf[idx].u;
    sumV += s_buf[idx].v;
  }
  float mu = sumU / s_count;
  float mv = sumV / s_count;

  float spd = sqrtf(mu * mu + mv * mv);
  float dir = 0.0f;
  if (spd > 1e-4f) {
    dir = atan2f(mu, mv) * RAD2DEG;   // atan2(east, north) -> compass bearing
    dir = fmodf(dir, 360.0f);
    if (dir < 0.0f) dir += 360.0f;
    if (dir >= 360.0f) dir = 0.0f;
  }

  if (avg_dir_deg)   *avg_dir_deg = dir;
  if (avg_speed_mps) *avg_speed_mps = spd;
  return true;
}
