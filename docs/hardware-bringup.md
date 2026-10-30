# 硬件清单与分步验收

> 原则：**一次只验证一种硬件**。
> 889 行的完整固件不要第一个烧——先把屏、语音、网络、按键分四个小测试跑通，
> 哪一步失败就只查那一步，不用在一堆可能性里猜。

---

## 一、购物清单（约 ¥130–180）

| 元件 | 关键词 | 约价 | 必须/可选 | 关键要求 |
|---|---|---|---|---|
| 主控 | `ESP32-S3 开发板 N16R8` | ¥55–75 | **必须** | **16MB Flash + 8MB PSRAM（八线/OPI）**。别买 N8R2，PSRAM 不够 |
| 屏 | `GC9A01 1.28寸 圆屏 240x240 SPI` | ¥25–40 | **必须** | **必须带 BL 背光脚**；有些便宜模块没引出 BL |
| 语音 | `DFPlayer Mini MP3 模块 + 3W 喇叭` | ¥20–30 | **必须** | 买带喇叭的套装；4Ω 或 8Ω 都行 |
| 存储 | microSD 卡 8–32GB | ¥0–20 | **必须** | 家里有旧卡最好；必须 **FAT32** 格式 |
| 连线 | 面包板 + 杜邦线（公母/母母） | ¥15–25 | **必须** | 各买一包，母母线最缺 |
| 按键 | 12mm 圆头按键帽 ×2 | ¥3 | **必须** | 孩子手指要按得动，别买贴片的 |
| 蜂鸣器 | 无源蜂鸣器（3.3V 可驱） | ¥2 | 建议 | 没接语音模块时的替代反馈 |
| 电阻 | 1kΩ ×2 | ¥1 | **必须** | DFPlayer 的 RX 线上串一个，少一个就烧模块 |
| 电源 | 5V/2A USB 电源 | ¥15 | 建议 | 屏 + 语音同时工作，电脑 USB 口可能不够 |
| USB 电流表 | `USB 测试仪` | ¥15 | 可选 | 排查"一接屏就重启"的利器 |

> **不建议一次买齐外壳**。先裸板跑通两周，再按实际尺寸打印。

---

## 二、接线（和固件里的引脚一一对应）

### 屏幕 GC9A01 → ESP32-S3

| 屏 | 板子 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL / CLK | GPIO12 |
| SDA / MOSI | GPIO11 |
| CS | GPIO9 |
| DC | GPIO10 |
| RST | GPIO18 |
| BL | GPIO7 |

> 如果模块**没有 CS 脚**：把模块的 CS 焊盘接 GND（有些模块内部已接地）。

### 其他

| 元件 | 板子 |
|---|---|
| 按键 A | GPIO4 ↔ GND |
| 按键 B | GPIO5 ↔ GND |
| 蜂鸣器 + | GPIO6（− 接 GND） |
| DFPlayer TX | GPIO16 |
| DFPlayer RX | GPIO17（**中间串 1kΩ**） |
| DFPlayer VCC / GND | 5V / GND（别接 3V3） |
| 喇叭 | DFPlayer SPK1 / SPK2 |

引脚选择已避开：USB(19/20)、Flash(26–32)、八线 PSRAM(33–37)、启动绑带脚(0/3/45/46)。

---

## 三、分步验收（按顺序做，每步单独成功再进下一步）

### Step 0 · 板子能被电脑识别

1. USB 线插上（**必须能传数据的线**，不是充电线）
2. Arduino IDE → 工具 → 开发板 → **ESP32S3 Dev Module**
3. 端口出现 `/dev/cu.usbmodem*` 之类的设备

✅ 成功：能看到端口
❌ 失败：换线 → 换 USB 口 → Mac 装 CH340/CP210x 驱动（S3 原生 USB 一般不用）

---

### Step 0.5 · 板子验真（`../hardware-test/0_board_check`）

**到货第一件事，先烧这个，别的都别接。**

串口应该打印：

```
Flash    : 16777216 bytes  (16 MB)  ✓
PSRAM    : 8388608 bytes   (8.0 MB)  ✓
115KB PSRAM 分配 : OK  ✓ 可以跑卡片模式
```

❌ 如果 PSRAM 是 0 或 2MB，或者 115KB 分配失败 → **卖家发错货，直接退**，
不要试图用代码绕过去（卡片模式必须要这么大一块缓冲）。

---

### Step 1 · 点屏（`../hardware-test/1_display`）

**只接屏幕，不接其他**。烧 `1_display.ino`。

✅ 成功：屏上出现彩色圆环 + `DISPLAY OK` 字样，颜色循环
❌ 排查顺序：
1. **黑屏** → 90% 是 BL 没接 / 没拉高（代码里 `digitalWrite(BL,HIGH)`）
2. **花屏、全白、全红** → CS 和 DC 接反了（最容易错的一对）
3. 编译报 `Arduino_Canvas 未定义` → Arduino_GFX 版本 <1.6.5，升级
4. 编译报 `BLACK 未定义` → Arduino_GFX ≥1.3 取消了颜色宏，测试代码里已经 `#define` 好了

---

### Step 2 · 语音（`../hardware-test/2_mp3`）

1. 把 `../voice-pack/mp3/` 拷到 SD 卡根目录，文件夹名 **`mp3`**
2. 插进 DFPlayer
3. 烧 `2_mp3.ino`（这时可以只接 DFPlayer，不接屏）

✅ 成功：串口打印 `DFPlayer OK`，每隔 3 秒播一句（0001→0005）
❌ 排查：
1. 串口打印 `未找到 DFPlayer` → **RX/TX 接反了**（ESP 的 TX 要接模块的 RX）
2. 没声音但打印 OK → 喇叭接错（SPK1/SPK2）或没插紧
3. 有杂音/重启 → 供电不足，DFPlayer 单独给 5V
4. 完全不识别 → 少了 1kΩ 电阻，或者 SD 卡不是 FAT32

> **命名规则**：`0001.mp3` 四位数字，不能是 `1.mp3`。

---

### Step 3 · 网络（`../hardware-test/3_net`）

1. 复制 `secrets.h.example` 为 `secrets.h`，填 Wi-Fi 和电脑 IP
2. 电脑上先跑 `python3 server.py`
3. 烧 `3_net.ino`，串口看输出

✅ 成功：串口打印 IP，然后打印出服务器返回的 `DATE= / STREAK= / H=...`
❌ 排查：
1. 连不上 WiFi → 2.4GHz！ESP32 不支持 5GHz
2. 连上了但请求失败 → 电脑 IP 写错（`ipconfig getifaddr en0`）
3. 请求超时 → macOS 防火墙：系统设置 → 网络 → 防火墙 → 允许 Python 接入
4. 手机能打开页面但板子不行 → 电脑和板子不在同一网段

---

### Step 4 · 按键 + 蜂鸣器（`../hardware-test/4_buttons`）

✅ 成功：按 A/B 有"嘀"声，串口打印 `A down` / `B down`
❌ 排查：按键没反应 → 引脚接 GND 了吗（代码用 `INPUT_PULLUP`，按下是 LOW）

---

### Step 5 · 完整固件（`../firmware/habit_coach`）

四个都通了再烧这个。

**开发板设置**（一个都不能错）：

| 项 | 值 |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | **16MB** |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **Huge APP (3MB No OTA)** |

烧完串口应该依次看到：

```
[ui] canvas OK
[mp3] DFPlayer OK
[wifi] ok ip=192.168.x.x
```

然后屏幕出现小豆的脸 + 星星。

**验收三件事**：

1. 串口敲 `h` → 打印出习惯表（id / 时间 / 语音编号）
2. 串口敲 `s` → 打印 `同步成功`
3. 家长页点一个字 → 设备播「牙，刷牙的牙」→ 屏幕出字卡，按 B 翻页

---

### Step 6 · 卡片模式（最容易出问题的一步）

推一张字卡，设备要下载 **115200 字节**的图片。

✅ 成功：屏幕显示"认一个字 1/2"
❌ 排查：
1. 串口报 `NO MEM` → PSRAM 没开（回去检查 PSRAM 选项）
2. 串口报 `CARD FAIL` → 大小对不上，多半是没走完 115200 字节（WiFi 太慢，重试一次）
3. 卡死 → 看是否 `[ui] canvas FAIL`，canvas 和 PSRAM 抢内存

---

## 四、串口命令速查（完整固件）

| 键 | 作用 |
|---|---|
| `t` | 试音（播 0001） |
| `r` | 重置今天 |
| `s` | 立即同步 |
| `q` | 查看离线队列 |
| `h` | 打印习惯表 |
| `c` | 进入卡片模式 |
| `1` `2` `3` `8` | 直接播对应语音编号 |

---

## 五、下单后、到货前可以做的事

1. 把手上的旧 microSD 卡找出来，格式化成 **FAT32**
2. 电脑装好 Arduino IDE 2.x + esp32 开发板包（3.3.x 或更新）
3. 库管理器装两个库：**Arduino_GFX_Library**、**DFRobotDFPlayerMini**
4. 跑一遍 `python3 server.py`，用手机试试家长页
5. 把 `secrets.h.example` 复制成 `secrets.h` 填好（WiFi 名/密码/电脑 IP）

到货那天，前四步大概 40 分钟能全部验证完。
