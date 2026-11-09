/*
 * Step 3 · 网络测试（屏幕/语音都可以先不接）
 * 通过标准：串口打印本机 IP，然后打印出服务器返回的 DATE= / STREAK= / H=...
 *
 * 准备：
 *   1. 把这个目录里的 secrets.h.example 复制成 secrets.h 并填好
 *   2. 电脑上先跑 python3 server/server.py
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "缺少 secrets.h —— 请把 secrets.h.example 复制成 secrets.h 再填内容"
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[3_net] 连接 %s ...\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // 关省电，避免请求偶发超时
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[3_net] 连接失败");
    Serial.println("  1) ESP32 只支持 2.4GHz，你连的是 5G 吗？");
    Serial.println("  2) 密码对不对？");
    return;
  }
  Serial.printf("[3_net] 已连接，IP = %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[3_net] 服务器 = %s:%d\n", SERVER_HOST, SERVER_PORT);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { delay(1000); return; }

  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/state.txt";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[3_net] 请求失败 code=%d\n", code);
    Serial.println("  1) SERVER_HOST 是不是电脑的局域网 IP？");
    Serial.println("  2) server/server.py 在跑吗？");
    Serial.println("  3) macOS 防火墙允许 Python 接入吗？");
    http.end();
    delay(5000);
    return;
  }

  Serial.println("[3_net] ---- 服务器返回 ----");
  Serial.print(http.getString());
  Serial.println("[3_net] --------------------");
  http.end();
  delay(5000);
}
