// 主机端测试：只依赖 ui_theme 的纯函数，不需要 Arduino/硬件。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "../../firmware/habit_coach/ui_theme.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto _a = (a);                                                         \
    auto _b = (b);                                                         \
    if (!(_a == _b)) {                                                     \
      std::printf("FAIL %s:%d: %s == %s (%ld != %ld)\n", __FILE__,         \
                  __LINE__, #a, #b, (long)_a, (long)_b);                   \
      failures++;                                                          \
    }                                                                      \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    long _a = (long)(a);                                                   \
    long _b = (long)(b);                                                   \
    long _t = (long)(tol);                                                 \
    if (std::labs(_a - _b) > _t) {                                         \
      std::printf("FAIL %s:%d: |%s - %s| <= %s (%ld vs %ld)\n", __FILE__, \
                  __LINE__, #a, #b, #tol, _a, _b);                         \
      failures++;                                                          \
    }                                                                      \
  } while (0)

int main() {
  // 时段四个边界 + 午夜
  CHECK_EQ(phase_for_time(6, 29), PHASE_NIGHT);
  CHECK_EQ(phase_for_time(6, 30), PHASE_MORNING);
  CHECK_EQ(phase_for_time(8, 29), PHASE_MORNING);
  CHECK_EQ(phase_for_time(8, 30), PHASE_DAY);
  CHECK_EQ(phase_for_time(16, 59), PHASE_DAY);
  CHECK_EQ(phase_for_time(17, 0), PHASE_EVENING);
  CHECK_EQ(phase_for_time(19, 59), PHASE_EVENING);
  CHECK_EQ(phase_for_time(20, 0), PHASE_NIGHT);
  CHECK_EQ(phase_for_time(0, 0), PHASE_NIGHT);

  // 亮度
  CHECK_EQ(palette_for_phase(PHASE_MORNING).brightness, 80);
  CHECK_EQ(palette_for_phase(PHASE_DAY).brightness, 100);
  CHECK_EQ(palette_for_phase(PHASE_EVENING).brightness, 70);
  CHECK_EQ(palette_for_phase(PHASE_NIGHT).brightness, 20);

  // RGB565
  CHECK_EQ(rgb565(0xFFFFFF), 0xFFFF);
  CHECK_EQ(rgb565(0x000000), 0x0000);
  CHECK_EQ(rgb565(0xFF0000), 0xF800);
  CHECK_EQ(rgb565(0x00FF00), 0x07E0);
  CHECK_EQ(rgb565(0x0000FF), 0x001F);

  // 音量：0 必须 0；100 到顶；夜间 60% 缩放
  CHECK_EQ(volume_steps(0, 21, false), 0);
  CHECK_EQ(volume_steps(100, 21, false), 21);
  CHECK_EQ(volume_steps(50, 21, false), 11);
  CHECK_EQ(volume_steps(50, 21, true), 6);
  CHECK_EQ(volume_steps(100, 30, false), 30);
  CHECK_EQ(volume_steps(50, 30, true), 9);

  // 7 位解析
  bool bits[7] = {};
  CHECK_EQ(parse_bits7("1,0,1,1,0,0,0", bits), 7);
  CHECK(bits[0] && !bits[1] && bits[2] && bits[3] && !bits[4] && !bits[5] &&
        !bits[6]);
  CHECK_EQ(parse_bits7("", bits), 0);
  CHECK_EQ(parse_bits7("1,0", bits), 2);
  CHECK_EQ(parse_bits7("1,0,1,1,0,0,0,1", bits), 7);
  CHECK_EQ(parse_bits7("1,x,1", bits), 3);
  CHECK(bits[0] && !bits[1] && bits[2]);

  // 音量字符串：只接受 0-100 的纯数字；toInt() 会把 "abc" 当 0，不能用
  int vp = -1;
  CHECK(parse_volume_pct("60", vp));
  CHECK_EQ(vp, 60);
  CHECK(parse_volume_pct("0", vp));
  CHECK_EQ(vp, 0);
  CHECK(parse_volume_pct("100", vp));
  CHECK_EQ(vp, 100);
  CHECK(!parse_volume_pct("101", vp));
  CHECK(!parse_volume_pct("-1", vp));
  CHECK(!parse_volume_pct("abc", vp));
  CHECK(!parse_volume_pct("", vp));
  CHECK(!parse_volume_pct("60x", vp));

  // 背光占空比（8 位）：夹紧 0-100，20% = 51
  CHECK_EQ(backlight_duty(0), 0);
  CHECK_EQ(backlight_duty(20), 51);
  CHECK_EQ(backlight_duty(50), 127);
  CHECK_EQ(backlight_duty(100), 255);
  CHECK_EQ(backlight_duty(-5), 0);
  CHECK_EQ(backlight_duty(150), 255);

  // 弧线坐标：240 与 360 都不越界、左右对称、顶部点在正上方
  for (int size : {240, 360}) {
    int cx = size / 2, cy = size / 2, R = size * 150 / 360;
    int x[7], y[7];
    week_dot_positions(cx, cy, R, x, y);
    CHECK_EQ(y[3], cy - R);
    CHECK_NEAR(x[0] + x[6], 2 * cx, 1);
    CHECK_NEAR(x[1] + x[5], 2 * cx, 1);
    for (int i = 0; i < 7; i++)
      CHECK(x[i] >= 0 && x[i] < size && y[i] >= 0 && y[i] < size);
  }

  // 习惯弧：1 件在正下方；2 件 ±16°；4 件 ±42°/±14°；不重复
  int hx[4], hy[4];
  habit_arc_positions(1, 180, 180, 140, hx, hy);
  CHECK_EQ(hx[0], 180);
  CHECK(hy[0] > 300);
  habit_arc_positions(2, 180, 180, 140, hx, hy);
  CHECK(hx[0] < 180 && hx[1] > 180);
  CHECK_NEAR(hx[0] + hx[1], 360, 1);
  habit_arc_positions(4, 180, 180, 140, hx, hy);
  CHECK(hx[0] < hx[1] && hx[1] < hx[2] && hx[2] < hx[3]);
  for (int i = 0; i < 4; i++)
    CHECK_EQ(habit_hit_test(hx[i], hy[i], hx, hy, 4, 30), i);
  int mx[4], my[4];
  habit_arc_positions(4, 120, 120, 93, mx, my);
  for (int i = 0; i < 4; i++)
    CHECK(mx[i] >= 0 && mx[i] < 240 && my[i] >= 0 && my[i] < 240);

  // 贴纸半径随屏幕缩放；圆命中包含边界
  CHECK_EQ(habit_sticker_radius(360), 30);
  CHECK_EQ(habit_sticker_radius(240), 20);
  CHECK(circle_hit(10, 10, 10, 10, 0));
  CHECK(circle_hit(10, 40, 10, 10, 30));
  CHECK(!circle_hit(10, 41, 10, 10, 30));
  CHECK(!circle_hit(50, 50, 10, 10, 30));

  // 触摸坐标映射：夹紧 + 可选镜像
  CHECK_EQ(touch_map_axis(100, 360, false), 100);
  CHECK_EQ(touch_map_axis(100, 360, true), 259);
  CHECK_EQ(touch_map_axis(-5, 360, false), 0);
  CHECK_EQ(touch_map_axis(400, 360, false), 359);
  CHECK_EQ(touch_map_axis(0, 240, true), 239);

  // 命中测试：中心命中、半径外不命中
  int px[1] = {180}, py[1] = {310};
  CHECK_EQ(habit_hit_test(180, 310, px, py, 1, 30), 0);
  CHECK_EQ(habit_hit_test(180, 250, px, py, 1, 30), -1);

  // 太阳光芒：8 条、都在身外、夜间更短
  int rx[8], ry[8], dx[8], dy[8];
  sun_ray_endpoints(PHASE_DAY, 180, 170, 52, 90, rx, ry);
  sun_ray_endpoints(PHASE_NIGHT, 180, 170, 52, 90, dx, dy);
  for (int i = 0; i < 8; i++) {
    int dDay =
        (rx[i] - 180) * (rx[i] - 180) + (ry[i] - 170) * (ry[i] - 170);
    int dNight =
        (dx[i] - 180) * (dx[i] - 180) + (dy[i] - 170) * (dy[i] - 170);
    CHECK(dDay >= 52 * 52);
    CHECK(dNight < dDay);
  }

  // 阶段名
  CHECK(std::strcmp(phase_name(PHASE_MORNING), "MORNING") == 0);
  CHECK(std::strcmp(phase_name(PHASE_DAY), "DAY") == 0);
  CHECK(std::strcmp(phase_name(PHASE_EVENING), "EVENING") == 0);
  CHECK(std::strcmp(phase_name(PHASE_NIGHT), "NIGHT") == 0);

  // 晨间上方光芒比下方长
  sun_ray_endpoints(PHASE_MORNING, 180, 170, 52, 90, rx, ry);
  {
    int top = (rx[0] - 180) * (rx[0] - 180) + (ry[0] - 170) * (ry[0] - 170);
    int bottom = (rx[4] - 180) * (rx[4] - 180) + (ry[4] - 170) * (ry[4] - 170);
    CHECK(top > bottom);
  }

  // 太阳几何：360/240 两档，且不越界
  SunGeom g = sun_geometry(360, 360);
  CHECK_EQ(g.cx, 180);
  CHECK_EQ(g.cy, 170);
  CHECK_EQ(g.bodyR, 52);
  CHECK_EQ(g.rayR, 90);
  CHECK_EQ(g.eyeDx, 20);
  CHECK_EQ(g.eyeDy, -8);
  CHECK_EQ(g.eyeR, 7);
  CHECK_EQ(g.mouthY, 20);
  CHECK(g.cy - g.rayR >= 0 && g.cy + g.bodyR < 360);
  SunGeom m = sun_geometry(240, 240);
  CHECK_EQ(m.cx, 120);
  CHECK_EQ(m.cy, 113);
  CHECK_EQ(m.bodyR, 34);
  CHECK_EQ(m.rayR, 60);
  CHECK(m.cy - m.rayR >= 0 && m.cy + m.bodyR < 240);
  CHECK(m.bodyR < g.bodyR);

  // 颜色插值与调色板插值端点
  CHECK_EQ(lerp565(0x1234, 0xABCD, 0), 0x1234);
  CHECK_EQ(lerp565(0x1234, 0xABCD, 255), 0xABCD);
  CHECK_EQ(palette_lerp(palette_for_phase(PHASE_DAY),
                        palette_for_phase(PHASE_NIGHT), 0)
               .bgTop,
           palette_for_phase(PHASE_DAY).bgTop);
  CHECK_EQ(palette_lerp(palette_for_phase(PHASE_DAY),
                        palette_for_phase(PHASE_NIGHT), 255)
               .bgTop,
           palette_for_phase(PHASE_NIGHT).bgTop);

  // 今天状态：无新字段时调用方走旧 WEEK 回退；有新字段时识别今天/今天完成
  bool cal[7] = {true, false, true, true, false, false, false};
  CHECK_EQ(week_dot_state(2, true, cal, 3), WEEK_TODAY_DONE);
  CHECK_EQ(week_dot_state(3, true, cal, 3), WEEK_DONE);
  CHECK_EQ(week_dot_state(0, true, cal, 3), WEEK_DONE);
  CHECK_EQ(week_dot_state(4, true, cal, 3), WEEK_FUTURE);
  bool calTodayOpen[7] = {true, false, false, true, false, false, false};
  CHECK_EQ(week_dot_state(2, true, calTodayOpen, 3), WEEK_TODAY);

  // 「今天」的点更大；完成/未来保持基础半径
  CHECK_EQ(week_dot_radius(WEEK_FUTURE, 7), 7);
  CHECK_EQ(week_dot_radius(WEEK_DONE, 7), 7);
  CHECK_EQ(week_dot_radius(WEEK_TODAY, 7), 9);
  CHECK_EQ(week_dot_radius(WEEK_TODAY_DONE, 7), 9);

  if (failures) {
    std::printf("%d ui_theme test(s) failed\n", failures);
    return 1;
  }
  std::printf("all ui_theme tests passed\n");
  return 0;
}
