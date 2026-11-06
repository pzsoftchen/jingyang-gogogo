# 小豆豆的一天 · 固件 Phase 1 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在三种 ESP32-S3 板型上实现已批准的「小豆豆的一天」Phase 1 固件界面：四幕配色、太阳角色、顶部周弧、底部习惯弧、1.85C 直接点、夜间调光与音量。

**Architecture:** 新增无 Arduino 依赖的纯函数模块 `ui_theme.h/.cpp`（时段、调色板、弧线坐标、音量映射），用主机端 C++ 测试驱动；`habit_coach.ino` 只负责读输入、调纯函数、用 Arduino_GFX 绘制。协议只做加法：新增 `WEEK_CAL/WEEKDAY/VOL`，缺省回退旧字段。服务端、卡片配色、家长页不在本计划范围（父页面 worktree 处理）。

**Tech Stack:** Arduino ESP32 core 2.x/3.x、Arduino_GFX、CST816 触摸、clang++ 主机测试、arduino-cli。

**Spec:** `docs/superpowers/specs/2026-11-06-sun-ui-design.md`

## Global Constraints

- 1.85C 已验证配置不得回退：系统 I2C `SDA=GPIO11 / SCL=GPIO10`；TCA9554 P0=LCD_RST、P1=TP_RST；ST77916 厂商 init 表；`0x3A=0x05`；QSPI 40 MHz；`canvas->begin(GFX_SKIP_OUTPUT_BEGIN)`。
- 屏幕不显示中文；所有文案走语音或服务器出图。
- 不做积分、连续天数、排行榜、扣分、红叉或失败画面。
- 协议只追加；旧字段 `WEEK=` 保留，新固件优先 `WEEK_CAL/WEEKDAY`。
- 夜间亮度 20%，冷启动无时间时先夜间深色 + 20% 亮度。
- 240×240（DevKitC）本期只要求编译通过且布局不裁切；1.85C 为真机验收目标。
- 不新增第三方库；不改变每日推送预算。
- 所有改动只在 worktree 分支 `worktree-firmware-ui-phase1` 上提交。

## Review Focus

- 240×240 顶部弧、底部弧、太阳是否越界或重叠 —— 坐标测试覆盖 240/360 边界。
- 1.85C CST816 坐标轴/方向不匹配 —— 轻点会命中错误的习惯贴纸。
- 冷启动无 NTP 时是否先进入夜间 20% —— 夜间开机不能刺眼。
- 旧服务端没有 `WEEK_CAL/WEEKDAY` —— 必须回退 `WEEK=`，不能误亮「今天」。
- 音量 0 与夜间 60% 缩放 —— 不能静音失败或超出 0–21/0–30。

---

## 准备：基线提交

**Files:**
- 已存在于工作区：`.gitignore`、`firmware/habit_coach/habit_coach.ino`、`firmware/habit_coach/st77916_185c_init.h`、`docs/superpowers/specs/2026-11-06-sun-ui-design.md`

- [ ] **Step 1: 确认基线编译与测试通过**

Run: `make test && ./tools/build.sh all`
Expected: 54 个 Python 测试 OK；三种板型均输出 `Sketch uses ...`，无 error。

- [ ] **Step 2: 提交基线**

```bash
git add .gitignore docs/superpowers firmware/habit_coach/habit_coach.ino firmware/habit_coach/st77916_185c_init.h
git commit -m "基线：1.85C 上机修复 + 小豆豆 UI 设计文档"
```

---

### Task 1: 主题纯函数模块 + 主机测试脚手架

**Files:**
- Create: `firmware/habit_coach/ui_theme.h`
- Create: `firmware/habit_coach/ui_theme.cpp`
- Create: `tests/firmware/test_ui_theme.cpp`
- Create: `tools/test_firmware.sh`
- Modify: `Makefile`

**Interfaces:**
- Produces: `rgb565`、`phase_for_time`、`phase_name`、`palette_for_phase`、`volume_steps`、`parse_bits7`、`week_dot_positions`、`week_dot_state`、`habit_arc_positions`、`habit_hit_test`、`sun_ray_endpoints`、`lerp565`、`palette_lerp`（签名见下）

- [ ] **Step 1: 写失败测试**

`tests/firmware/test_ui_theme.cpp` 用最小断言宏（`CHECK`/`CHECK_EQ`），失败计数非零时返回 1。必须覆盖：

```cpp
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
CHECK(bits[0] && !bits[1] && bits[2] && bits[3] && !bits[4] && !bits[5] && !bits[6]);
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
  for (int i = 0; i < 7; i++) CHECK(x[i] >= 0 && x[i] < size && y[i] >= 0 && y[i] < size);
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
  int dDay = (rx[i] - 180) * (rx[i] - 180) + (ry[i] - 170) * (ry[i] - 170);
  int dNight = (dx[i] - 180) * (dx[i] - 180) + (dy[i] - 170) * (dy[i] - 170);
  CHECK(dDay >= 52 * 52);
  CHECK(dNight < dDay);
}

// 颜色插值与调色板插值端点
CHECK_EQ(lerp565(0x1234, 0xABCD, 0), 0x1234);
CHECK_EQ(lerp565(0x1234, 0xABCD, 255), 0xABCD);
CHECK_EQ(palette_lerp(palette_for_phase(PHASE_DAY), palette_for_phase(PHASE_NIGHT), 0).bgTop,
         palette_for_phase(PHASE_DAY).bgTop);
CHECK_EQ(palette_lerp(palette_for_phase(PHASE_DAY), palette_for_phase(PHASE_NIGHT), 255).bgTop,
         palette_for_phase(PHASE_NIGHT).bgTop);

// 今天状态：无新字段时调用方走旧 WEEK 回退；有新字段时识别今天/今天完成
bool cal[7] = {true, false, true, true, false, false, false};
CHECK_EQ(week_dot_state(2, true, cal, 3), WEEK_TODAY_DONE);
CHECK_EQ(week_dot_state(3, true, cal, 3), WEEK_TODAY);
CHECK_EQ(week_dot_state(0, true, cal, 3), WEEK_DONE);
CHECK_EQ(week_dot_state(4, true, cal, 3), WEEK_FUTURE);
```

新建 `tools/test_firmware.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CXX=${CXX:-clang++}
$CXX -std=c++17 -Wall -Wextra -O2 \
  -o /tmp/test_ui_theme \
  tests/firmware/test_ui_theme.cpp firmware/habit_coach/ui_theme.cpp
/tmp/test_ui_theme
```

`Makefile` 增加 `test-firmware`，并让 `test` 先跑它：

```make
test-firmware:
	./tools/test_firmware.sh

test: test-firmware
	python3 -m unittest discover -s tests -v
```

- [ ] **Step 2: 运行测试确认失败**

Run: `make test-firmware`
Expected: 编译失败，`ui_theme.h: No such file or directory`。

- [ ] **Step 3: 实现 `ui_theme.h` / `ui_theme.cpp`**

`ui_theme.h` 公开以下接口：

```cpp
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
  uint8_t  brightness;
};

constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xFF) >> 3) << 11 |
                    (((rgb >> 8) & 0xFF) >> 2) << 5 |
                    ((rgb & 0xFF) >> 3));
}

Phase phase_for_time(int hour, int minute);
const char *phase_name(Phase p);
const Palette &palette_for_phase(Phase p);
int volume_steps(int percent, int maxSteps, bool night);
int parse_bits7(const char *csv, bool out[7]);
void week_dot_positions(int cx, int cy, int radius, int outX[7], int outY[7]);
WeekDotState week_dot_state(int i, bool calValid, const bool cal[7], int weekday);
void habit_arc_positions(int count, int cx, int cy, int radius, int outX[4], int outY[4]);
int habit_hit_test(int x, int y, const int px[4], const int py[4], int count, int radius);
void sun_ray_endpoints(Phase p, int cx, int cy, int bodyR, int rayR, int outX[8], int outY[8]);
uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t);
Palette palette_lerp(const Palette &a, const Palette &b, uint8_t t);
```

实现要点（`ui_theme.cpp`，只用 `<math.h>`/`<stdint.h>`）：
- 时段边界：`390/510/1020/1200` 分钟。
- 调色板使用 spec 5.5 的十六进制值经 `rgb565()` 转换；夜间 `text=0xE8E2D4`、`accent=0xFFE8A3`。
- `volume_steps`：`percent` 夹到 0–100；`night` 时先乘 60/100；返回 `(percent*maxSteps+50)/100`。
- `week_dot_positions`：角度 `-90 + (i-3)*16` 度，半径 `radius`。
- `habit_arc_positions`：角度表 `{0} / {-16,16} / {-28,0,28} / {-42,-14,14,42}`，`x=cx+round(sin*R)`、`y=cy+round(cos*R)`。
- `habit_hit_test`：圆心距离平方 ≤ `radius²`，按数组顺序返回第一个命中索引，否则 -1。
- `sun_ray_endpoints`：8 条，角度 `-90 + i*45`；长度倍数：晨 `{1.15,1.0,0.85,0.75,0.85,1.0,1.15,1.15}`、昼全 1.0、暮右侧 `{0.9,1.0,1.2,1.2,1.0,0.85,0.85,0.9}`、夜全 0.6；端点半径 = `bodyR + (rayR-bodyR)*倍数`。
- `week_dot_state`：`calValid && weekday 1..7` 时，`i==weekday-1` 返回 `cal[i] ? WEEK_TODAY_DONE : WEEK_TODAY`；否则 `cal[i] ? WEEK_DONE : WEEK_FUTURE`。
- `palette_lerp`：每个颜色走 `lerp565`，`brightness` 线性插值。

- [ ] **Step 4: 运行主机测试确认通过**

Run: `make test-firmware`
Expected: 全部 `CHECK` 通过，输出 `all ui_theme tests passed`，退出码 0。

- [ ] **Step 5: 确认固件仍能编译**

Run: `./tools/build.sh all`
Expected: 三种板型均 `Sketch uses ...`，无 error。

- [ ] **Step 6: 提交**

```bash
git add firmware/habit_coach/ui_theme.h firmware/habit_coach/ui_theme.cpp \
        tests/firmware/test_ui_theme.cpp tools/test_firmware.sh Makefile
git commit -m "主题纯函数：四幕时段/调色板/弧线/音量映射 + 主机测试"
```

---

### Task 2: 协议新增 WEEK_CAL / WEEKDAY / VOL

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（全局状态、`syncState()`、新增 `applyVolume()`）

**Interfaces:**
- Consumes: `parse_bits7`、`volume_steps`（Task 1）
- Produces: 全局 `bool weekCalValid`、`bool weekCal[7]`、`int weekday`、`int volumePct`、`bool nightMode`；`void applyVolume()`

- [ ] **Step 1: 加测试**

在 `tests/firmware/test_ui_theme.cpp` 增加：`parse_bits7` 遇到超长字段只取前 7 个（`parse_bits7("1,0,1,1,0,0,0,1", bits)` 返回 7）；坏值 `"1,x,1"` 中 `x` 视为 0，返回 3。

Run: `make test-firmware`
Expected: 新断言失败（当前实现按需调整）。

- [ ] **Step 2: 实现解析与音量**

在 `habit_coach.ino` 全局区新增：

```cpp
bool weekCalValid = false;
bool weekCal[7] = {false, false, false, false, false, false, false};
int  weekday = 0;        // 1=周一 … 7=周日；0=未知
int  volumePct = -1;     // -1=服务器未下发
bool nightMode = false;  // 由 Task 3 的时段引擎更新
```

`syncState()` 的行解析中，在 `WEEK=` 之前增加：

```cpp
else if (line.startsWith("WEEK_CAL=")) {
  weekCalValid = (parse_bits7(line.substring(9).c_str(), weekCal) == 7);
}
else if (line.startsWith("WEEKDAY=")) weekday = line.substring(8).toInt();
else if (line.startsWith("VOL=")) {
  int v = line.substring(4).toInt();
  if (v >= 0 && v <= 100) { volumePct = v; applyVolume(); }
}
```

新增 `applyVolume()`：

```cpp
void applyVolume() {
  if (volumePct < 0) return;
#if USE_SD_AUDIO
  if (audioOk) audio.setVolume(volume_steps(volumePct, 21, nightMode));
#elif USE_DFPLAYER
  if (mp3Ok) mp3.volume(volume_steps(volumePct, 30, nightMode));
#endif
}
```

`audioInit()` 里 `audio.setVolume(12)` 改为 `applyVolume()`（`volumePct<0` 时保留 12 的默认值）。`setup()` 里 DFPlayer `mp3.volume(22)` 后调用 `applyVolume()`。

- [ ] **Step 3: 运行测试与编译**

Run: `make test-firmware && ./tools/build.sh all`
Expected: 主机测试全绿；三种板型编译通过。

- [ ] **Step 4: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino tests/firmware/test_ui_theme.cpp firmware/habit_coach/ui_theme.cpp
git commit -m "协议：解析 WEEK_CAL/WEEKDAY/VOL，音量按板型映射"
```

---

### Task 3: 背光 PWM + 四幕时段引擎

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（`setup()`、`loop()`、新增背光与时段函数）

**Interfaces:**
- Consumes: `phase_for_time`、`palette_for_phase`（Task 1）；`nightMode`、`applyVolume`（Task 2）
- Produces: 全局 `Phase currentPhase`、`unsigned long phaseChangedAt`；`void backlightInit()`、`void setBacklightPct(int)`、`void updatePhase(bool force)`

- [ ] **Step 1: 实现背光 PWM**

兼容 core 2.x/3.x，仿照现有 `buzzerInit()`：

```cpp
void backlightInit() {
#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  pinMode(LCD_BL, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LCD_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8); ledcAttachPin(LCD_BL, 0);
#endif
#else
  pinMode(TFT_BL, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8); ledcAttachPin(TFT_BL, 0);
#endif
#endif
}

void setBacklightPct(int pct) {
  pct = constrain(pct, 0, 100);
  int duty = pct * 255 / 100;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  ledcWrite(LCD_BL, duty);
  #else
  ledcWrite(TFT_BL, duty);
  #endif
#else
  ledcWrite(0, duty);
#endif
}
```

`setup()` 中把原来的 `pinMode(...); digitalWrite(..., HIGH);` 换成 `backlightInit(); setBacklightPct(20);`，保证无 NTP 冷启动先暗。

- [ ] **Step 2: 实现时段引擎**

```cpp
Phase currentPhase = PHASE_NIGHT;
unsigned long phaseChangedAt = 0;
unsigned long lastPhaseCheck = 0;

void updatePhase(bool force) {
  struct tm t;
  if (!getLocalTime(&t, 10)) return;          // 无时间：保持当前时段
  Phase p = phase_for_time(t.tm_hour, t.tm_min);
  if (!force && p == currentPhase) return;
  if (p != currentPhase) phaseChangedAt = millis();
  currentPhase = p;
  nightMode = (p == PHASE_NIGHT);
  setBacklightPct(palette_for_phase(p).brightness);
  applyVolume();                              // 夜间音量压到 60%
  needRedraw = true;
}
```

`setup()` 在 `connectWifi()` 和首次 `syncState()` 之后调用 `updatePhase(true)`。`loop()` 每 30 秒调用一次 `updatePhase(false)`。

- [ ] **Step 3: 加串口测试命令**

`handleSerial()` 增加 `p`：循环切换 `currentPhase`（`PHASE_MORNING→DAY→EVENING→NIGHT`），设置 `nightMode`、背光、`needRedraw`，并打印 `[phase] NAME brightness=NN`。用于真机验收四幕。

- [ ] **Step 4: 编译验证**

Run: `./tools/build.sh all`
Expected: 三种板型编译通过。

- [ ] **Step 5: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino
git commit -m "背光 PWM + 四幕时段：冷启动夜间 20%，VOL 夜间 60%"
```

---

### Task 4: 四幕背景 + 太阳角色

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（`drawEyes`/`drawMouth` 重写为 `drawSunFace`；新增 `drawBackground`/`drawSun`；`render()` 与 `playCelebration()` 改调用）

**Interfaces:**
- Consumes: `Palette`、`sun_ray_endpoints`（Task 1）；`currentPhase`（Task 3）
- Produces: `void drawBackground(const Palette &p)`、`void drawSun(const Palette &p)`、`void drawSunFace(int mode)`

- [ ] **Step 1: 加太阳几何测试**

`tests/firmware/test_ui_theme.cpp` 增加：`sun_ray_endpoints(PHASE_MORNING,...)` 顶部两条射线长度 ≥ 其余；`phase_name` 返回 `"MORNING"/"DAY"/"EVENING"/"NIGHT"`。

Run: `make test-firmware`
Expected: 新断言通过（若未实现 `phase_name` 则失败，先补实现）。

- [ ] **Step 2: 实现背景渐变**

`drawBackground`：把屏幕分成 24 条横带，颜色用 `lerp565(bgTop, bgBottom, t)` 插值，`ui->fillRect(0, y, SCREEN_W, bandH, color)`。最后一条覆盖到 `SCREEN_H`。

- [ ] **Step 3: 实现太阳角色**

新增 `#define S(v) ((int)((v) * SCREEN_W / 360.0f))`（360 设计基准）。`drawSun`：

- 中心 `cx=SCREEN_W/2`、`cy=SCREEN_H*17/36`；本体半径 `S(52)`，光芒外缘 `S(90)`。
- 先画 8 条光芒：`sun_ray_endpoints(currentPhase, cx, cy, S(52), S(90), rx, ry)`，从 `bodyR-S(4)` 起画到端点，线色 `p.ray`；每条端点加 `fillCircle(x,y,S(3),p.ray)` 做圆头。
- 再画本体 `fillCircle(cx,cy,S(52),p.sun)`。
- 腮红：`fillCircle(cx±S(30), cy+S(12), S(9), p.cheek)`。
- 面部按表情绘制：眼睛中心 `cx±S(20), cy-S(8)`，眼半径 `S(7)`，高光 `S(2)`；嘴中心 `cy+S(20)`。表情沿用现有 8 种（常态/开心/困/说话/惊讶/眨眼/睡/好奇），线条颜色 `p.face`，高光颜色 `p.sun`。
- 夜间表情额外在嘴上方画 `z z`（用 `p.accent`），沿用现有 `EXPR_SLEEPY` 分支。

把 `drawEyes(mode)` / `drawMouth(mode)` 替换为 `drawSunFace(mode)`，由 `drawSun` 调用；`playCelebration()` 里对眼睛/嘴的直接调用改为 `drawSun(palette_for_phase(currentPhase))` 后重绘。

- [ ] **Step 4: 接入 render**

`render()` 开头改为：

```cpp
if (cardMode) { renderCard(); return; }
Palette p = palette_for_phase(currentPhase);
drawBackground(p);
// 顶部旧的 7 点代码保持原样，Task 5 再替换为 drawWeekArc(p)
drawSun(p);
// 底部旧的图标代码保持原样，Task 7 再替换为 drawHabitArc(p)
// 进度条 / banner 改用 p.accent / p.weekFuture
if (canvasOk) canvas->flush();
needRedraw = false;
```

本任务不要引用 Task 5/7 尚未定义的函数，保证每一步都能单独编译。

- [ ] **Step 5: 编译验证**

Run: `make test-firmware && ./tools/build.sh all`
Expected: 主机测试全绿，三种板型编译通过。

- [ ] **Step 6: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino tests/firmware/test_ui_theme.cpp
git commit -m "四幕背景渐变 + 太阳角色（光芒姿势/可见五官/腮红）"
```

---

### Task 5: 顶部 7 点缓弧

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（新增 `drawWeekArc`，替换 `render()` 顶部旧格子）

**Interfaces:**
- Consumes: `week_dot_positions`、`week_dot_state`、`Palette`（Task 1）；`weekCalValid/weekCal/weekday`（Task 2）；`weekDone`（旧字段回退）
- Produces: `void drawWeekArc(const Palette &p)`

- [ ] **Step 1: 加状态测试**

确保 `week_dot_state` 的四类返回值在 Task 1 已覆盖；本任务增加「`calValid=false` 时调用方走 `weekDone`」的行为测试（纯函数层无需新增，测试注释说明由 `drawWeekArc` 分支保证）。

- [ ] **Step 2: 实现 drawWeekArc**

- 中心 `(SCREEN_W/2, SCREEN_H/2)`，半径 `S(150)`，`week_dot_positions` 取 7 个坐标。
- 新字段有效：`week_dot_state(i, weekCalValid, weekCal, weekday)`：
  - `WEEK_DONE`：`fillCircle(x,y,S(7),p.weekDone)`
  - `WEEK_TODAY`：`fillCircle(x,y,S(9),p.weekToday)` + `drawCircle(x,y,S(12),p.weekTodayRing)`
  - `WEEK_TODAY_DONE`：`fillCircle(x,y,S(9),p.weekDone)` + `drawCircle(x,y,S(12),p.weekTodayRing)` + 中心画一个小勾（两条 `drawLine`，颜色 `p.bgTop`）
  - `WEEK_FUTURE`：`drawCircle(x,y,S(7),p.weekFuture)`
- 旧字段回退（`weekCalValid == false`）：按 `weekDone[i]` 画 `WEEK_DONE` / `WEEK_FUTURE`，不画「今天」。
- WiFi 指示灯移到左上角 `(S(18), S(18))`，半径 `S(4)`，颜色沿用绿/灰。

- [ ] **Step 3: 编译验证**

Run: `./tools/build.sh all`
Expected: 三种板型编译通过。

- [ ] **Step 4: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino
git commit -m "顶部周弧：WEEK_CAL/WEEKDAY，今天珊瑚外圈，旧 WEEK 回退"
```

---

### Task 6: CST816 坐标读取 + 轻点/长按事件

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（`touchInit`/`touchGesture`/`readTouch` 区域与 `loop()` 触摸映射）

**Interfaces:**
- Produces: `bool touchReadPoint(int &x, int &y)`、`int readTouchEvent(int &x, int &y)`（0 无 / 1 轻点 / 2 长按，轻点返回最后坐标）
- Consumes: Task 7 的命中测试将使用事件坐标

- [ ] **Step 1: 实现坐标读取**

CST816 标准寄存器：`0x01=GestureID`、`0x02=FingerNum`、`0x03/0x04=XposH/L`、`0x05/0x06=YposH/L`。一次从 `0x01` 读 6 字节：

```cpp
#define CST816_REG_GESTURE 0x01
#define CST816_REG_FINGERS 0x02
#define CST816_REG_XH      0x03
#define CST816_REG_XL      0x04
#define CST816_REG_YH      0x05
#define CST816_REG_YL      0x06
```

`x = ((buf[2] & 0x0F) << 8) | buf[3]`，`y = ((buf[4] & 0x0F) << 8) | buf[5]`，按下判定用 `buf[1] > 0`（FingerNum）。若真机读数异常，先打印原始 6 字节再核对 Waveshare wiki 的寄存器表——不要直接假设 0x02 就是 XposH。

坐标映射宏（默认不交换/不镜像；真机方向不对时只改这里）：

```cpp
#define TP_SWAP_XY  0
#define TP_MIRROR_X 0
#define TP_MIRROR_Y 0

int mapTouchX(int rawX, int rawY) {
#if TP_SWAP_XY
  int v = rawY;
#else
  int v = rawX;
#endif
#if TP_MIRROR_X
  v = SCREEN_W - 1 - v;
#endif
  return constrain(v, 0, SCREEN_W - 1);
}
// mapTouchY 同理
```

- [ ] **Step 2: 实现事件状态机**

```cpp
bool touchHeld = false, touchLongFired = false;
unsigned long touchDownAt = 0;
int touchLastX = 0, touchLastY = 0;

bool touchReadPoint(int &x, int &y);   // 读到有效坐标才返回 true

int readTouchEvent(int &x, int &y) {
  int g = touchGesture();                 // 0x01 手势寄存器，非 0 = 按下
  bool now = (g != 0);
  if (now) {
    int rx, ry;
    if (touchReadPoint(rx, ry)) {
      touchLastX = mapTouchX(rx, ry);
      touchLastY = mapTouchY(rx, ry);
    }
    if (!touchHeld) { touchHeld = true; touchDownAt = millis(); touchLongFired = false; }
    else if (!touchLongFired && millis() - touchDownAt > 900) {
      touchLongFired = true;
      x = touchLastX; y = touchLastY;
      Serial.printf("[touch] long %d,%d\n", x, y);
      return 2;
    }
  } else if (touchHeld) {
    touchHeld = false;
    if (!touchLongFired) {
      x = touchLastX; y = touchLastY;
      Serial.printf("[touch] tap %d,%d\n", x, y);
      return 1;
    }
  }
  return 0;
}
```

`loop()` 中原来的 `readTouch()` 调用改为 `readTouchEvent(tx, ty)`，保留卡片模式和主界面的 `a/b` 事件映射（本任务先维持旧行为，Task 7 换成命中测试）。

- [ ] **Step 3: 编译验证**

Run: `./tools/build.sh all`
Expected: 三种板型编译通过（240 板 `USE_TOUCH=0`，代码不参与编译）。

- [ ] **Step 4: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino
git commit -m "CST816：读取触摸坐标，轻点/长按事件带坐标，串口可调试"
```

---

### Task 7: 底部习惯缓弧 + 直接点交互

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（`drawHabitIcon` 重写、新增 `drawHabitArc`/`hitHabit`/`hitSun`；`loop()` 触摸分支）

**Interfaces:**
- Consumes: `habit_arc_positions`、`habit_hit_test`、`Palette`（Task 1）；`readTouchEvent`（Task 6）；`completeHabit`、`startSeq`、`postPath`（现有）
- Produces: 全局 `int habitX[4], habitY[4], habitStickerR`；`void drawHabitArc(const Palette &p)`、`int hitHabit(int x, int y)`、`bool hitSun(int x, int y)`

- [ ] **Step 1: 加命中测试断言**

`tests/firmware/test_ui_theme.cpp` 增加：`habit_arc_positions(4, 120, 120, 93, ...)` 的四个点都在 240×240 内；`habit_hit_test` 在 4 件时能区分相邻贴纸（用中点构造用例，确保返回各自索引）。

Run: `make test-firmware`
Expected: 通过。

- [ ] **Step 2: 重写贴纸绘制**

`drawHabitIcon(int i, const Palette &p)`：
- `habitStickerR = S(30)`；底色 `p.sticker` 填充圆，边框 `p.stickerEdge` 两圈。
- 图标：目标直径 `habitStickerR*2 - S(8)`。若等于 40 直接用 `draw16bitRGBBitmap`；否则用最近邻采样（`pgm_read_word` 读 `HABIT_ICON_PIX[idx]`，逐目标像素 `ui->drawPixel`）。
- 已完成：右上角 `fillCircle(cx+S(18), cy-S(18), S(7), p.accent)` + 小勾（`p.bgTop` 两条线）；不再画大双圈。
- 仅 240（`HAS_BUTTONS`）画选中环 `drawCircle(cx,cy,habitStickerR+S(4),p.weekToday)`；触摸板不画。

`drawHabitArc`：`habit_arc_positions(habCount, SCREEN_W/2, SCREEN_H*17/36, S(140), habitX, habitY)`，逐个调用 `drawHabitIcon`。`habCount==0` 时画 `SYNCING` 文本。

- [ ] **Step 3: 直接点交互**

新增：

```cpp
int hitHabit(int x, int y) {
  return habit_hit_test(x, y, habitX, habitY, habCount, habitStickerR + S(8));
}
bool hitSun(int x, int y) {
  int cx = SCREEN_W / 2, cy = SCREEN_H * 17 / 36;
  int dx = x - cx, dy = y - cy, r = S(60);
  return dx * dx + dy * dy <= r * r;
}
```

`loop()` 触摸分支（仅 `USE_TOUCH`）：

```cpp
int tx = 0, ty = 0;
int t = readTouchEvent(tx, ty);
if (cardMode) {
  if (t == 2) a = 2;                 // 长按退出卡片
  else if (t == 1) b = 1;            // 轻点翻页
} else if (t == 1) {
  int i = hitHabit(tx, ty);
  if (i >= 0) {
    selected = i;
    animateStickerBounce(i);          // Task 8；先留空函数
    if (hab[i].minSec > 0 && !hab[i].done) {
      runIdx = i; runStart = millis();
      runDur = (unsigned long)hab[i].minSec * 1000UL;
      say(17); talkFor(2500); needRedraw = true;
    } else {
      completeHabit(i);
    }
  } else if (hitSun(tx, ty)) {
    if (habCount > 0 && allDone()) {
      say(VOICE_TELL_PARENTS); postPath("/api/child_note");
      talkFor(4000); setBanner("TELL PARENTS", 4000);
    } else {
      say(VOICE_Q_SAVED); postPath("/api/question_quick");
      talkFor(4000); setBanner("QUESTION SAVED", 4000);
    }
  }
} else if (t == 2) {
  syncState(); flushQueue(); setBanner("SYNCED", 2000);
}
```

240 板保留现有按键分支不动（`a==1` 完成选中、`a==2` 记录、`b==1` 切换、`b==2` 同步）。

- [ ] **Step 4: 编译验证**

Run: `make test-firmware && ./tools/build.sh all`
Expected: 主机测试全绿；三种板型编译通过。

- [ ] **Step 5: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino tests/firmware/test_ui_theme.cpp
git commit -m "底部习惯弧 + 直接点：点贴纸完成、点太阳记录、长按同步"
```

---

### Task 8: 动画与时段过渡

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`（`completeHabit`、`render`、`loop`、新增动画函数）
- Modify: `firmware/habit_coach/ui_theme.h/.cpp`（如需 `palette_lerp` 已存在）

**Interfaces:**
- Consumes: `lerp565`、`palette_lerp`（Task 1）；`habitX/habitY`（Task 7）；`week_dot_positions`（Task 1）
- Produces: `void animateStickerBounce(int idx)`、`void animateStarFlight(int fromX, int fromY, int toX, int toY)`

- [ ] **Step 1: 加插值测试**

`tests/firmware/test_ui_theme.cpp` 增加 `palette_lerp` 中间值单调性：`t=128` 的 `bgTop` 不等于两端（防止未插值）；`lerp565` 对相同颜色返回原值。

Run: `make test-firmware`
Expected: 通过。

- [ ] **Step 2: 实现贴纸轻弹与星星飞行**

- `animateStickerBounce(idx)`：对 `dy = {0,-S(3),-S(1),0}` 四帧，调用 `render()` 后在贴纸位置画一圈高亮（`drawCircle`），`delay(40)`；结束 `needRedraw = true`。
- `animateStarFlight(fromX, fromY, toX, toY)`：30 帧，`t=i/29.0`，坐标线性插值 + `sin(pi*t)*S(20)` 抛物线偏移，画 `drawStar(x,y,S(8),true,false)`（颜色用 `p.accent`），每帧 `delay(20)`；最后 `needRedraw = true`。
- `completeHabit(idx)` 在 `hab[idx].done = true` 后调用：起点 `habitX[idx], habitY[idx]`，终点取今天周点（`weekCalValid && weekday 1..7` 时 `week_dot_positions` 的第 `weekday-1` 个，否则 `(SCREEN_W/2, S(30))`）。

- [ ] **Step 3: 实现时段过渡**

- 全局 `Palette oldPalette; unsigned long phaseTransitionStart = 0;`；`updatePhase` 变化时保存 `oldPalette = palette_for_phase(previousPhase)`、`phaseTransitionStart = millis()`。
- `render()` 计算 `Palette p = (millis()-phaseTransitionStart < 2000) ? palette_lerp(oldPalette, palette_for_phase(currentPhase), (millis()-phaseTransitionStart)*255/2000) : palette_for_phase(currentPhase);`
- 时段切换时角色动作：晨 `happyFor(1500)`、暮 `expr = EXPR_WINK; exprUntil = millis()+1500`、夜 `expr = EXPR_SLEEPY; exprUntil = millis()+1500`、昼保持常态。
- `loop()` 在过渡期间每 100ms 设 `needRedraw = true`。

- [ ] **Step 4: 庆祝动画配色**

`playCelebration` 的背景和颜色改用当前 `Palette`（`p.bgTop`、`p.accent`、`p.sun`、`p.weekDone`），不再用旧 `C_*` 常量。

- [ ] **Step 5: 编译验证**

Run: `make test-firmware && ./tools/build.sh all`
Expected: 主机测试全绿；三种板型编译通过。

- [ ] **Step 6: 提交**

```bash
git add firmware/habit_coach/habit_coach.ino tests/firmware/test_ui_theme.cpp
git commit -m "动画：贴纸轻弹、星星飞向周点、四幕 2 秒过渡与新庆祝配色"
```

---

### Task 9: 文档与最终验证

**Files:**
- Modify: `docs/board-porting.md`
- Modify: `README.md`（串口命令与交互说明）
- Modify: `docs/superpowers/specs/2026-11-06-sun-ui-design.md`（仅状态行改为「已批准，固件 Phase 1 实施中」）

- [ ] **Step 1: 更新 board-porting.md**

- 1.85C 引脚表：`LCD_RST=EXIO0`、`TP_RST=EXIO1`、系统 I2C `SDA=11 / SCL=10`；注明 1.85 与 1.85C 的差异。
- 「诚实清单」：1.85C 显示/触摸/WiFi/卡片/TCA/QSPI 已上机验证（2026-11-06）；1.85 裸板仍未上机。
- 记录已验证参数：厂商 ST77916 init 表、`0x3A=0x05`、40 MHz、`GFX_SKIP_OUTPUT_BEGIN`。
- 触摸坐标调试：串口 `[touch] tap x,y`；方向不对时改 `TP_SWAP_XY/TP_MIRROR_X/TP_MIRROR_Y`。

- [ ] **Step 2: 更新 README**

串口调试命令补 `p`（切换四幕）、`d/D/G`（屏幕自检）；交互补「1.85C 轻点贴纸完成、轻点太阳记录、长按同步；卡片轻点翻页/长按退出」。

- [ ] **Step 3: 最终验证**

Run:
```bash
make test
./tools/build.sh all
make check
```
Expected: 主机 + Python 测试全绿；三种板型编译通过；Python 语法检查 OK。

- [ ] **Step 4: 真机验收清单（交给用户执行）**

在 1.85C 上烧录后逐项确认：
1. 串口 `p` 切四幕：背景/太阳/亮度正确，夜间 20% 不刺眼。
2. 顶部 7 点：有 `WEEK_CAL/WEEKDAY` 时今天珊瑚外圈；旧服务端时无今天标记但不崩。
3. 轻点每个习惯贴纸直接完成；点已完成贴纸提示「ALREADY DONE」。
4. 点太阳记录问题；全部完成后点太阳记录留言。
5. 长按主界面立即同步；卡片内轻点翻页、长按退出。
6. 音量滑块改变后设备音量生效；夜间自动降到 60%。
7. 冷启动（拔电再上电、不连网）先夜间深色 + 20% 亮度。

- [ ] **Step 5: 提交**

```bash
git add docs/board-porting.md README.md docs/superpowers/specs/2026-11-06-sun-ui-design.md
git commit -m "文档：1.85C 验证记录、新交互与串口命令"
```

---

## Self-Review

- **Spec coverage:** 5.1→Task 3/4；5.2→Task 4；5.3→Task 5/7；5.4→Task 5；5.5→Task 1/4；5.7→Task 8；5.8→Task 3；6.4 音量→Task 2/3；7 约束→Global Constraints；8 协议→Task 2；9 测试→Task 1/9；10 Phase 1→范围。卡片配色与家长页明确不在本计划（服务端 worktree）。
- **Type consistency:** `weekCalValid/weekCal/weekday/volumePct/nightMode` 在 Task 2 定义，Task 3/5 使用；`Palette` 在 Task 1 定义，Task 4–8 使用；`habitX/habitY/habitStickerR` 在 Task 7 定义，Task 8 使用。
- **Review Focus tests:** 240/360 边界→Task 1；CST816 方向→Task 6 串口坐标 + Task 9 真机清单；冷启动夜间→Task 3 + Task 9 清单；旧服务端回退→Task 5 分支 + Task 9 清单；音量 0/夜间缩放→Task 1。
