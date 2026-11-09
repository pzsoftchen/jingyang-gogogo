/*
 * Step 0 · 板子验真（到货第一件事）
 * 目的：确认卖家发的是不是真的 ESP32-S3 N16R8
 *       16MB Flash + 8MB(八线) PSRAM —— 这是这个项目能跑的前提
 *
 * 通过标准（串口 115200）：
 *   Flash : 16 MB
 *   PSRAM : 8.0 MB 左右
 *   115KB 一次分配：OK
 * 如果 PSRAM 是 0 或 2MB，或者 115KB 分配失败 → 板子不对，直接退
 */
#include <Arduino.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("========== 板子验真 ==========");

  Serial.printf("芯片型号 : %s  rev %d  核心 %d\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("主频     : %d MHz\n", getCpuFrequencyMhz());

  uint32_t flash = ESP.getFlashChipSize();
  Serial.printf("Flash    : %u bytes  (%.0f MB)  %s\n",
                flash, flash / 1048576.0,
                (flash >= 15 * 1048576) ? "✓" : "✗ 不是 16MB");

  size_t psram = ESP.getPsramSize();
  Serial.printf("PSRAM    : %u bytes  (%.1f MB)  %s\n",
                psram, psram / 1048576.0,
                (psram >= 7 * 1048576) ? "✓" : "✗ 需要 8MB，检查是不是买了 N8R2");

  Serial.printf("空闲内存 : heap %u / psram %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

  // 真实分配测试：卡片缓冲就是 115200 字节
  void *p = heap_caps_malloc(115200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    Serial.println("115KB PSRAM 分配 : OK  ✓ 可以跑卡片模式");
    heap_caps_free(p);
  } else {
    void *q = heap_caps_malloc(115200, MALLOC_CAP_8BIT);
    Serial.printf("115KB PSRAM 分配 : FAIL %s\n",
                  q ? "（但内部 RAM 能分配，会很紧张）" : "");
    if (q) heap_caps_free(q);
  }

#if defined(ESP_ARDUINO_VERSION_MAJOR)
  Serial.printf("Arduino core : %d.%d.%d\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
#endif

  Serial.println();
  Serial.println("对照清单：");
  Serial.println("  Flash 16MB + PSRAM 8MB + 115KB OK  → 板子没问题，可以往下走");
  Serial.println("  其他情况 → 联系卖家换货，别浪费时间调代码");
  Serial.println("==============================");
}

void loop() {
  delay(10000);
  Serial.printf("[alive] free heap=%u psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}
