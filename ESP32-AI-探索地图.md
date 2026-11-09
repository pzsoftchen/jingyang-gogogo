# ESP32 × AI 探索地图（2026-10 版）

> 结论先说：**主力板买 ESP32-S3（N16R8）**；想省事/玩"AI 外设"就加一块 **M5StickC Plus2**；想要视觉、大屏、性能再上 **ESP32-P4**。
> 现在 ESP32 上最"有现在感"的三件事：① 给 Claude Code 做实体桌宠 ② 小智 AI 语音助手 + MCP 控制设备 ③ 用 MCP 让 AI Agent 直接写固件/烧录/调试。

---

## 一、选板：别买错，AI 玩法挑板子

| 芯片 | 关键规格 | AI 相关 | 适合 |
|---|---|---|---|
| **ESP32-S3** | 双核 Xtensa 240MHz，512KB SRAM，可外挂 8MB PSRAM，**AI 指令扩展** | 生态最全：ESP-SR 语音、ESP-DL 视觉、xiaozhi、Arduino 库全支持 | **第一块板首选** |
| **ESP32-P4** | 双核 32 位 RISC-V **400MHz**，768KB SRAM，大 PSRAM，MIPI-CSI/DSI、H.264、USB2.0 HS，**无 Wi-Fi**（配 ESP32-C6） | 跑视觉/显示/3D 有余量；已能 1W 功耗跑 OpenLara（古墓丽影） | 第二块：视觉、大屏、性能向 |
| **ESP32-C6** | RISC-V + Wi-Fi 6 / BLE / Thread / Zigbee | 算力弱，主要做无线协处理 | 配 P4 用，或纯联网节点 |
| **ESP32-C3 / 老 ESP32** | 便宜、内存小 | 新 AI 算法已不再支持老 ESP32 | 不建议做 AI 主力 |

**价位参考（随渠道波动）**：S3 开发板 N16R8 ¥40–90；ESP32-S3-BOX-3 ¥250–350；M5StickC Plus2 ¥150–200；M5Stack CoreS3 ¥350–450；ESP32-P4 开发板 ¥150–300；ESP32-P4-EYE ¥170–250；XIAO ESP32S3 Sense ¥70–110。

**一句话**：S3 是"通用底盘"，P4 是"性能升级包"，M5Stack 是"开箱即用的外壳+屏+麦克风"。

---

## 二、9 个有意思的方向

### 1. 给 Claude Code / Cursor 做一只实体桌宠 ⭐当前最热
Anthropic 官方开源 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)：ESP32（Arduino）通过 **BLE** 连桌面版 Claude，把权限审批、会话状态、Token 进度显示在小屏上，**按 A 批准、按 B 拒绝**，还有 18 种 ASCII 小动物 + 自定义 GIF 角色包。
- 硬件：M5StickC Plus（官方参考），¥150 左右；任何 S3 板 + 屏也能改
- 难度：★☆☆（`pio run -t upload` 烧录，开发者模式里配对）
- 升级玩法：加语音输入当"实体入口"、加震动/灯效、改成自己画的角色
- 注意：需要在 Claude 桌面端开启 Developer Mode

### 2. 小智 AI 语音助手：端云协同的标准答案 ⭐生态最大
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)：流式 ASR + LLM + TTS，支持 **WebSocket / MQTT+UDP**，离线唤醒词（ESP-SR）、Opus 音频、表情屏、摄像头输入、说话人识别，**已适配 138 个板型 / 171 个固件变体**，覆盖 ESP32/C3/C5/C6/S3/P4。
- 官方已把它收进 [esp_xiaozhi 组件](https://components.espressif.com/components/espressif/esp_xiaozhi)（ESP-IDF ≥ 5.5），可自己写 MCP 工具扩展
- 服务端：官方 xiaozhi.me（个人可免费用 Qwen 实时模型），也可自建（[Python](https://github.com/xinnan-tech/xiaozhi-esp32-server) / Java / Go 版都有）
- 难度：★★☆（建议 ESP-IDF v6.0.1+，仓库推荐 release/v6.1，5.x 已不支持）
- 玩点：**设备端 MCP** 让它真能开关灯、转舵机；云端 MCP 能接智能家居、电脑操作

### 3. 让 LLM 拥有"手"：设备端 MCP / AI Agent 执行器 ⭐概念最新
MCP 正从软件走向硬件。两条路：
- **自己写**：ESP-IDF 的 `esp_mcp` 把 GPIO/电机/传感器封装成 MCP tool，LLM 直接调用
- **用框架**：[ESP-Brookesia v0.7](https://developer.espressif.com/blog/2026/05/esp-brookesia-new-architecture-0-7/)（2026-05）自带 HAL + 服务层 + Agent Manager，已支持 **Coze / OpenAI Realtime / 小智** 三种 Agent，`examples/agent/chatbot` 演示"说话 → 调 MCP → 调硬件"
- 难度：★★★（要理解 MCP 协议 + 服务注册）
- 这才是"AI 硬件"和"联网玩具"的分水岭：模型不只是聊天，而是能改变物理世界

### 4. 端侧离线语音：唤醒词 + 命令词 + 回声消除
[ESP-SR](https://components.espressif.com/components/espressif/esp-sr)（稳定版 2.6.0）：WakeNet9（"你好小智""Hi 乐鑫""Jarvis"…可定制）、MultiNet 最多 300 条中英文命令词、AFE（AEC/VAD/BSS/降噪）。S3/P4 才支持新模型，老 ESP32 已停更。
- 硬件：S3 + I2S 麦克风（INMP441）+ MAX98357A + 小喇叭，¥30 内搞定
- 难度：★★☆
- 玩点：**完全离线**、隐私友好、响应 <100ms；适合做"不开灯也能用的开关"

### 5. 端侧跑小语言模型：能跑，但别期待
2026-07 乌克兰开发者 Slava S 的 [ESP32-AI](https://ai.zol.com.cn/1223/12230801.html) 在 8 美元 ESP32-S3（8MB PSRAM/16MB Flash）上跑了 **2890 万参数**模型：4-bit 量化压到 14.9MB，参考 Gemma 的逐层嵌入，大 embedding 放 Flash、核心权重放 SRAM，按 token 按需取行。
- 现实边界：只能生成简短文本，不能问答/写代码；此前同类记录才 26 万参数
- 价值：极好的**量化 + 内存调度**教学项目；P4（更大 PSRAM）上空间更大
- 难度：★★★★（硬核，适合想懂原理的人）

### 6. 视觉：端侧检测 + 云端 VLM 端云协同
- 端侧：[ESP32-P4-EYE](https://developer.espressif.com/blog/2025/05/introducing-p4-eye/)（MIPI-CSI 摄像头 + 麦克风 + 屏 + SD + 板载 C6，约 ¥170–250）跑 ESP-DL/ESP-WHO 做人脸/目标检测；S3 用 ESP32-S3-EYE / XIAO Sense
- 云端：ESP32 只负责拍照上传，Qwen-VL / GPT 类模型"看图说话"，做巡检、找东西、"这盆花该浇水了吗"
- 难度：★★☆（端侧）/ ★★★（端云）
- 玩点：成本 ¥200 内做一个"会说话的摄像头"

### 7. Wi-Fi CSI 无线感知：不用摄像头的"雷达"
[ESP-CSI](https://documentation.espressif.com/esp-csi/master/README_cn.md)：全系 ESP32 都支持 CSI，通过信道状态信息感知**走动、呼吸、跌倒**，配套 [esp-radar 组件](https://components.espressif.com/components/espressif/esp-radar)。
- 硬件：2 块 ESP32（一块发一块收）或 1 块 + 路由器
- 难度：★★★（信号处理 + 自己训模型效果更好）
- 玩点：隐私友好的人体存在检测；"隔墙感知"的极客感极强

### 8. AI 桌面机器人 / 机器狗：情绪价值 + 具身智能入门
小智生态里的 [ESP-HI 低成本机器狗](https://www.bilibili.com/video/BV1BHJtz6E2S/)、桌面宠物狗（STM32 控运动 + S3 跑语音/视觉）这类项目，是"语音 + 视觉 + 运动控制"的最小组合。
- 难度：★★★★（机械 + 电源 + 多 MCU 协同）
- 玩点：上层 LLM 规划、下层 MCU 执行，正是具身智能的入门结构

### 9. 元方向：让 AI Agent 自己玩板子 ⭐建议所有人都试
- **ESP-IDF v6.0 内置 Tools MCP Server**（2026-04）：Cursor / Claude Code 直接设置 target、编译、烧录、看状态
- [nff](https://pypi.org/project/nff/)（2026-10 发布）：MCP server，给 coding agent 34 个工具——编译/烧录/串口读写/JTAG 断点/崩溃诊断/OTA，支持 PlatformIO 1000+ 板
- 玩法：你只说"让 LED 以 1Hz 闪并确认串口输出"，Agent 自己写代码→编译→烧录→读串口→修正
- 意义：**固件开发的门槛被 AI 抹平了**，你的时间应该花在"想做什么"上

---

## 三、按投入时间选路线

**周末 1（¥150–200）｜先尝到甜头**
M5StickC Plus2 → 烧 claude-desktop-buddy → 桌宠上线。10 分钟出结果，情绪价值拉满。

**周末 2–4（¥100–150）｜做出真正能用的东西**
S3 开发板 + INMP441 + MAX98357A → 跑小智固件 → 改唤醒词/表情 → 写第一个 MCP 工具（比如"帮我开台灯"）。

**1–2 个月（¥300–500）｜往深了走**
S3 + P4-EYE（或 P4 开发板）→ 视觉端云协同；同时用 ESP-IDF MCP / nff 让 Claude Code 帮你迭代固件。

---

## 四、起步购物清单（第一晚就能跑）

| 物品 | 建议 | 约价 |
|---|---|---|
| 主控 | ESP32-S3 开发板 N16R8（立创实战派 S3 / 官方 DevKitC-1） | ¥40–90 |
| 麦克风 | INMP441（I2S） | ¥10 |
| 功放+喇叭 | MAX98357A + 3W 小喇叭 | ¥15 |
| 线材 | USB-C 数据线（能传数据！）、杜邦线、面包板 | ¥20 |
| 可选 | 0.96" OLED / 1.8" TFT | ¥15–40 |

---

## 五、几个坑，提前说

1. **一定要 N16R8 这种带 PSRAM 的型号**，AI 音频/视觉都吃 PSRAM；只买 N8R2 会很难受。
2. **ESP-IDF 版本很挑**：小智要求 v6.0.1+（推荐 release/v6.1），5.x 不支持；旧教程会让你踩坑。
3. **Arduino 还是 ESP-IDF**：桌宠/小项目 Arduino 快；语音/视觉/多任务用 ESP-IDF。别一开始就纠结。
4. **电源**：麦克风+功放+屏幕一起上时，USB 口供电容易掉压重启，准备一个 5V/2A 供电。
5. **API Key 与延迟**：端云方案要联网、要 Key，注意成本；实时语音模型的账单会比想象中快。
6. **MCP 安全**：让 LLM 控制 GPIO/电机前，先做"危险动作白名单"，别把 220V 继电器直接接上去。
7. **P4 没有 Wi-Fi**，买 P4 板子要确认板载了 ESP32-C6（P4-EYE、多数 Waveshare/FireBeetle 板都有）。

---

## 六、参考链接

- Anthropic 官方桌宠：<https://github.com/anthropics/claude-desktop-buddy>
- 小智 AI 语音助手：<https://github.com/78/xiaozhi-esp32> ｜ [组件](https://components.espressif.com/components/espressif/esp_xiaozhi)
- ESP-Brookesia（Agent 框架）v0.7 架构：[博客](https://developer.espressif.com/blog/2026/05/esp-brookesia-new-architecture-0-7/) ｜ [GitHub](https://github.com/espressif/esp-brookesia)
- ESP-SR 语音识别：<https://components.espressif.com/components/espressif/esp-sr>
- ESP-CSI 无线感知：<https://documentation.espressif.com/esp-csi/master/README_cn.md>
- ESP32-P4-EYE 视觉板：[介绍](https://developer.espressif.com/blog/2025/05/introducing-p4-eye/) ｜ [用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-eye/user_guide.html)
- ESP-IDF Tools MCP Server：[博客](https://developer.espressif.com/blog/2026/04/esp-idf-tools-mcp-server/)
- nff（Agent 控制硬件）：<https://pypi.org/project/nff/>
- ESP32 上跑 2890 万参数 LLM：[报道](https://ai.zol.com.cn/1223/12230801.html)
- ESP32-P4 跑《古墓丽影》（性能参考）：<https://m.163.com/dy/article/L6O81O980511CPVM.html>
