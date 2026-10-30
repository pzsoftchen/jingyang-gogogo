/*
 * Step 2 · 语音模块测试（只接 DFPlayer，屏幕可以先拔掉）
 * 通过标准：串口打印 DFPlayer OK，每 3 秒播一句（0001 → 0005）
 *
 * 库：DFRobotDFPlayerMini
 * 接线：ESP GPIO16 ← DFPlayer TX
 *       ESP GPIO17 → DFPlayer RX（中间串 1kΩ）
 *       DFPlayer VCC=5V  GND=GND  喇叭接 SPK1/SPK2
 * SD 卡：根目录下建 mp3/ 文件夹，里面放 0001.mp3 ...（四位数字！）
 */
#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>

#define MP3_RX 16    // ESP 接收 ← 模块 TX
#define MP3_TX 17    // ESP 发送 → 模块 RX（串 1k）

DFRobotDFPlayerMini mp3;
bool ok = false;
int track = 1;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[2_mp3] 等模块上电...");

  Serial1.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
  delay(1500);                       // DFPlayer 上电需要时间，太早 begin 会失败

  ok = mp3.begin(Serial1);
  if (!ok) {
    Serial.println("[2_mp3] 未找到 DFPlayer");
    Serial.println("  1) ESP TX(17) 是否接到模块 RX？");
    Serial.println("  2) RX 线上有没有串 1kΩ？");
    Serial.println("  3) 模块是不是 5V 供电？");
    Serial.println("  4) SD 卡是不是 FAT32，文件夹叫 mp3？");
    return;
  }
  Serial.println("[2_mp3] DFPlayer OK");
  mp3.volume(22);
  mp3.EQ(DFPLAYER_EQ_NORMAL);
}

void loop() {
  if (!ok) { delay(1000); return; }
  Serial.printf("[2_mp3] 播放 %04d.mp3\n", track);
  mp3.play((uint16_t)track);
  track = (track >= 5) ? 1 : track + 1;
  delay(3000);
}
