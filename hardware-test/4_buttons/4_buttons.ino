/*
 * Step 4 · 按键 + 蜂鸣器测试
 * 通过标准：按 A/B 有"嘀"声，串口打印 A down / B down
 *
 * 接线：按键 A: GPIO4 ↔ GND  按键 B: GPIO5 ↔ GND
 *       蜂鸣器 +: GPIO6   −: GND
 * 说明：用 INPUT_PULLUP，按下是 LOW，所以按键另一端接 GND
 */
#include <Arduino.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#define BTN_A   4
#define BTN_B   5
#define BUZZER  6

void buzzerInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER, 2000, 10);
#else
  ledcSetup(0, 2000, 10);
  ledcAttachPin(BUZZER, 0);
#endif
}

void beep(uint16_t freq, uint16_t ms) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER, freq); delay(ms);
  ledcWriteTone(BUZZER, 0);    ledcWrite(BUZZER, 0);
#else
  ledcWriteTone(0, freq); delay(ms);
  ledcWriteTone(0, 0);    ledcWrite(0, 0);
#endif
}

bool lastA = HIGH, lastB = HIGH;

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  buzzerInit();
  Serial.println("\n[4_buttons] 按 A 或 B 试试");
  beep(1800, 80);
  delay(120);
  beep(2400, 80);
}

void loop() {
  bool a = digitalRead(BTN_A);
  bool b = digitalRead(BTN_B);

  if (a == LOW && lastA == HIGH) { Serial.println("[4_buttons] A down"); beep(1200, 60); }
  if (a == HIGH && lastA == LOW) { Serial.println("[4_buttons] A up"); }
  if (b == LOW && lastB == HIGH) { Serial.println("[4_buttons] B down"); beep(1600, 60); }
  if (b == HIGH && lastB == LOW) { Serial.println("[4_buttons] B up"); }

  lastA = a; lastB = b;
  delay(15);
}
