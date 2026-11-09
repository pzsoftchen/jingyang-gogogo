# 分步验收（一次只测一种硬件）

| 顺序 | 目录 | 测什么 | 通过标准 |
|---|---|---|---|
| 0 | `0_board_check/` | **板子验真**（是不是真的 N16R8） | 串口显示 Flash 16MB + PSRAM 8MB + 115KB OK |
| 0.5 | — | 板子被电脑识别 | Arduino IDE 里能看到端口 |
| 1 | `1_display/` | GC9A01 圆屏 | 屏上出现彩色圆环 + OK |
| 2 | `2_mp3/` | DFPlayer 语音 | 串口 `DFPlayer OK`，每 3 秒一句 |
| 3 | `3_net/` | Wi-Fi + 访问服务器 | 串口打印出 `DATE= / H=...` |
| 4 | `4_buttons/` | 按键 + 蜂鸣器 | 按下有"嘀"声 + 串口打印 |
| 5 | `../habit_coach/` | 完整固件 | 屏幕出现小豆的脸 + 星星 |

**前四步都通过再烧完整固件。** 哪一步失败就只查那一步，别在一堆可能性里猜。

严重故障的排查顺序见 [`../../docs/manual.md`](../../docs/manual.md) 的排错章节。

依赖库（库管理器装）：
- `Arduino_GFX_Library`（moononournation）>= 1.6.5
- `DFRobotDFPlayerMini`（DFRobot）

开发板设置（完整固件必须）：
- Board: **ESP32S3 Dev Module** / Flash Size: **16MB**
- PSRAM: **OPI PSRAM** / Partition: **Huge APP (3MB No OTA)**
