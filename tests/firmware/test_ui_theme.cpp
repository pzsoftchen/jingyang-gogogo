// 主机端测试：只依赖 ui_theme 的纯函数，不需要 Arduino/硬件。
#include <cstdio>
#include <cstdlib>
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

  if (failures) {
    std::printf("%d ui_theme test(s) failed\n", failures);
    return 1;
  }
  std::printf("all ui_theme tests passed\n");
  return 0;
}
