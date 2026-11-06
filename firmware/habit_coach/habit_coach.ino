/*
 * 习惯伙伴 · 设备端固件
 * ---------------------------------------------------------------
 * v3：同一份固件支持两块板（改一行 BOARD 宏）：
 *     - BOARD_DEVKITC_GC9A01 : 240×240 GC9A01(SPI) + DFPlayer + 两个按键（参考实现）
 *     - BOARD_WS_1_85        : 360×360 ST77916(QSPI) + CST816 触摸 + PCM5101(I2S) + TF 卡
 *     换板指南见 docs/board-porting.md
 *
 * v2：习惯数量由家长页决定（1~4 件），设备通过 /api/state.txt 自动同步，
 *     不需要再改代码；断网时用 NVS 里缓存的习惯继续工作。
 *
 * 设计原则：
 *   1. 断网也能用 —— 打卡、星星、本周格子全部本地完成，联网只是同步。
 *   2. 屏幕不显示中文（Arduino_GFX 默认只有 ASCII 字库）——5~10 岁本来就
 *      识字少，所以：屏幕只画表情、星星、格子；所有"话"都由语音说出。
 *   3. 语音全部来自 SD 卡上的预录音频（编号由服务器下发），延迟低、不花钱。
 *   4. AI 不夸孩子，"爸妈的认可"由家长从手机发出来，设备只负责播出来。
 *
 * 依赖库：Arduino_GFX_Library (>=1.6.5)、DFRobotDFPlayerMini（可选）
 * 开发板：ESP32S3 Dev Module / Flash 16MB / PSRAM: OPI PSRAM / Huge APP
 *
 * 按键：A=完成当前习惯  长按A=你教教我   B=切换习惯  长按B=立即同步
 * 串口：t=试音 r=重置今天 s=立即同步 q=查看队列 h=打印习惯表
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include "habit_icons.h"      // 习惯 emoji 图标（由 tools/gen_icons.py 生成）
#include "st77916_185c_init.h" // 1.85C 专用 ST77916 厂商初始化表
#include "ui_theme.h"         // 四幕时段/调色板/弧线/音量纯函数
#include <esp_heap_caps.h>
#include <pgmspace.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
// 没有 secrets.h 时进入纯离线模式（也能玩）
#ifndef DEVICE_ID
#define DEVICE_ID "poc"        // 每台设备唯一；上云后用于鉴权与多设备
#endif
#ifndef DEVICE_TOKEN
#define DEVICE_TOKEN ""        // 服务器设了 DEVICE_TOKEN 环境变量时，这里要填一致
#endif

#define WIFI_SSID ""
#define WIFI_PASS ""
#define SERVER_HOST "192.168.1.10"
#define SERVER_PORT 8099
#endif

// ======================= 选板 =======================
// BOARD_DEVKITC_GC9A01 : ESP32-S3 DevKitC-1 + GC9A01 1.28" 240×240（SPI，参考实现）
// BOARD_WS_1_85         : Waveshare ESP32-S3-Touch-LCD-1.85（ST77916 QSPI 360×360
//                         + CST816 电容触摸 + PCM5101 I2S + TF 卡 + RTC + 电池）
#define BOARD_DEVKITC_GC9A01 0
#define BOARD_WS_1_85        1   // 标准版裸板（1.85）
#define BOARD_WS_1_85C       2   // 带音箱盒版（1.85C / 1.85C-BOX）：多了外壳、5W 喇叭、电池仓

#ifndef BOARD
#define BOARD BOARD_DEVKITC_GC9A01
#endif

// ======================= 功能开关 =======================
#define USE_WIFI       1   // 设 0 则完全离线
#define ENABLE_REMIND  1   // 定时语音提醒

#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  #define USE_DFPLAYER   0
  #define USE_TOUCH      1
  #define USE_SD_AUDIO   1     // TF 卡 + I2S（需要 ESP32-audioI2S 库）
  #define HAS_BUTTONS    0
  #define SCREEN_W     360
  #define SCREEN_H     360
#else
  #define USE_DFPLAYER   1
  #define USE_TOUCH      0
  #define USE_SD_AUDIO   0
  #define HAS_BUTTONS    1
  #define SCREEN_W     240
  #define SCREEN_H     240
#endif

// 布局缩放：旧绘图坐标以 240 为设计基准，乘这个系数适配 360
#define UI_K ((float)SCREEN_W / 240.0f)
#define K(v) ((int)((v) * UI_K))
// 新主题（小豆豆的一天）以 360 设计稿为基准
#define S(v) ((int)((v) * SCREEN_W / 360.0f))

// ======================= 引脚 =======================
#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  // 屏幕 ST77916（QSPI）——1.85 与 1.85C 完全一致
  #define LCD_SDA0  46
  #define LCD_SDA1  45
  #define LCD_SDA2  42
  #define LCD_SDA3  41
  #define LCD_SCK   40
  #define LCD_CS    21
  #define LCD_BL     5
  #define LCD_RST_EXIO 0      // 官方 1.85C：LCD/触摸复位在 TCA9554 P0/P1，不是 GPIO
  // 触摸 CST816：1.85 用独立总线(1/3)；1.85C 与主 I2C 共用(11/10)
  #if BOARD == BOARD_WS_1_85C
    #define TP_SDA   11
    #define TP_SCL   10
    #define TP_ON_SYS_BUS 1
  #else
    #define TP_SDA    1
    #define TP_SCL    3
    #define TP_ON_SYS_BUS 0
  #endif
  #define TP_INT     4
  #define TP_RST_EXIO 1       // TCA9554 P1
  // 系统 I2C（TCA9554 / RTC / IMU 共用）：SDA=GPIO11, SCL=GPIO10
  // 注意：1.85C 的触摸也挂在这条总线上，写反会读到 0xFF。
  #define SYS_SDA   11
  #define SYS_SCL   10
  // I2S 音频（PCM5101 → 功放 → 喇叭）
  #define I2S_BCK   48
  #define I2S_LRCK  38
  #define I2S_DIN   47
  // TF 卡（SDMMC 1 线）
  #define SD_CLK    14
  #define SD_CMD    17
  #define SD_D0     16
  // 板载 I2S 数字麦克风（MIC_WS=2 / MIC_SCK=15 / MIC_SD=39），但默认关闭：
  // 孩子音频不出设备（见 docs/network-strategy.md 红线 2）。
  // 改成 1 只允许「本地唤醒 + 本地命令词」，不要把音频上传。
  #define USE_MIC    0
  // 这块板没有用户按键（只有 RESET/BOOT），交互靠触摸
  #define BTN_A     -1
  #define BTN_B     -1
  #define BUZZER    -1
#else
  #define TFT_SCK   12
  #define TFT_MOSI  11
  #define TFT_CS     9
  #define TFT_DC    10
  #define TFT_RST   18
  #define TFT_BL     7
  #define BTN_A      4
  #define BTN_B      5
  #define BUZZER     6
  #define MP3_RX    16
  #define MP3_TX    17
#endif

// ======================= 显示 =======================
#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_SDA0, LCD_SDA1, LCD_SDA2, LCD_SDA3);
Arduino_GFX *gfx = new Arduino_ST77916(bus, GFX_NOT_DEFINED /* RST 走 TCA9554 */, 0 /* rotation */,
                                       true /* IPS */, SCREEN_W, SCREEN_H,
                                       0, 0, 0, 0,
                                       st77916_185c_init_operations,
                                       sizeof(st77916_185c_init_operations));
#else
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0, true);
#endif
Arduino_Canvas *canvas = nullptr;
Arduino_GFX *ui = gfx;
bool canvasOk = false;

// ======================= 习惯（由服务器下发，最多 4 件）=======================
#define MAX_HABITS 4

struct Habit {
  String  id;         // 与服务器 habits.id 对应
  bool    done;
  uint8_t hh, mm;     // 提醒时间，0:0 表示不提醒
  int     rt;         // 提醒语音编号（单句）
  int     dt;         // 完成语音编号
  int     rtSeq[4];    // 流程的引导语音序列（如 脏衣服→洗澡）
  int     rtSeqMin[4]; // 每句在第几分钟播（出门流程 0/7/12）
  int     rtSeqN;
  int     minSec;      // >0：按下不立刻亮星，先倒计时这么多秒
};

Habit hab[MAX_HABITS];
int   habCount = 0;
bool  remindedToday[MAX_HABITS] = {false, false, false, false};
int   selected = 0;
int   habitX[4] = {0, 0, 0, 0};   // 底部习惯贴纸中心（命中测试用）
int   habitY[4] = {0, 0, 0, 0};
int   habitStickerR = 30;

// 问题卡模式：设备不解析中文，只把服务器画好的 240x240 RGB565 贴到屏上
bool  cardMode = false;
bool  cardPending = false;
int   cardN = 0, cardIdx = 0;
uint16_t *cardBuf = nullptr;

// 按键状态（必须定义在所有函数之前：Arduino 会把函数原型生成到文件顶部）
struct Btn { int8_t pin; bool last; unsigned long pressedAt; bool longFired; };
Btn btnA = {BTN_A, HIGH, 0, false};
Btn btnB = {BTN_B, HIGH, 0, false};

// 二选一时刻：完成全部后，偶尔让小豆豆问一句"你想看我跳舞还是听秘密"
bool choiceMode = false;
unsigned long choiceUntil = 0;

// "按下 → 计时 → 才点亮"（防止随手按）
int runIdx = -1;
unsigned long runStart = 0, runDur = 0, lastBarMs = 0;

// 流程引导语音的播放队列（一条流程 = 几句话，按"第几分钟"排期）
int seqBuf[4];
unsigned long seqAt[4];          // 每句话的绝对触发时间（millis）
int seqN = 0, seqIdx = 0;

// 语音编号（对应 SD 卡 mp3/xxxx.mp3）
#define VOICE_BOOT        1
#define VOICE_ALL_DONE    8
#define VOICE_ONE_LEFT    9
#define VOICE_TEACH_ME   11
#define VOICE_THANKS     12
#define VOICE_MSG_GENERIC 19
#define VOICE_EGG_BASE   400   // 0401~0420 彩蛋语音（低概率触发）
#define VOICE_EGG_COUNT   20
#define EGG_CHANCE_PCT     7   // 完成时 % 概率说一句彩蛋
#define VOICE_Q_SAVED     65   // 「这个问题真好，我们记下来」
#define VOICE_TELL_PARENTS 70  // 「好，我记住了。晚上记得告诉爸爸妈妈」
#define VOICE_CHOICE_Q   601   // 二选一：跳舞还是秘密
#define VOICE_CHOICE_A   602   // 跳舞
#define VOICE_CHOICE_B   603   // 秘密
#define CHOICE_CHANCE_PCT 20   // 完成全部后出现二选一时刻的概率

// 表情档位（借鉴 Skylight Buddy 的"角色有情绪"）
enum { EXPR_IDLE = 0, EXPR_HAPPY, EXPR_BLINK, EXPR_TALK, EXPR_SURPRISED,
       EXPR_WINK, EXPR_SLEEPY, EXPR_CURIOUS };

// ======================= 旧庆祝动画调色板 =======================
// 主界面/太阳已改用 ui_theme 的四幕 Palette；这里只保留庆祝动画还需要的颜色。
#define C_INK      0x18E3
#define C_STAR_ON  0xFFE0
#define C_STAR_OFF 0x2945
#define C_ACCENT   0xFD20

// ======================= 语音 =======================
#if USE_DFPLAYER
#include <DFRobotDFPlayerMini.h>
DFRobotDFPlayerMini mp3;
bool mp3Ok = false;
#endif

#if USE_TOUCH
#include <Wire.h>
#define CST816_ADDR 0x15
bool touchReady = false;
#endif

// ======================= 状态 =======================
Preferences prefs;
bool weekDone[7] = {false, false, false, false, false, false, false};  // 近 7 天是否完成（旧字段）
bool weekCalValid = false;                                             // 服务器是否下发 WEEK_CAL
bool weekCal[7] = {false, false, false, false, false, false, false};   // 本周一到周日
int  weekday = 0;             // 1=周一 … 7=周日；0=未知
int  volumePct = -1;          // 家长页音量 0-100；-1=服务器未下发
bool nightMode = false;       // 由时段引擎更新
Phase currentPhase = PHASE_NIGHT;   // 冷启动先按夜间，NTP 成功后再切
unsigned long phaseChangedAt = 0;
unsigned long lastPhaseCheck = 0;
int  streak = 0;
int  protoVer = 1;        // 服务器协议版本（V=2）
String packVer = "";      // 服务器上的语音包版本（PACK=voice.1）
String todayKey = "";        // "2026-10-30"
bool  wifiOk = false;
unsigned long lastSync = 0, lastWifiTry = 0, lastBlink = 0, lastBannerClear = 0;
bool needRedraw = true;
int  expr = 0;               // 0 常态 1 开心 2 困 3 说话
unsigned long exprUntil = 0;
String banner = "";          // 屏幕上的一行提示（ASCII，给家长看）
String queue = "";           // 离线打卡队列："brush@2026-10-30;"

// 前置声明
bool postCheckin(const String &habit);
void postPath(const String &path);
String deviceQuery();
String apiUrl(const String &path);
void setBanner(const String &s, unsigned long ms = 4000);
void say(int track);

// ======================= 音频 =======================
#if USE_SD_AUDIO
#include <SD_MMC.h>
// 直接 include：用 __has_include 判断时编译器还没拿到库路径，会把自己跳过。
#include <Audio.h>
Audio audio;
#endif

// 兼容 ESP32 Arduino core 2.x 与 3.x 的 LEDC API
void buzzerInit() {
#if BUZZER >= 0
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER, 2000, 10);
#else
  ledcSetup(0, 2000, 10);
  ledcAttachPin(BUZZER, 0);
#endif
#endif
}

void beep(uint16_t freq, uint16_t ms) {
#if BUZZER >= 0
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER, freq); delay(ms);
  ledcWriteTone(BUZZER, 0);    ledcWrite(BUZZER, 0);
#else
  ledcWriteTone(0, freq); delay(ms);
  ledcWriteTone(0, 0);    ledcWrite(0, 0);
#endif
#endif
}

#if USE_SD_AUDIO
bool audioOk = false;

// 1.85：TF 卡（SDMMC 1 线）+ PCM5101 I2S，播 /mp3/xxxx.mp3
void audioInit() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[audio] TF 卡挂载失败（确认卡是 FAT32，里面有 mp3/ 目录）");
    return;
  }
  audio.setPinout(I2S_BCK, I2S_LRCK, I2S_DIN);
  audio.setVolume(12);          // 默认 0-21；服务器下发 VOL 后覆盖
  audioOk = true;
  applyVolume();
  Serial.println("[audio] TF + I2S OK");
}

void audioLoop() {
  if (audioOk) audio.loop();
}
#else
void audioInit() {}
void audioLoop() {}
#endif

// 家长页音量 0-100 → 板型对应的音量档位；夜间自动压到 60%
void applyVolume() {
  if (volumePct < 0) return;
#if USE_SD_AUDIO
  if (audioOk) audio.setVolume(volume_steps(volumePct, 21, nightMode));
#elif USE_DFPLAYER
  if (mp3Ok) mp3.volume(volume_steps(volumePct, 30, nightMode));
#endif
}

void say(int track) {
#if USE_SD_AUDIO
  if (audioOk && track > 0) {
    char path[24];
    snprintf(path, sizeof(path), "/mp3/%04d.mp3", track);
    audio.connecttoFS(SD_MMC, path);
    return;
  }
#elif USE_DFPLAYER
  if (mp3Ok && track > 0) { mp3.play((uint16_t)track); return; }
#endif
  beep(1800, 70); delay(90); beep(2400, 70);
  (void)track;
}

// 完成音效：三种轮换，避免听觉疲劳
// DevKitC 用蜂鸣器；1.85 播 0501~0503 的短音效（和语音走同一条 I2S）
void playDoneChime() {
  int v = random(0, 3);
#if USE_SD_AUDIO
  say(501 + v);
#else
  if (v == 0) { beep(1320, 60); delay(70); beep(1760, 90); }
  else if (v == 1) { beep(1760, 60); delay(60); beep(1320, 60); delay(60); beep(1980, 90); }
  else { beep(990, 70); delay(80); beep(1480, 110); }
#endif
}

void happyFor(unsigned long ms = 2500)  { expr = 1; exprUntil = millis() + ms; needRedraw = true; }
void talkFor(unsigned long ms = 2500)   { expr = 3; exprUntil = millis() + ms; needRedraw = true; }

// 一条流程的语音按"第几分钟"排队：出门流程 0/7/12 分，洗澡 0/1 分
void startSeq(int idx) {
  unsigned long now = millis();
  seqN = hab[idx].rtSeqN;
  for (int i = 0; i < seqN && i < 4; i++) {
    seqBuf[i] = hab[idx].rtSeq[i];
    seqAt[i]  = now + (unsigned long)hab[idx].rtSeqMin[i] * 60000UL;
  }
  seqIdx = 0;
}

void cancelSeq() { seqN = 0; seqIdx = 0; }

// ======================= 绘制：小豆豆（太阳角色） =======================
// 五官颜色必须与太阳本体有对比：深棕五官 + 太阳色高光
void drawSunFace(int mode, const Palette &p, const SunGeom &g) {
  int lx = g.cx - g.eyeDx, rx = g.cx + g.eyeDx;
  int ey = g.cy + g.eyeDy;
  int e = g.eyeR;
  int hi = (e / 3 > 1) ? e / 3 : 1;

  if (mode == EXPR_WINK) {
    ui->drawLine(lx - e, ey, lx + e, ey, p.face);
    ui->fillCircle(rx, ey, e, p.face);
    ui->fillCircle(rx + hi, ey - hi, hi, p.sun);
  } else if (mode == EXPR_CURIOUS) {
    ui->fillCircle(lx, ey, e + 2, p.face);
    ui->fillCircle(rx, ey, e - 2, p.face);
    ui->fillCircle(lx + hi, ey - hi, hi, p.sun);
    ui->fillCircle(rx + 1, ey - 1, hi, p.sun);
  } else if (mode == EXPR_SURPRISED) {
    ui->fillCircle(lx, ey, e + 3, p.face);
    ui->fillCircle(rx, ey, e + 3, p.face);
    ui->fillCircle(lx + hi, ey - hi * 2, hi, p.sun);
    ui->fillCircle(rx + hi, ey - hi * 2, hi, p.sun);
  } else if (mode == 2 || mode == EXPR_SLEEPY) {
    ui->drawLine(lx - e, ey, lx + e, ey, p.face);
    ui->drawLine(rx - e, ey, rx + e, ey, p.face);
  } else if (mode == 1 || mode == EXPR_HAPPY) {
    ui->drawLine(lx - e, ey + 2, lx, ey - 3, p.face);
    ui->drawLine(lx, ey - 3, lx + e, ey + 2, p.face);
    ui->drawLine(rx - e, ey + 2, rx, ey - 3, p.face);
    ui->drawLine(rx, ey - 3, rx + e, ey + 2, p.face);
  } else {
    ui->fillCircle(lx, ey, e, p.face);
    ui->fillCircle(rx, ey, e, p.face);
    ui->fillCircle(lx + hi, ey - hi, hi, p.sun);
    ui->fillCircle(rx + hi, ey - hi, hi, p.sun);
  }

  int cx = g.cx, my = g.cy + g.mouthY;
  if (mode == EXPR_SURPRISED) {
    ui->fillCircle(cx, my, e, p.face);
  } else if (mode == EXPR_CURIOUS) {
    ui->drawLine(cx - e, my + 2, cx + 1, my - 1, p.face);
    ui->drawLine(cx + 1, my - 1, cx + e + 1, my + 1, p.face);
  } else if (mode == EXPR_SLEEPY) {
    ui->drawLine(cx - e, my, cx, my - 3, p.face);
    ui->drawLine(cx, my - 3, cx + e, my, p.face);
    ui->setTextColor(p.accent);
    ui->setTextSize(1);
    ui->setCursor(cx + g.cheekDx, my - g.cheekDy);
    ui->print("z z");
  } else if (mode == 1 || mode == EXPR_HAPPY) {
    ui->fillCircle(cx, my, e + 4, p.face);
    ui->fillCircle(cx, my - 3, e + 4, p.sun);
  } else if (mode == 3) {
    ui->fillCircle(cx, my, e - 1, p.face);
  } else {
    ui->drawLine(cx - e - 2, my, cx - 2, my + 3, p.face);
    ui->drawLine(cx - 2, my + 3, cx + 2, my + 3, p.face);
    ui->drawLine(cx + 2, my + 3, cx + e + 2, my, p.face);
  }
}

void drawSun(const Palette &p) {
  SunGeom g = sun_geometry(SCREEN_W, SCREEN_H);
  int rx[8], ry[8];
  sun_ray_endpoints(currentPhase, g.cx, g.cy, g.bodyR, g.rayR, rx, ry);
  int startR = g.bodyR - S(4);
  for (int i = 0; i < 8; i++) {
    float dx = (float)(rx[i] - g.cx), dy = (float)(ry[i] - g.cy);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) continue;
    int sx = g.cx + (int)(dx * startR / len);
    int sy = g.cy + (int)(dy * startR / len);
    ui->drawLine(sx, sy, rx[i], ry[i], p.ray);
    ui->fillCircle(rx[i], ry[i], S(3), p.ray);
  }
  ui->fillCircle(g.cx, g.cy, g.bodyR, p.sun);
  ui->fillCircle(g.cx - g.cheekDx, g.cy + g.cheekDy, g.cheekR, p.cheek);
  ui->fillCircle(g.cx + g.cheekDx, g.cy + g.cheekDy, g.cheekR, p.cheek);
  drawSunFace(expr, p, g);
}

void drawBackground(const Palette &p) {
  const int bands = 24;
  for (int i = 0; i < bands; i++) {
    int y0 = SCREEN_H * i / bands;
    int y1 = SCREEN_H * (i + 1) / bands;
    uint8_t t = (uint8_t)(i * 255 / (bands - 1));
    ui->fillRect(0, y0, SCREEN_W, y1 - y0, lerp565(p.bgTop, p.bgBottom, t));
  }
}

// 顶部 7 点：周一到周日，沿缓弧排布；今天更大 + 珊瑚外圈
void drawWeekArc(const Palette &p) {
  int x[7], y[7];
  week_dot_positions(SCREEN_W / 2, SCREEN_H / 2, S(150), x, y);
  for (int i = 0; i < 7; i++) {
    WeekDotState st = weekCalValid
        ? week_dot_state(i, true, weekCal, weekday)
        : (weekDone[i] ? WEEK_DONE : WEEK_FUTURE);
    int r = week_dot_radius(st, S(7));
    if (st == WEEK_DONE) {
      ui->fillCircle(x[i], y[i], r, p.weekDone);
    } else if (st == WEEK_TODAY) {
      ui->fillCircle(x[i], y[i], r, p.weekToday);
      ui->drawCircle(x[i], y[i], S(12), p.weekTodayRing);
    } else if (st == WEEK_TODAY_DONE) {
      ui->fillCircle(x[i], y[i], r, p.weekDone);
      ui->drawCircle(x[i], y[i], S(12), p.weekTodayRing);
      int u = (r / 3 > 1) ? r / 3 : 1;
      ui->drawLine(x[i] - u, y[i], x[i], y[i] + u, p.bgTop);
      ui->drawLine(x[i], y[i] + u, x[i] + 2 * u, y[i] - u, p.bgTop);
    } else {
      ui->drawCircle(x[i], y[i], r, p.weekFuture);
    }
  }
  ui->fillCircle(S(18), S(18), S(4), wifiOk ? 0x07E0 : 0x4208);
}

void drawStar(int cx, int cy, int r, bool filled, bool sel) {
  int px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float a = -PI / 2 + i * PI / 5.0f;
    float rr = (i % 2 == 0) ? r : r * 0.45f;
    px[i] = cx + cos(a) * rr;
    py[i] = cy + sin(a) * rr;
  }
  for (int i = 0; i < 10; i++) {
    int j = (i + 1) % 10;
    ui->drawLine(px[i], py[i], px[j], py[j], filled ? C_STAR_ON : C_STAR_OFF);
  }
  if (filled) for (int i = 0; i < 10; i++) ui->drawLine(cx, cy, px[i], py[i], C_STAR_ON);
  if (sel) ui->drawCircle(cx, cy, r + K(7), C_ACCENT);
}

void renderCard();   // 前置声明

// ---------- 习惯图标（不识字也能认出"现在该做哪件"）----------
int iconIndexOf(const String &id) {
  for (int i = 0; i < HABIT_ICON_COUNT; i++) {
    if (id == HABIT_ICON_IDS[i]) return i;
  }
  return -1;
}

// 奶油贴纸：图标 + 已完成右上角小星星（不再画大双圈）
void drawHabitIcon(int i, const Palette &p) {
  if (i < 0 || i >= habCount) return;
  int cx = habitX[i], cy = habitY[i], r = habitStickerR;
  ui->fillCircle(cx, cy, r, p.sticker);
  ui->drawCircle(cx, cy, r, p.stickerEdge);
  ui->drawCircle(cx, cy, r - 1, p.stickerEdge);

  int idx = iconIndexOf(hab[i].id);
  int size = r * 2 - S(8);
  if (size < 1) size = 1;
  if (idx >= 0) {
    if (size >= HABIT_ICON_W) {
      ui->draw16bitRGBBitmap(cx - HABIT_ICON_W / 2, cy - HABIT_ICON_H / 2,
                             (const uint16_t *)HABIT_ICON_PIX[idx],
                             HABIT_ICON_W, HABIT_ICON_H);
    } else {
      // 最近邻缩放：240 板上把 40x40 图标缩进更小的贴纸
      const uint8_t *src = (const uint8_t *)HABIT_ICON_PIX[idx];
      for (int y = 0; y < size; y++) {
        int sy = y * HABIT_ICON_H / size;
        for (int x = 0; x < size; x++) {
          int sx = x * HABIT_ICON_W / size;
          uint16_t c = pgm_read_word(src + (sy * HABIT_ICON_W + sx) * 2);
          ui->drawPixel(cx - size / 2 + x, cy - size / 2 + y, c);
        }
      }
    }
  } else {
    drawStar(cx, cy, S(16), hab[i].done, false);
  }

  if (hab[i].done) {
    int bx = cx + r - S(8), by = cy - r + S(8);
    ui->fillCircle(bx, by, S(7), p.accent);
    int u = (S(3) > 0) ? S(3) : 2;
    ui->drawLine(bx - u, by, bx, by + u, p.bgTop);
    ui->drawLine(bx, by + u, bx + 2 * u, by - u, p.bgTop);
  }

#if HAS_BUTTONS
  if (i == selected) ui->drawCircle(cx, cy, r + S(4), p.weekToday);
#endif
}

void animateStickerBounce(int idx) { (void)idx; }   // Task 8 实现

void drawHabitArc(const Palette &p) {
  habitStickerR = habit_sticker_radius(SCREEN_W);
  habit_arc_positions(habCount, SCREEN_W / 2, SCREEN_H * 17 / 36, S(140),
                      habitX, habitY);
  if (habCount == 0) {
    ui->setTextColor(p.text);
    ui->setTextSize(2);
    ui->setCursor(K(46), K(172));
    ui->print("SYNCING");
    return;
  }
  for (int i = 0; i < habCount; i++) drawHabitIcon(i, p);
}

int hitHabit(int x, int y) {
  return habit_hit_test(x, y, habitX, habitY, habCount,
                        habitStickerR + S(8));
}

bool hitSun(int x, int y) {
  SunGeom g = sun_geometry(SCREEN_W, SCREEN_H);
  return circle_hit(x, y, g.cx, g.cy, S(60));
}

// ---------- 庆祝动画：4 种图案轮换 ----------
// 借鉴 Skylight Buddy：完成整条流程时"下星星雨"，有时换成别的东西（他们换成火烈鸟、猫）。
// 这里用同样的机制：星星 / 爱心 / 随机 emoji 图标 / 彩纸。
void drawHeart(int cx, int cy, int r, uint16_t col) {
  ui->fillCircle(cx - r / 2, cy - r / 3, r * 2 / 3, col);
  ui->fillCircle(cx + r / 2, cy - r / 3, r * 2 / 3, col);
  int px[3] = {cx - r, cy - r / 4};
  int py[3] = {cx, cy + r * 3 / 4};
  // 用折线近似三角
  ui->drawLine(cx - r, cy - r / 4, cx, cy + r * 3 / 4, col);
  ui->drawLine(cx, cy + r * 3 / 4, cx + r, cy - r / 4, col);
  ui->fillCircle(cx, cy + r / 8, r * 3 / 5, col);
  (void)px; (void)py;
}

void playCelebration(int kind) {
  // kind: 0 星星雨 1 爱心雨 2 emoji 图标雨 3 彩纸
  const int N = 16;
  int px[N], py[N], spd[N], rot[N];
  uint16_t col[N];
  const uint16_t palette[5] = {C_STAR_ON, 0xFE60, 0xFD20, 0x07FF, 0xF81F};
  int w = SCREEN_W, h = SCREEN_H;
  for (int i = 0; i < N; i++) {
    px[i] = random(K(16), w - K(16));
    py[i] = -random(0, h / 2);          // 错开出场
    spd[i] = random(K(6), K(14));
    rot[i] = random(0, 3);
    col[i] = palette[random(0, 5)];
  }
  int iconPool[4];
  for (int i = 0; i < 4; i++) iconPool[i] = random(0, HABIT_ICON_COUNT);

  int savedExpr = expr;
  expr = EXPR_HAPPY;
  Palette cp = palette_for_phase(currentPhase);
  for (int f = 0; f < 42; f++) {
    drawBackground(cp);
    drawSun(cp);
    for (int i = 0; i < N; i++) {
      int y = py[i] + f * spd[i];
      int x = px[i] + (int)(sin((f + i * 7) / 6.0f) * K(6));
      if (y < 0 || y > h) continue;
      if (kind == 0) {
        drawStar(x, y, K(9), true, false);
      } else if (kind == 1) {
        drawHeart(x, y, K(9), col[i]);
      } else if (kind == 2) {
        int idx = iconPool[i % 4];
        if (y > K(HABIT_ICON_H / 2) && y < h - K(HABIT_ICON_H / 2)) {
          ui->draw16bitRGBBitmap(x - HABIT_ICON_W / 2, y - HABIT_ICON_H / 2,
                                 (const uint16_t *)HABIT_ICON_PIX[idx],
                                 HABIT_ICON_W, HABIT_ICON_H);
        }
      } else {
        int s2 = K(6);
        ui->fillRect(x, y, s2, s2 + (f % 3) * K(2), col[i]);
      }
    }
    if (canvasOk) canvas->flush();
    delay(30);
  }
  expr = savedExpr;
  needRedraw = true;
}

void render() {
  if (cardMode) { renderCard(); return; }
  Palette p = palette_for_phase(currentPhase);
  drawBackground(p);

  drawWeekArc(p);
  drawSun(p);

  drawHabitArc(p);

  // 计时进度条
  if (runIdx >= 0 && runDur > 0) {
    int w = K(160), x0 = SCREEN_W / 2 - w / 2, yb = K(205), hb = K(10);
    ui->drawRect(x0, yb, w, hb, p.weekFuture);
    unsigned long e = millis() - runStart;
    int f = (int)((e >= runDur) ? w - 2 : (e * (unsigned long)(w - 2)) / runDur);
    if (f > 0) ui->fillRect(x0 + 1, yb + 1, f, hb - 2, p.weekDone);
  }

  if (banner.length()) {
    ui->setTextSize(1);
    ui->setTextColor(p.accent);
    ui->setCursor(K(14), K(224));
    ui->print(banner);
  }

  if (canvasOk) canvas->flush();
  needRedraw = false;
}

// ---------- 问题卡 / 识字卡：服务器出图，设备只贴屏 ----------
bool fetchCard(int idx) {
  if (!wifiOk) return false;
  if (!cardBuf) {
    const size_t need = (size_t)SCREEN_W * SCREEN_H * 2;
    cardBuf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cardBuf) cardBuf = (uint16_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
    if (!cardBuf) { setBanner("NO MEM", 3000); return false; }
  }
  HTTPClient http;
  http.begin(apiUrl(String("/api/card.rgb565?i=") + String(idx) +
                    "&s=" + String(SCREEN_W)));
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  long len = (long)http.getSize();
  const long want = (long)((size_t)SCREEN_W * SCREEN_H * 2);
  if (len != want) { http.end(); return false; }
  WiFiClient *st = http.getStreamPtr();
  uint8_t *p = (uint8_t *)cardBuf;
  long got = 0;
  unsigned long t0 = millis();
  while (got < len && millis() - t0 < 6000) {
    int n = st->read(p + got, (size_t)(len - got));
    if (n > 0) got += n; else delay(1);
  }
  http.end();
  return got == len;
}

void renderCard() {
  if (!cardBuf) { cardMode = false; needRedraw = true; return; }
  if (canvasOk) {
    memcpy(canvas->getFramebuffer(), cardBuf, (size_t)SCREEN_W * SCREEN_H * 2);
    canvas->flush();
  } else {
    gfx->draw16bitRGBBitmap(0, 0, cardBuf, SCREEN_W, SCREEN_H);
  }
  needRedraw = false;
}

void enterCard(int idx) {
  if (cardN <= 0) return;
  cardIdx = (idx < 0) ? 0 : idx;
  if (fetchCard(cardIdx)) {
    cardMode = true;
    needRedraw = true;
    Serial.printf("[card] page %d/%d\n", cardIdx + 1, cardN);
  } else {
    setBanner("CARD FAIL", 2500);
  }
}

void setBanner(const String &s, unsigned long ms) {   // 默认值在前置声明里
  banner = s;
  lastBannerClear = millis() + ms;
  needRedraw = true;
}

// ======================= 习惯表 =======================
int findHabit(const String &id) {
  for (int i = 0; i < habCount; i++) if (hab[i].id == id) return i;
  return -1;
}

int ensureHabit(const String &id) {
  int i = findHabit(id);
  if (i >= 0) return i;
  if (habCount >= MAX_HABITS) return -1;
  hab[habCount].id = id;
  hab[habCount].done = false;
  hab[habCount].hh = 0;
  hab[habCount].mm = 0;
  hab[habCount].rt = 0;
  hab[habCount].dt = 0;
  hab[habCount].rtSeqN = 0;
  hab[habCount].minSec = 0;
  for (int k = 0; k < 4; k++) { hab[habCount].rtSeq[k] = 0; hab[habCount].rtSeqMin[k] = 0; }
  return habCount++;
}

void seedDefaults() {
  // 从没联过网时的兜底（和服务器默认的 2 件一致）
  int i = ensureHabit("brush");
  if (i >= 0) { hab[i].hh = 20; hab[i].mm = 30; hab[i].rt = 2; hab[i].dt = 3; }
  i = ensureHabit("toys");
  if (i >= 0) { hab[i].hh = 19; hab[i].mm = 30; hab[i].rt = 4; hab[i].dt = 5; }
}

// 习惯表存 NVS：id,rt,dt,hh,mm;id,rt,dt,hh,mm;
void saveHabits() {
  String s = "";
  for (int i = 0; i < habCount; i++) {
    s += hab[i].id + "," + String(hab[i].rt) + "," + String(hab[i].dt) + ","
       + String(hab[i].hh) + "," + String(hab[i].mm) + ";";
  }
  prefs.putString("habits", s);
}

void loadHabits() {
  String s = prefs.getString("habits", "");
  habCount = 0;
  int pos = 0;
  while (pos < (int)s.length() && habCount < MAX_HABITS) {
    int sep = s.indexOf(';', pos);
    if (sep < 0) break;
    String item = s.substring(pos, sep);
    pos = sep + 1;
    int c1 = item.indexOf(',');
    int c2 = item.indexOf(',', c1 + 1);
    int c3 = item.indexOf(',', c2 + 1);
    int c4 = item.indexOf(',', c3 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) continue;
    hab[habCount].id   = item.substring(0, c1);
    hab[habCount].rt   = item.substring(c1 + 1, c2).toInt();
    hab[habCount].dt   = item.substring(c2 + 1, c3).toInt();
    hab[habCount].hh   = item.substring(c3 + 1, c4).toInt();
    hab[habCount].mm   = item.substring(c4 + 1).toInt();
    hab[habCount].done = false;
    habCount++;
  }
  if (habCount == 0) seedDefaults();
}

bool allDone() {
  if (habCount == 0) return false;
  for (int i = 0; i < habCount; i++) if (!hab[i].done) return false;
  return true;
}

int doneCount() {
  int n = 0;
  for (int i = 0; i < habCount; i++) if (hab[i].done) n++;
  return n;
}

// ======================= 日期 / NVS =======================
String dateKey() {
  struct tm t;
  if (getLocalTime(&t, 50)) {
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return String(buf);
  }
  return todayKey;   // 没网没时间时沿用上次
}

void loadState() {
  prefs.begin("coach", false);
  todayKey = prefs.getString("day", "");
  streak = prefs.getInt("streak", 0);
  queue = prefs.getString("queue", "");
  int mask = prefs.getInt("done", 0);
  loadHabits();
  for (int i = 0; i < habCount; i++) hab[i].done = (mask >> i) & 1;
}

void saveState() {
  int mask = 0;
  for (int i = 0; i < MAX_HABITS; i++) if (i < habCount && hab[i].done) mask |= (1 << i);
  prefs.putString("day", todayKey);
  prefs.putInt("done", mask);
  prefs.putInt("streak", streak);
  prefs.putString("queue", queue);
}

// 跨天：清空今天的进度，连续天数结算
void rollDayIfNeeded() {
  String d = dateKey();
  if (d.length() && d != todayKey) {
    if (todayKey.length()) streak = allDone() ? streak + 1 : 0;
    else streak = 0;
    todayKey = d;
    for (int i = 0; i < MAX_HABITS; i++) { hab[i].done = false; remindedToday[i] = false; }
    selected = 0;
    runIdx = -1;
    saveState();
    needRedraw = true;
    Serial.printf("[day] %s streak=%d habits=%d\n", todayKey.c_str(), streak, habCount);
  }
}

// ======================= 网络 =======================
void queueCheckin(int idx) {
  if (idx < 0 || idx >= habCount) return;
  queue += hab[idx].id + "@" + todayKey + ";";
  saveState();
}

// 极轻量的 POST：设备只报"发生了一件事"，内容由家长补
void postPath(const String &path) {
  if (!wifiOk) return;                    // 断网就只是没记上，不打扰孩子
  HTTPClient http;
  http.begin(apiUrl(path));
  http.setTimeout(4000);
  http.addHeader("Content-Type", "text/plain");
  http.POST("");
  http.end();
}

// URL 查询参数编码：token 可能是 base64（含 + / =），不编码会被服务器解析错。
String urlEncodeComponent(const String &s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// 统一拼设备身份参数：d=设备号（&t=令牌，配了才带）
String deviceQuery() {
  String s = String("d=") + urlEncodeComponent(DEVICE_ID);
  String tk = String(DEVICE_TOKEN);
  if (tk.length()) s += String("&t=") + urlEncodeComponent(tk);
  if (WiFi.status() == WL_CONNECTED) {
    s += String("&rssi=") + String(WiFi.RSSI());
  }
  return s;
}

// 所有设备 API 都必须走这里：少了 d/t，开 DEVICE_TOKEN 后请求会被 401。
String apiUrl(const String &path) {
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + path;
  url += (path.indexOf('?') >= 0) ? "&" : "?";
  url += deviceQuery();
  return url;
}

bool postCheckin(const String &habit) {
  if (!wifiOk) return false;
  HTTPClient http;
  http.begin(apiUrl(String("/api/checkin.txt?habit=") + habit));
  http.setTimeout(2500);
  int code = http.POST("");
  bool ok = (code == 200 || code == 409);   // 409 = 今天已打过
  http.end();
  return ok;
}

void flushQueue() {
  if (!wifiOk || !queue.length()) return;
  while (queue.length()) {
    int sep = queue.indexOf(';');
    if (sep < 0) { queue = ""; break; }
    String item = queue.substring(0, sep);
    String habit = item.substring(0, item.indexOf('@'));
    if (postCheckin(habit)) { queue = queue.substring(sep + 1); saveState(); }
    else break;   // 网络还不稳，下次再试
  }
}

void postAck(int id) {
  if (!wifiOk) return;
  HTTPClient http;
  http.begin(apiUrl(String("/api/ack.txt?id=") + String(id)));
  http.setTimeout(2500);
  http.POST("");
  http.end();
}

// 把 "1,2,0,1,1,0,0" 解析进 weekDone[]
void parseWeek(const String &v) {
  String s = v;
  for (int i = 0; i < 7; i++) {
    int c = s.indexOf(',');
    weekDone[i] = ((c < 0 ? s : s.substring(0, c)).toInt() == 1);
    if (c < 0) break;
    s = s.substring(c + 1);
  }
}

// 解析 H=id|名字|是否完成|完成时间|提醒语音|完成语音|提醒时|提醒分
void parseHabitLine(const String &line) {
  String f[11];                             // 10 个字段：id..语音序列..最短秒数
  int n = 0, pos = 2;                       // 跳过 "H="
  while (n < 11 && pos <= (int)line.length()) {
    int p = line.indexOf('|', pos);
    if (p < 0) { f[n++] = line.substring(pos); break; }
    f[n++] = line.substring(pos, p);
    pos = p + 1;
  }
  if (n < 3) return;
  int idx = ensureHabit(f[0]);
  if (idx < 0) return;
  if (f[2].toInt() == 1) hab[idx].done = true;
  if (n >= 8) {
    hab[idx].rt = f[4].toInt();
    hab[idx].dt = f[5].toInt();
    hab[idx].hh = f[6].toInt();
    hab[idx].mm = f[7].toInt();
  }
  if (n >= 9) {          // 第 9 字段："53@0,43@7,62@12"（语音@第几分钟）
    hab[idx].rtSeqN = 0;
    String seq = f[8];
    while (seq.length() && hab[idx].rtSeqN < 4) {
      int c = seq.indexOf(',');
      String one = (c < 0) ? seq : seq.substring(0, c);
      int at = one.indexOf('@');
      int track = ((at < 0) ? one : one.substring(0, at)).toInt();
      int minute = (at < 0) ? 0 : one.substring(at + 1).toInt();
      if (track > 0) {
        int k = hab[idx].rtSeqN++;
        hab[idx].rtSeq[k] = track;
        hab[idx].rtSeqMin[k] = minute;
      }
      if (c < 0) break;
      seq = seq.substring(c + 1);
    }
  }
  if (n >= 10) hab[idx].minSec = f[9].toInt();
  needRedraw = true;
}

bool syncState() {
  if (!wifiOk) return false;
  HTTPClient http;
  http.begin(apiUrl("/api/state.txt"));
  http.setTimeout(2500);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();

  int pos = 0;
  while (pos < (int)body.length()) {
    int nl = body.indexOf('\n', pos);
    if (nl < 0) nl = body.length();
    String line = body.substring(pos, nl);
    pos = nl + 1;

    if (line.startsWith("V="))          protoVer = line.substring(2).toInt();
    else if (line.startsWith("PACK=")) {
      packVer = line.substring(5);
      // 设备上记一份，将来 OTA 换语音包时据此判断要不要下载
      String have = prefs.getString("pack", "");
      if (have != packVer) {
        Serial.printf("[pack] 服务器语音包 %s（本地 %s）\n",
                      packVer.c_str(), have.length() ? have.c_str() : "未知");
        prefs.putString("pack", packVer);
      }
    }
    else if (line.startsWith("Q="))      cardN = line.substring(2).toInt();
    else if (line.startsWith("WEEK_CAL=")) {
      weekCalValid = (parse_bits7(line.substring(9).c_str(), weekCal) == 7);
    }
    else if (line.startsWith("WEEKDAY=")) {
      int wd = line.substring(8).toInt();
      if (wd >= 1 && wd <= 7) weekday = wd;
    }
    else if (line.startsWith("VOL=")) {
      int v = 0;
      if (parse_volume_pct(line.substring(4).c_str(), v)) {
        volumePct = v;
        applyVolume();
      }
    }
    else if (line.startsWith("WEEK="))  parseWeek(line.substring(5));
    else if (line.startsWith("STREAK=")) streak = line.substring(7).toInt();
    else if (line.startsWith("H="))      parseHabitLine(line);
    else if (line.startsWith("MSG=")) {
      // MSG=<id>|<track>|<who>   track 就是 SD 卡语音编号，直接播
      int p1 = line.indexOf('|');
      int p2 = line.indexOf('|', p1 + 1);
      if (p1 > 0) {
        int mid   = line.substring(4, p1).toInt();
        int track = (p2 > 0) ? line.substring(p1 + 1, p2).toInt() : 0;
        Serial.printf("[net] voice track=%d id=%d\n", track, mid);
        setBanner("MESSAGE FROM FAMILY", 6000);
        say(track > 0 ? track : VOICE_MSG_GENERIC);
        happyFor(5000);
        // 0064 = 问题卡；0101~0199 = 识字卡；0201~0299 = 复习卡（还记得这个字吗）
        if (track == 64 || (track >= 101 && track <= 299)) cardPending = true;
        postAck(mid);
      }
    }
  }
  if (cardPending && cardN > 0 && !cardMode) enterCard(0);
  if (selected >= habCount) selected = 0;
  saveHabits();
  saveState();
  return true;
}

void connectWifi() {
#if USE_WIFI
  if (!strlen(WIFI_SSID)) { Serial.println("[wifi] 未配置，离线运行"); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(150);
  wifiOk = (WiFi.status() == WL_CONNECTED);
  Serial.printf("[wifi] %s ip=%s\n", wifiOk ? "ok" : "fail",
                WiFi.localIP().toString().c_str());
  if (wifiOk) {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com");
    setBanner("ONLINE", 2000);
  }
#endif
}

// ======================= 打卡 =======================
void completeHabit(int idx) {
  if (habCount == 0) { setBanner("NOT READY"); return; }
  if (idx < 0 || idx >= habCount) return;

  if (hab[idx].done) {
    setBanner("ALREADY DONE");
    say(VOICE_THANKS);
    return;
  }
  hab[idx].done = true;
  needRedraw = true;
  happyFor(3000);
  cancelSeq();                 // 提前做完了，后面几句就不再念了
  runIdx = -1;                 // 清掉计时状态

  // 本地先给反馈，再管网络（保证断网也不掉体验）
  playDoneChime();                       // 立刻"叮"：确认收到
  happyFor(3000);
  int r = random(0, 100);
  if (r < EGG_CHANCE_PCT) {              // 低概率：小豆豆说一句自己的事
    delay(350);
    say(VOICE_EGG_BASE + random(1, VOICE_EGG_COUNT + 1));
  } else if (r < EGG_CHANCE_PCT + 4) {   // 更低概率：角色示弱，让他来"教"
    delay(350);
    say(VOICE_TEACH_ME);                 // 0011「你教教我嘛」
    talkFor(4000);
  } else {
    say(hab[idx].dt);
  }

  if (wifiOk) { if (!postCheckin(hab[idx].id)) queueCheckin(idx); }
  else queueCheckin(idx);
  saveState();

  if (allDone()) {
    delay(1200);
    say(VOICE_ALL_DONE);
    happyFor(6000);
    setBanner("ALL DONE!", 6000);
    playCelebration(random(0, 4));      // 星星雨 / 爱心雨 / 图标雨 / 彩纸，随机
    if (random(0, 100) < CHOICE_CHANCE_PCT) {
      delay(700);
      say(VOICE_CHOICE_Q);
      choiceMode = true;
      choiceUntil = millis() + 15000;
      setBanner("A跳舞 B秘密", 12000);
      needRedraw = true;
    }
  } else if (doneCount() == habCount - 1) {
    delay(1200);
    say(VOICE_ONE_LEFT);
  }

  for (int k = 1; k <= habCount; k++) {
    int n = (idx + k) % habCount;
    if (!hab[n].done) { selected = n; break; }
  }

  // 立刻同步一次：把服务器刚自动排的"识字卡"取回来显示
  if (wifiOk) syncState();

  needRedraw = true;
}

// ======================= 1.85 专用：I/O 扩展器 + 触摸 =======================
#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
#define TCA9554_ADDR 0x20

void tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t tcaRead(uint8_t reg) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)TCA9554_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// 官方 1.85C：LCD/触摸复位在 P0/P1；P2/P3 也配成输出并拉高（SD_CS 等）
void tca9554Init() {
  Wire.begin(SYS_SDA, SYS_SCL, 400000);
  tcaWrite(0x03, 0b11110000);   // 配置：P0~P3 全部输出
  tcaWrite(0x01, 0b00000000);   // 先把复位脚全部拉低
  delay(300);
  tcaWrite(0x01, 0b00001111);   // P0~P3 拉高，释放 LCD/触摸复位
  delay(300);
  Serial.printf("[tca9554] cfg=%02X out=%02X\n", tcaRead(0x03), tcaRead(0x01));
}
#endif

#if USE_TOUCH
// 1.85C 的触摸挂在与 TCA9554/RTC 同一条 I2C 上；1.85 是独立总线
#if TP_ON_SYS_BUS
  #define TPWIRE Wire
#else
  #define TPWIRE Wire1
#endif
bool touchHeld = false, touchLongFired = false;
unsigned long touchDownAt = 0;
int touchLastX = 0, touchLastY = 0;

void touchInit() {
#if !TP_ON_SYS_BUS
  Wire1.begin(TP_SDA, TP_SCL, 400000);   // 1.85：独立总线
#endif                                     // 1.85C：Wire 已在 tca9554Init() 里起好
  TPWIRE.beginTransmission(CST816_ADDR);
  TPWIRE.write(0x00);
  TPWIRE.endTransmission(false);
  TPWIRE.requestFrom((int)CST816_ADDR, 1);
  touchReady = TPWIRE.available();
  Serial.printf("[touch] CST816 %s\n", touchReady ? "OK" : "未找到");
}

// CST816 标准寄存器：0x01 Gesture / 0x02 FingerNum / 0x03-0x06 X/Y
#define CST816_REG_GESTURE 0x01
#define CST816_REG_FINGERS 0x02
#define CST816_REG_XH      0x03
#define CST816_REG_XL      0x04
#define CST816_REG_YH      0x05
#define CST816_REG_YL      0x06

// 真机方向不对时只改这三行：交换 XY / 镜像 X / 镜像 Y
#define TP_SWAP_XY  0
#define TP_MIRROR_X 0
#define TP_MIRROR_Y 0

int mapTouchX(int rawX, int rawY) {
#if TP_SWAP_XY
  int v = rawY;
#else
  int v = rawX;
#endif
  return touch_map_axis(v, SCREEN_W, TP_MIRROR_X);
}

int mapTouchY(int rawX, int rawY) {
#if TP_SWAP_XY
  int v = rawX;
#else
  int v = rawY;
#endif
  return touch_map_axis(v, SCREEN_H, TP_MIRROR_Y);
}

bool touchReadRaw(uint8_t buf[6]) {
  if (!touchReady) return false;
  TPWIRE.beginTransmission(CST816_ADDR);
  TPWIRE.write(CST816_REG_GESTURE);
  if (TPWIRE.endTransmission(false) != 0) return false;
  if (TPWIRE.requestFrom((int)CST816_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) buf[i] = TPWIRE.read();
  return true;
}

bool touchReadPoint(int &x, int &y) {
  uint8_t b[6];
  if (!touchReadRaw(b) || b[1] == 0) return false;
  int rawX = ((b[2] & 0x0F) << 8) | b[3];
  int rawY = ((b[4] & 0x0F) << 8) | b[5];
  x = mapTouchX(rawX, rawY);
  y = mapTouchY(rawX, rawY);
  return true;
}

// 轻点 / 长按 → 0 无 / 1 轻点（带坐标）/ 2 长按（带坐标）
int readTouchEvent(int &x, int &y) {
  uint8_t b[6];
  if (!touchReadRaw(b)) return 0;
  bool now = (b[1] > 0);
  if (now) {
    int rawX = ((b[2] & 0x0F) << 8) | b[3];
    int rawY = ((b[4] & 0x0F) << 8) | b[5];
    touchLastX = mapTouchX(rawX, rawY);
    touchLastY = mapTouchY(rawX, rawY);
    if (!touchHeld) {
      touchHeld = true;
      touchDownAt = millis();
      touchLongFired = false;
    } else if (!touchLongFired && millis() - touchDownAt > 900) {
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
#endif

// ======================= 按键 =======================
int readBtn(Btn &b) {
  if (b.pin < 0) return 0;             // 这块板没有按键
  bool cur = digitalRead(b.pin);
  int ev = 0;
  if (cur == LOW && b.last == HIGH) {                       // 按下
    b.pressedAt = millis();
    b.longFired = false;
  } else if (cur == LOW && !b.longFired && millis() - b.pressedAt > 1500) {
    b.longFired = true;
    ev = 2;                                                 // 长按
  } else if (cur == HIGH && b.last == LOW) {                // 抬起
    if (!b.longFired && millis() - b.pressedAt > 30) ev = 1;
  }
  b.last = cur;
  return ev;
}

// ======================= 定时提醒 =======================
void checkReminders() {
#if ENABLE_REMIND
  if (!wifiOk) return;                     // 没网就没有准确时间
  struct tm t;
  if (!getLocalTime(&t, 10)) return;
  for (int i = 0; i < habCount; i++) {
    if (hab[i].done || remindedToday[i]) continue;
    if (hab[i].hh == 0 && hab[i].mm == 0) continue;   // 0:0 = 不提醒
    if (t.tm_hour == hab[i].hh && t.tm_min == hab[i].mm) {
      remindedToday[i] = true;
      selected = i;
      needRedraw = true;
      talkFor(5000);
      if (hab[i].rtSeqN > 0) startSeq(i);   // 流程：一句话一句话说
      else say(hab[i].rt);
      Serial.printf("[remind] habit %d seq=%d\n", i, hab[i].rtSeqN);
    }
  }
#endif
}

// ======================= 串口调试 =======================
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't') { say(1); }
    else if (c == 'r') {
      for (int i = 0; i < MAX_HABITS; i++) { hab[i].done = false; remindedToday[i] = false; }
      streak = 0; queue = ""; saveState(); needRedraw = true;
      Serial.println("已重置今天");
    }
    else if (c == 's') { Serial.println(syncState() ? "同步成功" : "同步失败"); }
    else if (c == 'q') { Serial.println("队列: " + queue); }
    else if (c == 'c') { Serial.printf("cards=%d\n", cardN); enterCard(0); }
    else if (c == 'h') {
      Serial.printf("habCount=%d\n", habCount);
      for (int i = 0; i < habCount; i++)
        Serial.printf("  %s done=%d %02d:%02d rt=%d dt=%d\n",
                      hab[i].id.c_str(), hab[i].done, hab[i].hh, hab[i].mm,
                      hab[i].rt, hab[i].dt);
    }
    else if (c == '1') { say(1); } else if (c == '2') { say(2); }
    else if (c == '3') { say(3); } else if (c == '8') { say(8); }
    else if (c == 'T') {                    // 触摸原始数据（核对坐标轴/寄存器）
#if USE_TOUCH
      uint8_t b[6];
      if (touchReadRaw(b)) {
        Serial.printf("[touch] raw %02X %02X %02X %02X %02X %02X\n",
                      b[0], b[1], b[2], b[3], b[4], b[5]);
      } else {
        Serial.println("[touch] no data");
      }
#else
      Serial.println("[touch] not enabled");
#endif
    }
    else if (c == 'p') {                    // 四幕自检：MORNING→DAY→EVENING→NIGHT
      currentPhase = (Phase)(((int)currentPhase + 1) & 3);
      phaseChangedAt = millis();
      nightMode = (currentPhase == PHASE_NIGHT);
      setBacklightPct(palette_for_phase(currentPhase).brightness);
      applyVolume();
      needRedraw = true;
      Serial.printf("[phase] %s brightness=%d\n", phase_name(currentPhase),
                    palette_for_phase(currentPhase).brightness);
    }
    else if (c == 'd') {                    // 屏幕自检：红/绿/蓝/白/黑
      const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
      const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
      for (uint8_t round = 0; round < 3; round++) {
        for (uint8_t i = 0; i < 5; i++) {
          ui->fillScreen(colors[i]);
          if (canvasOk) canvas->flush();
          Serial.printf("display %s\n", names[i]);
          delay(1000);
        }
      }
      needRedraw = true;
      Serial.println("display test done");
    }
    else if (c == 'D') {                    // 绕过 canvas，直接写屏
      const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
      const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
      for (uint8_t round = 0; round < 3; round++) {
        for (uint8_t i = 0; i < 5; i++) {
          gfx->fillScreen(colors[i]);
          Serial.printf("direct %s\n", names[i]);
          delay(1000);
        }
      }
      needRedraw = true;
      Serial.println("direct test done");
    }
    else if (c == 'G') {                    // 几何测试：看坐标是否错位/重复
      gfx->fillScreen(0x0000);
      gfx->drawRect(0, 0, SCREEN_W, SCREEN_H, 0xFFFF);
      gfx->fillRect(0, 0, 60, 60, 0xF800);       // 红
      gfx->fillRect(60, 0, 60, 60, 0x07E0);      // 绿
      gfx->fillRect(120, 0, 60, 60, 0x001F);     // 蓝
      gfx->fillRect(180, 0, 60, 60, 0xFFE0);     // 黄
      gfx->fillRect(0, 60, 60, 60, 0xF81F);      // 品红
      gfx->fillRect(0, 120, 60, 60, 0x07FF);     // 青
      gfx->drawLine(0, 0, SCREEN_W - 1, SCREEN_H - 1, 0xFFFF);
      gfx->drawLine(0, SCREEN_H - 1, SCREEN_W - 1, 0, 0xFFFF);
      Serial.println("geometry test drawn");
      delay(7000);
      needRedraw = true;
    }
  }
}

// ======================= 背光 / 四幕 =======================
// core 2.x 的 LEDC 按通道号；通道 1 给背光，通道 0 留给蜂鸣器（240 板）
void backlightInit() {
#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  pinMode(LCD_BL, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LCD_BL, 5000, 8);
#else
  ledcSetup(1, 5000, 8);
  ledcAttachPin(LCD_BL, 1);
#endif
#else
  pinMode(TFT_BL, OUTPUT);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(1, 5000, 8);
  ledcAttachPin(TFT_BL, 1);
#endif
#endif
}

void setBacklightPct(int pct) {
  int duty = backlight_duty(pct);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  ledcWrite(LCD_BL, duty);
  #else
  ledcWrite(TFT_BL, duty);
  #endif
#else
  ledcWrite(1, duty);
#endif
}

void updatePhase(bool force) {
  struct tm t;
  if (!getLocalTime(&t, 10)) return;     // 没时间：保持当前时段（冷启动=夜间）
  Phase p = phase_for_time(t.tm_hour, t.tm_min);
  if (!force && p == currentPhase) return;
  if (p != currentPhase) phaseChangedAt = millis();
  currentPhase = p;
  nightMode = (p == PHASE_NIGHT);
  setBacklightPct(palette_for_phase(p).brightness);
  applyVolume();                         // 夜间音量压到 60%
  needRedraw = true;
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  delay(200);

#if HAS_BUTTONS
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
#endif
#if BUZZER >= 0
  pinMode(BUZZER, OUTPUT);
#endif
  buzzerInit();

#if BOARD == BOARD_WS_1_85 || BOARD == BOARD_WS_1_85C
  tca9554Init();                 // 必须先放 LCD_RST / TP_RST，否则屏不亮
#endif
  backlightInit();
  setBacklightPct(20);           // 冷启动先夜间亮度；NTP 成功后 updatePhase() 切时段

  bool gfxOk = gfx->begin(40000000);   // 先降到 40 MHz，排除 QSPI 时序/信号问题
  Serial.printf("[ui] gfx %s\n", gfxOk ? "OK" : "FAIL");
  canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
  // gfx 已经初始化过 QSPI；Canvas 默认会再调一次 gfx->begin()，
  // ESP32 QSPI 第二次 spi_bus_initialize 会 ESP_ERROR_CHECK abort。
  canvasOk = canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  ui = canvasOk ? (Arduino_GFX *)canvas : gfx;
  Serial.printf("[ui] canvas %s\n", canvasOk ? "OK" : "FAIL(直接写屏)");

#if USE_TOUCH
  touchInit();
#endif
  audioInit();

  loadState();
  rollDayIfNeeded();
  render();

#if USE_DFPLAYER
  Serial1.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
  delay(1200);                       // DFPlayer 上电需要时间
  mp3Ok = mp3.begin(Serial1);
  if (mp3Ok) {
    mp3.volume(22);
    applyVolume();
    mp3.EQ(DFPLAYER_EQ_NORMAL);
    Serial.println("[mp3] DFPlayer OK");
  } else {
    Serial.println("[mp3] 未找到，用蜂鸣器代替");
  }
#endif

  connectWifi();
  if (wifiOk) { syncState(); flushQueue(); }
  updatePhase(true);

  say(VOICE_BOOT);
  happyFor(2500);
  render();
}

void loop() {
  // 1. 跨天检查
  static unsigned long lastDayCheck = 0;
  if (millis() - lastDayCheck > 5000) { lastDayCheck = millis(); rollDayIfNeeded(); }

  // 2b. 输入：按键（DevKitC）或触摸（1.85）
  int a = readBtn(btnA);
  int b = readBtn(btnB);
  // 2a. 二选一时刻（优先级最高，吃掉其它输入）
  if (choiceMode) {
    if (millis() > choiceUntil) { choiceMode = false; needRedraw = true; }
    else if (a == 1) {                 // 跳舞
      choiceMode = false;
      say(VOICE_CHOICE_A);
      playCelebration(random(0, 4));
    } else if (b == 1) {               // 听秘密
      choiceMode = false;
      say(VOICE_CHOICE_B);
      delay(1500);
      say(VOICE_EGG_BASE + random(1, VOICE_EGG_COUNT + 1));
    }
    if (choiceMode == false) { needRedraw = true; }
    delay(20);
    return;
  }

#if USE_TOUCH
  {
    int tx = 0, ty = 0;
    int t = readTouchEvent(tx, ty);
    if (cardMode) {
      if (t == 2) a = 2;                // 长按退出卡片
      else if (t == 1) b = 1;           // 轻点翻页
    } else if (t == 1) {
      int i = hitHabit(tx, ty);
      if (i >= 0) {
        selected = i;
        animateStickerBounce(i);
        if (hab[i].minSec > 0 && !hab[i].done) {
          runIdx = i;
          runStart = millis();
          runDur = (unsigned long)hab[i].minSec * 1000UL;
          lastBarMs = 0;
          say(17);
          talkFor(2500);
          needRedraw = true;
        } else {
          completeHabit(i);
        }
      } else if (hitSun(tx, ty)) {
        if (habCount > 0 && allDone()) {
          say(VOICE_TELL_PARENTS);
          postPath("/api/child_note");
          talkFor(4000);
          setBanner("TELL PARENTS", 4000);
        } else {
          say(VOICE_Q_SAVED);
          postPath("/api/question_quick");
          talkFor(4000);
          setBanner("QUESTION SAVED", 4000);
        }
      }
    } else if (t == 2) {                // 长按主界面：立即同步
      syncState();
      flushQueue();
      setBanner("SYNCED", 2000);
    }
  }
#endif

  if (cardMode) {
    if (a == 1) {                       // A：退出卡片
      cardMode = false; cardPending = false;
      needRedraw = true;
    } else if (b == 1) {                // B：翻页
      int n = cardIdx + 1;
      if (n < cardN) { if (fetchCard(n)) { cardIdx = n; renderCard(); } }
      else { cardMode = false; cardPending = false; needRedraw = true; }
    }
  } else {
  if (b == 1 && habCount > 0) {
    if (runIdx >= 0) { runIdx = -1; needRedraw = true; }
    selected = (selected + 1) % habCount;
    needRedraw = true;
    beep(1200, 40);
  }
  if (a == 2) {                       // 长按：记录
    // 全部做完了 → 给爸妈留句话；否则 → 记一个问题
    if (habCount > 0 && allDone()) {
      say(VOICE_TELL_PARENTS);
      postPath("/api/child_note");
      talkFor(4000);
      setBanner("告诉爸妈了", 4000);
    } else {
      say(VOICE_Q_SAVED);
      postPath("/api/question_quick");
      talkFor(4000);
      setBanner("记下来了", 4000);
    }
  }
  if (a == 1) {
    if (runIdx >= 0) {                       // 计时中再按 = 取消
      runIdx = -1;
      beep(700, 90);
      setBanner("CANCELLED", 1500);
      needRedraw = true;
    } else if (selected < habCount && !hab[selected].done && hab[selected].minSec > 0) {
      // 有最短时长的习惯：先计时，时间到才亮星
      runIdx = selected;
      runStart = millis();
      runDur = (unsigned long)hab[selected].minSec * 1000UL;
      lastBarMs = 0;
      say(17);                               // 0017「一、二、三，开始！」
      talkFor(2500);
      needRedraw = true;
    } else {
      completeHabit(selected);
    }
  }
  if (b == 2) {                       // 长按 B：立即同步
    syncState();
    flushQueue();
    setBanner("SYNCED", 2000);
  }
  }

  // 3. 联网状态维护
  if (millis() - lastWifiTry > 30000) {
    lastWifiTry = millis();
    if (!wifiOk && strlen(WIFI_SSID)) connectWifi();
  }
  if (wifiOk && millis() - lastSync > 15000) {
    lastSync = millis();
    syncState();
    flushQueue();
    needRedraw = true;
  }

  // 3b. 四幕时段（每 30 秒；无时间时保持当前时段）
  if (millis() - lastPhaseCheck > 30000) {
    lastPhaseCheck = millis();
    updatePhase(false);
  }

  // 4. 提醒
  checkReminders();

  // 4.2 倒计时推进
  if (runIdx >= 0) {
    if (millis() - runStart >= runDur) {
      int done = runIdx;
      runIdx = -1;
      completeHabit(done);                   // 时间到 → 这才算完成
    } else if (millis() - lastBarMs > 200) {
      lastBarMs = millis();
      needRedraw = true;                     // 每 200ms 刷一次进度条
    }
  }

  // 4.5 流程引导语音
  if (seqIdx < seqN && millis() >= seqAt[seqIdx]) {
    say(seqBuf[seqIdx]);
    seqIdx++;
  }

  // 5. 表情回落 + 眨眼
  if (exprUntil && millis() > exprUntil) { expr = 0; exprUntil = 0; needRedraw = true; }
  if (banner.length() && millis() > lastBannerClear) { banner = ""; needRedraw = true; }
  if (millis() - lastBlink > 4200) {
    lastBlink = millis();
    if (expr == 0) { expr = 2; exprUntil = millis() + 130; needRedraw = true; }
  }

  // 6. 音频泵（I2S 播放需要持续喂数据）+ 串口
  audioLoop();
  handleSerial();

  // 7. 重绘
  if (needRedraw) render();
  delay(20);
}
