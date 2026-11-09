/*
 * Step 1 · 点屏测试（只接屏幕，别的都别接）
 * 通过标准：屏上出现彩色圆环 + DISPLAY OK，颜色每 2 秒循环
 *
 * 库：Arduino_GFX_Library (>= 1.6.5)
 * 接线：SCL=12  SDA=11  CS=9  DC=10  RST=18  BL=7  VCC=3V3  GND=GND
 */
#include <Arduino_GFX_Library.h>

#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_CS     9
#define TFT_DC    10
#define TFT_RST   18
#define TFT_BL     7

// Arduino_GFX 新版不再导出颜色宏，自己补
#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef WHITE
#define WHITE 0xFFFF
#endif
#ifndef RED
#define RED 0xF800
#endif
#ifndef GREEN
#define GREEN 0x07E0
#endif
#ifndef BLUE
#define BLUE 0x001F
#endif
#ifndef YELLOW
#define YELLOW 0xFFE0
#endif

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0, true);

const uint16_t colors[] = {RED, GREEN, BLUE, YELLOW, 0xFE60, 0xFD20};
int idx = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[1_display] 开始点屏");

  gfx->begin();
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);          // 背光！漏了就是黑屏

  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(40, 60);
  gfx->print("DISPLAY");
  gfx->setCursor(72, 100);
  gfx->print("OK");
  gfx->setTextSize(1);
  gfx->setCursor(60, 140);
  gfx->print("GC9A01 240x240");
  Serial.println("[1_display] 如果屏上能看到 DISPLAY OK，说明屏幕通了");
}

void loop() {
  gfx->fillScreen(BLACK);
  for (int r = 20; r < 120; r += 8) {
    gfx->drawCircle(120, 120, r, colors[(idx + r / 8) % 6]);
  }
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(78, 112);
  gfx->print("OK");
  idx++;
  delay(2000);
}
