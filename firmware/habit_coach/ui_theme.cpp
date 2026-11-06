#include "ui_theme.h"

#include <math.h>

namespace {

constexpr float kPi = 3.14159265358979323846f;

const Palette kPalettes[4] = {
    // 晨：奶油 → 蜜桃
    {rgb565(0xFFF8EA), rgb565(0xFFD9B5), rgb565(0xFFB703), rgb565(0xFFC93C),
     rgb565(0x4A3B32), rgb565(0xFF8FA3), rgb565(0x5BC8B0), rgb565(0xFF8A65),
     rgb565(0x2F6F63), rgb565(0xB9C7C2), rgb565(0xFFFDF4), rgb565(0xF3D6A8),
     rgb565(0x3E3228), rgb565(0xE76F51), 80},
    // 昼：浅天蓝 → 近白
    {rgb565(0xDDF3FF), rgb565(0xF8FDFF), rgb565(0xFFC93C), rgb565(0xFFC93C),
     rgb565(0x4A3B32), rgb565(0xFF8FA3), rgb565(0x5BC8B0), rgb565(0xFF8A65),
     rgb565(0x2F6F63), rgb565(0xB9C7C2), rgb565(0xFFFDF4), rgb565(0xF3D6A8),
     rgb565(0x3E3228), rgb565(0xE76F51), 100},
    // 暮：橙 → 丁香晚霞
    {rgb565(0xFFB38A), rgb565(0xC7B8EA), rgb565(0xFF9F5A), rgb565(0xFFC07A),
     rgb565(0x4A3B32), rgb565(0xFF8FA3), rgb565(0x5BC8B0), rgb565(0xFF8A65),
     rgb565(0x2F6F63), rgb565(0xB9C7C2), rgb565(0xFFFDF4), rgb565(0xF3D6A8),
     rgb565(0x3E3228), rgb565(0xE76F51), 70},
    // 夜：深蓝 → 靛蓝
    {rgb565(0x1B2340), rgb565(0x2E3B63), rgb565(0xF4C56A), rgb565(0xE0AE58),
     rgb565(0x4A3B32), rgb565(0xFF8FA3), rgb565(0x5BC8B0), rgb565(0xFF8A65),
     rgb565(0xFFE8A3), rgb565(0x6B7A9E), rgb565(0x38446E), rgb565(0x56649A),
     rgb565(0xE8E2D4), rgb565(0xFFE8A3), 20},
};

const int kHabitDeg[4][4] = {
    {0, 0, 0, 0},
    {-16, 16, 0, 0},
    {-28, 0, 28, 0},
    {-42, -14, 14, 42},
};

const float kRayMult[4][8] = {
    {1.15f, 1.00f, 0.85f, 0.75f, 0.85f, 1.00f, 1.15f, 1.15f},
    {1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
    {0.90f, 1.00f, 1.20f, 1.20f, 1.00f, 0.85f, 0.85f, 0.90f},
    {0.60f, 0.60f, 0.60f, 0.60f, 0.60f, 0.60f, 0.60f, 0.60f},
};

uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t t) {
  int v = (int)a + ((int)b - (int)a) * (int)t / 255;
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

Phase phase_for_time(int hour, int minute) {
  int m = hour * 60 + minute;
  if (m >= 390 && m < 510) return PHASE_MORNING;    // 06:30-08:29
  if (m >= 510 && m < 1020) return PHASE_DAY;       // 08:30-16:59
  if (m >= 1020 && m < 1200) return PHASE_EVENING;  // 17:00-19:59
  return PHASE_NIGHT;                               // 20:00-06:29
}

const char *phase_name(Phase p) {
  switch (p) {
    case PHASE_MORNING: return "MORNING";
    case PHASE_DAY: return "DAY";
    case PHASE_EVENING: return "EVENING";
    default: return "NIGHT";
  }
}

const Palette &palette_for_phase(Phase p) {
  int idx = (int)p;
  if (idx < 0 || idx > 3) idx = 3;
  return kPalettes[idx];
}

int volume_steps(int percent, int maxSteps, bool night) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  if (night) percent = percent * 60 / 100;
  if (maxSteps <= 0) return 0;
  return (percent * maxSteps + 50) / 100;
}

SunGeom sun_geometry(int screenW, int screenH) {
  SunGeom g;
  g.cx = screenW / 2;
  g.cy = screenH * 17 / 36;
  g.bodyR = 52 * screenW / 360;
  g.rayR = 90 * screenW / 360;
  g.eyeDx = 20 * screenW / 360;
  g.eyeDy = -8 * screenW / 360;
  g.eyeR = 7 * screenW / 360;
  g.mouthY = 20 * screenW / 360;
  g.cheekDx = 30 * screenW / 360;
  g.cheekDy = 12 * screenW / 360;
  g.cheekR = 9 * screenW / 360;
  return g;
}

int backlight_duty(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return percent * 255 / 100;
}

int parse_bits7(const char *csv, bool out[7]) {
  if (!csv) return 0;
  int n = 0;
  const char *p = csv;
  while (n < 7 && *p) {
    out[n++] = (*p == '1');
    while (*p && *p != ',') p++;
    if (*p == ',') {
      p++;
      if (!*p) break;   // 容忍末尾逗号
    } else {
      break;
    }
  }
  return n;
}

bool parse_volume_pct(const char *s, int &out) {
  if (!s || !*s) return false;
  int v = 0;
  const char *p = s;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 100) return false;
    p++;
  }
  if (*p) return false;   // 只接受纯数字
  out = v;
  return true;
}

void week_dot_positions(int cx, int cy, int radius, int outX[7], int outY[7]) {
  for (int i = 0; i < 7; i++) {
    float a = (float)(-90 + (i - 3) * 16) * kPi / 180.0f;
    outX[i] = cx + (int)lroundf(cosf(a) * (float)radius);
    outY[i] = cy + (int)lroundf(sinf(a) * (float)radius);
  }
}

WeekDotState week_dot_state(int i, bool calValid, const bool cal[7],
                            int weekday) {
  if (i < 0 || i > 6) return WEEK_FUTURE;
  if (calValid && weekday >= 1 && weekday <= 7) {
    if (i == weekday - 1) return cal[i] ? WEEK_TODAY_DONE : WEEK_TODAY;
  }
  return cal[i] ? WEEK_DONE : WEEK_FUTURE;
}

void habit_arc_positions(int count, int cx, int cy, int radius, int outX[4],
                         int outY[4]) {
  if (count < 1) count = 1;
  if (count > 4) count = 4;
  for (int i = 0; i < count; i++) {
    float a = (float)kHabitDeg[count - 1][i] * kPi / 180.0f;
    outX[i] = cx + (int)lroundf(sinf(a) * (float)radius);
    outY[i] = cy + (int)lroundf(cosf(a) * (float)radius);
  }
}

int habit_hit_test(int x, int y, const int px[4], const int py[4], int count,
                   int radius) {
  int r2 = radius * radius;
  for (int i = 0; i < count && i < 4; i++) {
    int dx = x - px[i];
    int dy = y - py[i];
    if (dx * dx + dy * dy <= r2) return i;
  }
  return -1;
}

void sun_ray_endpoints(Phase p, int cx, int cy, int bodyR, int rayR, int outX[8],
                       int outY[8]) {
  int idx = (int)p;
  if (idx < 0 || idx > 3) idx = 3;
  for (int i = 0; i < 8; i++) {
    float a = (float)(-90 + i * 45) * kPi / 180.0f;
    float len = (float)bodyR + (float)(rayR - bodyR) * kRayMult[idx][i];
    outX[i] = cx + (int)lroundf(cosf(a) * len);
    outY[i] = cy + (int)lroundf(sinf(a) * len);
  }
}

uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  if (t == 0) return a;
  if (t == 255) return b;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (br - ar) * (int)t / 255;
  int g = ag + (bg - ag) * (int)t / 255;
  int bl = ab + (bb - ab) * (int)t / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

Palette palette_lerp(const Palette &a, const Palette &b, uint8_t t) {
  Palette out;
  out.bgTop = lerp565(a.bgTop, b.bgTop, t);
  out.bgBottom = lerp565(a.bgBottom, b.bgBottom, t);
  out.sun = lerp565(a.sun, b.sun, t);
  out.ray = lerp565(a.ray, b.ray, t);
  out.face = lerp565(a.face, b.face, t);
  out.cheek = lerp565(a.cheek, b.cheek, t);
  out.weekDone = lerp565(a.weekDone, b.weekDone, t);
  out.weekToday = lerp565(a.weekToday, b.weekToday, t);
  out.weekTodayRing = lerp565(a.weekTodayRing, b.weekTodayRing, t);
  out.weekFuture = lerp565(a.weekFuture, b.weekFuture, t);
  out.sticker = lerp565(a.sticker, b.sticker, t);
  out.stickerEdge = lerp565(a.stickerEdge, b.stickerEdge, t);
  out.text = lerp565(a.text, b.text, t);
  out.accent = lerp565(a.accent, b.accent, t);
  out.brightness = lerp_u8(a.brightness, b.brightness, t);
  return out;
}
