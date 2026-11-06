/*
 * 小豆豆的一天 · 主题纯函数
 * ---------------------------------------------------------------
 * 这个文件刻意不依赖 Arduino：时段、调色板、弧线坐标、音量映射
 * 都可以在电脑上用 clang++ 直接测试（tools/test_firmware.sh）。
 */
#pragma once
#include <stdint.h>

enum Phase { PHASE_MORNING = 0, PHASE_DAY, PHASE_EVENING, PHASE_NIGHT };
enum WeekDotState { WEEK_FUTURE = 0, WEEK_DONE, WEEK_TODAY, WEEK_TODAY_DONE };

struct Palette {
  uint16_t bgTop, bgBottom;
  uint16_t sun, ray, face, cheek;
  uint16_t weekDone, weekToday, weekTodayRing, weekFuture;
  uint16_t sticker, stickerEdge;
  uint16_t text, accent;
  uint8_t  brightness;   // 0-100
};

constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)(((((rgb >> 16) & 0xFF) >> 3) << 11) |
                    ((((rgb >> 8) & 0xFF) >> 2) << 5) |
                    (((rgb & 0xFF) >> 3)));
}

Phase phase_for_time(int hour, int minute);
const char *phase_name(Phase p);
const Palette &palette_for_phase(Phase p);
int volume_steps(int percent, int maxSteps, bool night);
int backlight_duty(int percent);
int parse_bits7(const char *csv, bool out[7]);
bool parse_volume_pct(const char *s, int &out);
void week_dot_positions(int cx, int cy, int radius, int outX[7], int outY[7]);
WeekDotState week_dot_state(int i, bool calValid, const bool cal[7], int weekday);
void habit_arc_positions(int count, int cx, int cy, int radius, int outX[4], int outY[4]);
int habit_hit_test(int x, int y, const int px[4], const int py[4], int count, int radius);
void sun_ray_endpoints(Phase p, int cx, int cy, int bodyR, int rayR, int outX[8], int outY[8]);
uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t);
Palette palette_lerp(const Palette &a, const Palette &b, uint8_t t);
