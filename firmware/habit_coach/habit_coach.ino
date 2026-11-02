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
#include <esp_heap_caps.h>
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
#define BOARD_WS_1_85        1

#ifndef BOARD
#define BOARD BOARD_DEVKITC_GC9A01
#endif

// ======================= 功能开关 =======================
#define USE_WIFI       1   // 设 0 则完全离线
#define ENABLE_REMIND  1   // 定时语音提醒

#if BOARD == BOARD_WS_1_85
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

// 布局缩放：所有绘图坐标以 240 为设计基准，乘这个系数适配 360
#define UI_K ((float)SCREEN_W / 240.0f)
#define K(v) ((int)((v) * UI_K))

// ======================= 引脚 =======================
#if BOARD == BOARD_WS_1_85
  // 屏幕 ST77916（QSPI）
  #define LCD_SDA0  46
  #define LCD_SDA1  45
  #define LCD_SDA2  42
  #define LCD_SDA3  41
  #define LCD_SCK   40
  #define LCD_CS    21
  #define LCD_BL     5
  #define LCD_RST_EXIO 2      // 挂在 TCA9554 的 P2 上，不是 GPIO
  // 触摸 CST816（独立 I2C 总线）
  #define TP_SDA     1
  #define TP_SCL     3
  #define TP_INT     4
  #define TP_RST_EXIO 1       // TCA9554 P1
  // 系统 I2C（TCA9554 / RTC / IMU 共用）
  #define SYS_SDA   10
  #define SYS_SCL   11
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
#if BOARD == BOARD_WS_1_85
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_SDA0, LCD_SDA1, LCD_SDA2, LCD_SDA3);
Arduino_GFX *gfx = new Arduino_ST77916(bus, GFX_NOT_DEFINED /* RST 走 TCA9554 */, 0 /* rotation */,
                                       true /* IPS */, SCREEN_W, SCREEN_H);
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

// 问题卡模式：设备不解析中文，只把服务器画好的 240x240 RGB565 贴到屏上
bool  cardMode = false;
bool  cardPending = false;
int   cardN = 0, cardIdx = 0;
uint16_t *cardBuf = nullptr;

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

// ======================= 调色板 =======================
#define C_BG       0x0861   // 深夜蓝黑
#define C_FACE     0xFE60   // 暖黄
#define C_INK      0x18E3
#define C_STAR_ON  0xFFE0
#define C_STAR_OFF 0x2945
#define C_TXT      0xC618
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
bool weekDone[7] = {false, false, false, false, false, false, false};  // 近 7 天是否完成
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
void say(int track);

// ======================= 音频 =======================
#if USE_SD_AUDIO
#include <SD_MMC.h>
#if __has_include(<Audio.h>)
#include <Audio.h>
Audio audio;
#endif
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
#if __has_include(<Audio.h>)
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[audio] TF 卡挂载失败（确认卡是 FAT32，里面有 mp3/ 目录）");
    return;
  }
  audio.setPinout(I2S_BCK, I2S_LRCK, I2S_DIN);
  audio.setVolume(12);          // 0-21；孩子房间建议 8-12
  audioOk = true;
  Serial.println("[audio] TF + I2S OK");
#else
  Serial.println("[audio] 没装 ESP32-audioI2S 库，跳过");
#endif
}

void audioLoop() {
#if __has_include(<Audio.h>)
  if (audioOk) audio.loop();
#endif
}
#else
void audioInit() {}
void audioLoop() {}
#endif

void say(int track) {
#if USE_SD_AUDIO
#if __has_include(<Audio.h>)
  if (audioOk && track > 0) {
    char path[24];
    snprintf(path, sizeof(path), "/mp3/%04d.mp3", track);
    audio.connecttoFS(SD_MMC, path);
    return;
  }
#endif
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

// ======================= 绘制 =======================
// 设计基准是 240×240，所有坐标乘 K() 适配 360×360
void drawEyes(int mode) {
  int lx = K(88), rx = K(152), ey = K(92);
  int e = K(14), hi = K(4);
  // 眨眼：一只闭一只圆
  if (mode == EXPR_WINK) {
    ui->drawLine(lx - e, ey, lx + e, ey, C_INK);
    ui->fillCircle(rx, ey, K(12), C_INK);
    ui->fillCircle(rx + hi, ey - hi, hi, C_FACE);
    return;
  }
  // 好奇：一大一小
  if (mode == EXPR_CURIOUS) {
    ui->fillCircle(lx, ey, K(14), C_INK);
    ui->fillCircle(rx, ey, K(8), C_INK);
    ui->fillCircle(lx + hi, ey - hi, hi, C_FACE);
    ui->fillCircle(rx + K(2), ey - K(2), K(3), C_FACE);
    return;
  }
  // 惊讶：更大的圆眼 + 高光偏上
  if (mode == EXPR_SURPRISED) {
    ui->fillCircle(lx, ey, K(15), C_INK);
    ui->fillCircle(rx, ey, K(15), C_INK);
    ui->fillCircle(lx + hi, ey - K(6), hi, C_FACE);
    ui->fillCircle(rx + hi, ey - K(6), hi, C_FACE);
    return;
  }
  if (mode == 2 || mode == EXPR_SLEEPY) {
    ui->drawLine(lx - e, ey, lx + e, ey, C_INK);
    ui->drawLine(rx - e, ey, rx + e, ey, C_INK);
    return;
  }
  if (mode == 1 || mode == EXPR_HAPPY) {
    ui->drawLine(lx - e, ey + K(6), lx, ey - K(8), C_INK);
    ui->drawLine(lx, ey - K(8), lx + e, ey + K(6), C_INK);
    ui->drawLine(rx - e, ey + K(6), rx, ey - K(8), C_INK);
    ui->drawLine(rx, ey - K(8), rx + e, ey + K(6), C_INK);
    return;
  }
  ui->fillCircle(lx, ey, K(12), C_INK);
  ui->fillCircle(rx, ey, K(12), C_INK);
  ui->fillCircle(lx + hi, ey - hi, hi, C_FACE);
  ui->fillCircle(rx + hi, ey - hi, hi, C_FACE);
}

void drawMouth(int mode) {
  int cx = SCREEN_W / 2, my = K(132);
  // 惊讶：小圆嘴
  if (mode == EXPR_SURPRISED) { ui->fillCircle(cx, my + K(2), K(9), C_INK); return; }
  // 好奇：歪嘴
  if (mode == EXPR_CURIOUS) {
    ui->drawLine(cx - K(10), my + K(4), cx + K(2), my - K(2), C_INK);
    ui->drawLine(cx + K(2), my - K(2), cx + K(13), my + K(3), C_INK);
    return;
  }
  // 困：小弧 + zz
  if (mode == EXPR_SLEEPY) {
    ui->drawLine(cx - K(8), my + K(3), cx, my - K(2), C_INK);
    ui->drawLine(cx, my - K(2), cx + K(8), my + K(3), C_INK);
    ui->setTextColor(C_ACCENT);
    ui->setTextSize(1);
    ui->setCursor(cx + K(28), my - K(26));
    ui->print("z z");
    return;
  }
  if (mode == 1 || mode == EXPR_HAPPY) {
    ui->fillCircle(cx, my, K(20), C_INK);
    ui->fillCircle(cx, my - K(8), K(20), C_FACE);
    return;
  }
  if (mode == 3) { ui->fillCircle(cx, my + K(2), K(8), C_INK); return; }
  ui->drawLine(cx - K(16), my, cx - K(6), my + K(7), C_INK);
  ui->drawLine(cx - K(6), my + K(7), cx + K(6), my + K(7), C_INK);
  ui->drawLine(cx + K(6), my + K(7), cx + K(16), my, C_INK);
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

void drawHabitIcon(int cx, int cy, const String &id, bool done, bool sel) {
  int rBg = K(24);
  ui->fillCircle(cx, cy, rBg, 0x10A2);

  int idx = iconIndexOf(id);
  if (idx >= 0) {
    ui->draw16bitRGBBitmap(cx - HABIT_ICON_W / 2, cy - HABIT_ICON_H / 2,
                           (const uint16_t *)HABIT_ICON_PIX[idx], HABIT_ICON_W, HABIT_ICON_H);
  } else {
    drawStar(cx, cy, K(16), done, false);
  }

  if (done) {
    ui->drawCircle(cx, cy, rBg, C_STAR_ON);
    ui->drawCircle(cx, cy, rBg - 1, C_STAR_ON);
    ui->fillCircle(cx + K(17), cy + K(17), K(8), C_STAR_ON);
    ui->drawLine(cx + K(13), cy + K(17), cx + K(16), cy + K(21), C_INK);
    ui->drawLine(cx + K(16), cy + K(21), cx + K(21), cy + K(13), C_INK);
  } else {
    ui->drawCircle(cx, cy, rBg, C_STAR_OFF);
  }

  if (sel) ui->drawCircle(cx, cy, rBg + K(5), C_ACCENT);
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

  for (int f = 0; f < 42; f++) {
    ui->fillScreen(C_BG);
    drawEyes(EXPR_HAPPY);
    drawMouth(EXPR_HAPPY);
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
  needRedraw = true;
}

void render() {
  if (cardMode) { renderCard(); return; }
  ui->fillScreen(C_BG);

  // 顶部：本周 7 个格子
  int dw = K(24), dx0 = SCREEN_W / 2 - 3 * dw, topY = K(34), dotR = K(9);
  for (int i = 0; i < 7; i++) {
    int x = dx0 + i * dw;
    if (weekDone[i]) ui->fillCircle(x, topY, dotR, C_STAR_ON);
    else             ui->drawCircle(x, topY, dotR, C_STAR_OFF);
  }
  ui->fillCircle(K(24), topY, K(4), wifiOk ? 0x07E0 : 0x4208);

  drawEyes(expr);
  drawMouth(expr);

  // 今天要做的事：每个习惯一个 emoji 图标
  if (habCount > 0) {
    int usable = SCREEN_W - 2 * K(34);
    int gap = (habCount > 1) ? (usable / habCount) : 0;
    int startX = SCREEN_W / 2 - (habCount - 1) * gap / 2;
    for (int i = 0; i < habCount; i++) {
      drawHabitIcon(startX + i * gap, K(176), hab[i].id, hab[i].done, (i == selected));
    }
  } else {
    ui->setTextColor(C_TXT);
    ui->setTextSize(2);
    ui->setCursor(K(46), K(172));
    ui->print("SYNCING");
  }

  // 计时进度条
  if (runIdx >= 0 && runDur > 0) {
    int w = K(160), x0 = SCREEN_W / 2 - w / 2, yb = K(205), hb = K(10);
    ui->drawRect(x0, yb, w, hb, C_STAR_OFF);
    unsigned long e = millis() - runStart;
    int f = (int)((e >= runDur) ? w - 2 : (e * (unsigned long)(w - 2)) / runDur);
    if (f > 0) ui->fillRect(x0 + 1, yb + 1, f, hb - 2, C_STAR_ON);
  }

  if (banner.length()) {
    ui->setTextSize(1);
    ui->setTextColor(C_ACCENT);
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
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT +
               "/api/card.rgb565?i=" + String(idx) + "&s=" + String(SCREEN_W) +
               "&d=" + DEVICE_ID;
  http.begin(url);
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

void setBanner(const String &s, unsigned long ms = 4000) {
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
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + path;
  http.begin(url);
  http.setTimeout(4000);
  http.addHeader("Content-Type", "text/plain");
  http.POST("");
  http.end();
}

// 统一拼设备身份参数：d=设备号（&t=令牌，配了才带）
String deviceQuery() {
  String s = String("?d=") + DEVICE_ID;
  String tk = String(DEVICE_TOKEN);
  if (tk.length()) s += String("&t=") + tk;
  return s;
}

bool postCheckin(const String &habit) {
  if (!wifiOk) return false;
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT +
               "/api/checkin.txt?habit=" + habit + "&d=" + DEVICE_ID;
  http.begin(url);
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
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT +
               "/api/ack.txt?id=" + String(id);
  http.begin(url);
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
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/state.txt"
               + deviceQuery();
  http.begin(url);
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
#if BOARD == BOARD_WS_1_85
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

// LCD_RST=EXIO2 / TP_RST=EXIO1 / SD_CS=EXIO3 都挂在这颗扩展器上
void tca9554Init() {
  Wire.begin(SYS_SDA, SYS_SCL, 400000);
  tcaWrite(0x03, 0b11110001);   // 配置：P1/P2/P3 输出，其余输入
  tcaWrite(0x01, 0b00000000);   // 先把复位脚拉低
  delay(20);
  tcaWrite(0x01, 0b00001110);   // TP_RST / LCD_RST / SD_CS 拉高
  delay(120);
  Serial.printf("[tca9554] cfg=%02X out=%02X\n", tcaRead(0x03), tcaRead(0x01));
}
#endif

#if USE_TOUCH
bool touchHeld = false, touchLongFired = false;
unsigned long touchDownAt = 0;

void touchInit() {
  Wire1.begin(TP_SDA, TP_SCL, 400000);
  Wire1.beginTransmission(CST816_ADDR);
  Wire1.write(0x00);
  Wire1.endTransmission(false);
  Wire1.requestFrom((int)CST816_ADDR, 1);
  touchReady = Wire1.available();
  Serial.printf("[touch] CST816 %s\n", touchReady ? "OK" : "未找到");
}

int touchGesture() {
  if (!touchReady) return 0;
  Wire1.beginTransmission(CST816_ADDR);
  Wire1.write(0x01);
  Wire1.endTransmission(false);
  Wire1.requestFrom((int)CST816_ADDR, 1);
  return Wire1.available() ? Wire1.read() : 0;
}

// 轻点 / 长按 → 映射成和按键一样的事件（0 无 / 1 短 / 2 长）
int readTouch() {
  int g = touchGesture();
  bool now = (g != 0);
  if (now && !touchHeld) {
    touchHeld = true;
    touchDownAt = millis();
    touchLongFired = false;
  } else if (now && !touchLongFired && millis() - touchDownAt > 900) {
    touchLongFired = true;
    return 2;
  } else if (!now && touchHeld) {
    touchHeld = false;
    if (!touchLongFired) return 1;
  }
  return 0;
}
#endif

// ======================= 按键 =======================
struct Btn { uint8_t pin; bool last; unsigned long pressedAt; bool longFired; };
Btn btnA = {BTN_A, HIGH, 0, false};
Btn btnB = {BTN_B, HIGH, 0, false};

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
  }
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

#if BOARD == BOARD_WS_1_85
  tca9554Init();                 // 必须先放 LCD_RST / TP_RST，否则屏不亮
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#else
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  gfx->begin();
  canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
  canvasOk = canvas->begin();
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
    mp3.EQ(DFPLAYER_EQ_NORMAL);
    Serial.println("[mp3] DFPlayer OK");
  } else {
    Serial.println("[mp3] 未找到，用蜂鸣器代替");
  }
#endif

  connectWifi();
  if (wifiOk) { syncState(); flushQueue(); }

  say(VOICE_BOOT);
  happyFor(2500);
  render();
}

void loop() {
  // 1. 跨天检查
  static unsigned long lastDayCheck = 0;
  if (millis() - lastDayCheck > 5000) { lastDayCheck = millis(); rollDayIfNeeded(); }

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

  // 2b. 输入：按键（DevKitC）或触摸（1.85）
  int a = readBtn(btnA);
  int b = readBtn(btnB);
#if USE_TOUCH
  {
    int t = readTouch();
    if (t == 2) {                       // 长按
      if (cardMode) a = 2; else b = 2;
    } else if (t == 1) {                // 轻点
      if (cardMode) b = 1; else a = 1;  // 卡片里轻点翻页；主界面轻点=完成
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
