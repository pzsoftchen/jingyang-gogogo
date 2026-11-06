# 家长页重设计 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把家长端 `/`、`/quick`、`/album` 改成与设备端同一套暖纸/深蓝视觉，第一屏收敛为三个时刻 + 问题提示 + 设备状态，并让音量可调。

**Architecture:** 继续使用 Python 标准库 `http.server` + SQLite 的单文件服务；用 CSS 变量切换 day/night，用纯函数 `theme_for_time()` 决定时段。设备状态复用 main 已实现的 `device_status()`；音量新增 `settings.volume` + `VOL=` 协议，固件端应用并做夜间 60% 压缩。家长页只做工具，不复制设备端的弧线/四幕布局。

**Tech Stack:** Python 3.9+ 标准库、SQLite、原生 HTML/CSS/少量 JS、Arduino ESP32-S3 固件。

**Spec:** `docs/superpowers/specs/2026-11-06-parent-page-design.md`

## Global Constraints

- 工作目录是本 worktree：`.claude/worktrees/parent-page-design`；不要修改主工作树的文件。
- 不实现设备端四幕 UI 重设计；本计划只做家长页、quick、album、设备状态与音量。
- 不引入前端框架、构建器或新 Python 依赖；保持单页 HTML + 标准库。
- 不改变现有路由、表单字段语义、PRG 行为；不新增账号/鉴权。
- 不新增内容推送、积分、排名、图表或完成率看板。
- 主题 token 以 spec 第五节为准：暖纸/深蓝、薄荷/珊瑚；正文左对齐，时间右上角。
- 音量是跨端功能：`settings.volume` + `VOL=` + 固件 `set_volume()`；夜间 60% 在设备端应用。
- 现有测试全绿是每个任务的门槛；固件三种板都必须编译。
- 每个任务结束提交一次；提交信息用 `feat:`/`test:`/`chore:` 前缀。

## Review Focus

1. **设备离线/心跳过期**：`device_status()` 必须显示"设备离线 · 最后同步 …"，页面不能 500；测试覆盖旧 `last_seen` 和缺失设备行。
2. **AI 草稿未审核**：问题提示必须计入 `status='draft'`，否则家长会以为没有待办；测试覆盖 ask/pending/draft/answered 四种状态。
3. **音量边界**：0/60/100 和非法值必须 clamp；固件缺 `VOL=` 时保持默认，不能静音或越界；测试覆盖服务端与固件源契约。
4. **时段边界**：06:29/06:30/19:59/20:00 必须正确切换 day/night；测试纯函数，不依赖当前时间。
5. **惊喜已用**：已给状态下不能出现可点击但无效的「明天再说」；测试断言不包含 `surprise_skip` 表单。
6. **打印/夜间**：`/album` 打印必须强制暖纸浅色；测试检查 `@media print` 与主题类。

---

### Task 0: 把 main 已验证的 1.85C/状态改动带进 worktree（前置）

**Files:**
- Modify: `server.py`, `tests/test_server.py`, `firmware/habit_coach/habit_coach.ino`
- Create: `firmware/habit_coach/st77916_185c_init.h`
- Test: `tests/test_server.py`

**Interfaces:**
- Consumes: main 工作树 `server.py` 中的 `device_status()`、`devices.ip/rssi`、`deviceQuery()` 的 `rssi=`，以及固件 1.85C 显示/触摸修复。
- Produces: 本分支拥有 69 个绿测试的基线；后续任务直接使用 `device_status()`。

- [ ] **Step 1: 从 main 生成补丁（排除 .gitignore，避免与 worktree 的 .superpowers 规则冲突）**

```bash
cd /Users/lanlan/esp32/habit-coach
git diff -- server.py tests/test_server.py firmware/habit_coach/habit_coach.ino > /tmp/coach-main-status.patch
cp firmware/habit_coach/st77916_185c_init.h .claude/worktrees/parent-page-design/firmware/habit_coach/
```

- [ ] **Step 2: 在 worktree 应用补丁**

```bash
cd /Users/lanlan/esp32/habit-coach/.claude/worktrees/parent-page-design
git apply --3way /tmp/coach-main-status.patch
```

Expected: 无冲突。若冲突，停止并报告，不要手工猜测。

- [ ] **Step 3: 跑现有测试**

Run: `make test`
Expected: `Ran 69 tests ... OK`（或 main 当前测试数；必须全绿）。

- [ ] **Step 4: 提交前置改动**

```bash
git add server.py tests/test_server.py firmware/habit_coach/habit_coach.ino firmware/habit_coach/st77916_185c_init.h
git commit -m "chore: bring verified 1.85C and device-status work into parent-page branch"
```

---

### Task 1: 时段选择纯函数

**Files:**
- Modify: `server.py`（在 `today_str()` 附近新增函数）
- Test: `tests/test_server.py`

**Interfaces:**
- Produces: `theme_for_time(hour: int, minute: int) -> str` 返回 `"day"`/`"night"`；`theme_now() -> str` 使用 `datetime.datetime.now()`。

- [ ] **Step 1: 写失败测试**

```python
class ThemeTests(Base):
    def test_theme_boundaries(self):
        self.assertEqual(server.theme_for_time(6, 29), "night")
        self.assertEqual(server.theme_for_time(6, 30), "day")
        self.assertEqual(server.theme_for_time(19, 59), "day")
        self.assertEqual(server.theme_for_time(20, 0), "night")
        self.assertEqual(server.theme_for_time(0, 0), "night")
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k theme_boundaries`
Expected: FAIL / AttributeError（函数不存在）。

- [ ] **Step 3: 实现**

```python
def theme_for_time(hour, minute):
    """06:30–20:00 为白天，其余为夜晚。"""
    return "day" if (6, 30) <= (hour, minute) < (20, 0) else "night"


def theme_now():
    now = datetime.datetime.now()
    return theme_for_time(now.hour, now.minute)
```

- [ ] **Step 4: 运行测试**

Run: `python3 -m unittest discover -s tests -v -k theme_boundaries`
Expected: PASS。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: add day/night theme boundary helper"
```

---

### Task 2: 家长页主题、标题、日期时间和状态胶囊

**Files:**
- Modify: `server.py`（`PAGE` 模板 CSS/头部、`render_page` 占位符）
- Test: `tests/test_server.py`

**Interfaces:**
- Consumes: `theme_now()`、`theme_for_time()`、`device_status_html(conn)`。
- Produces: `PAGE` 中 `{{THEME}}`、`{{SUN}}`、`{{DAY_CN}}`、`{{WEEKDAY_CN}}`、`{{NOW}}`、`{{DEVSTATUS}}`；`sun_svg(theme: str) -> str`。

- [ ] **Step 1: 写失败测试**

```python
class ParentPageThemeTests(HttpTests):
    def test_home_has_theme_class(self):
        code, body = self.get("/")
        self.assertEqual(code, 200)
        html = body.decode("utf-8")
        self.assertIn('class="%s"' % server.theme_now(), html)

    def test_home_has_date_and_time_left_aligned(self):
        code, body = self.get("/")
        html = body.decode("utf-8")
        d = datetime.date.today()
        wd = "一二三四五六日"[d.weekday()]
        self.assertIn("今天 %d月%d日 周%s" % (d.month, d.day, wd), html)
        self.assertRegex(html, r"\d{2}:\d{2}")
        self.assertIn("today-head", html)

    def test_home_has_device_status(self):
        self.get("/api/state.txt?d=poc&rssi=-61")
        code, body = self.get("/")
        html = body.decode("utf-8")
        self.assertIn("设备在线", html)
        self.assertIn("-61 dBm", html)
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k "has_theme_class or has_date_and_time or has_device_status"`
Expected: FAIL（缺少 class/日期/状态）。

- [ ] **Step 3: 改 PAGE 头部和 CSS**

- `<body class="{{THEME}}">`；添加 day/night CSS 变量（spec 5.1）。
- 头部结构：`<div class="today-head">{{SUN}}<div class="identity"><h1>{{CHILD}}</h1><div class="dateline">今天 {{DAY_CN}} 周{{WEEKDAY_CN}} · {{STAGENAME}}</div></div><div class="now">{{NOW}}</div></div>`。
- 所有 `h1/.dateline/.identity/.now` 左对齐；`.now { margin-left:auto; }`。
- `{{DEVSTATUS}}` 放标题下、三张卡之上。
- 移除标题里的 `每天最多推 {{BUDGET}} 次内容`；预算移到惊喜卡/更多。
- 实现 `sun_svg(theme)`：day 太阳、night 睡帽/闭眼；返回内联 SVG。
- `render_page` 计算：

```python
d = datetime.date.fromisoformat(s["day"])
day_cn = "%d月%d日" % (d.month, d.day)
weekday_cn = "一二三四五六日"[d.weekday()]
now = datetime.datetime.now().strftime("%H:%M")
```

并替换上述占位符；保留现有占位符替换。

- [ ] **Step 4: 运行相关测试和全量测试**

Run: `python3 -m unittest discover -s tests -v -k "has_theme_class or has_date_and_time or has_device_status or placeholders"`
Expected: PASS；`make test` 全绿。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: add warm day/night parent page header with date, time and device status"
```

---

### Task 3: 问题提示 + 「更多」折叠区 + 问题列表

**Files:**
- Modify: `server.py`（`open_questions_count`、`questions_for_parent`、`render_page`、`PAGE`）
- Test: `tests/test_server.py`

**Interfaces:**
- Produces: `open_questions_count(conn) -> int`（`ask`+`pending`+`draft`）；`questions_for_parent(conn, answered_limit=8) -> list[dict]`。
- PAGE 占位符：`{{NUDGE}}`、`{{QUESTIONS}}`、`{{QCOUNT}}`；`<details class="more" id="more">`。

- [ ] **Step 1: 写失败测试**

```python
class NudgeCountTests(Base):
    def test_count_includes_draft_pending_ask(self):
        for status in ("ask", "pending", "draft", "answered"):
            self.conn.execute(
                "INSERT INTO questions(question,answer,status,created) VALUES(?,?,?,?)",
                ("q-" + status, "", status, server.now_iso()))
        self.conn.commit()
        self.assertEqual(server.open_questions_count(self.conn), 3)


class NudgeHttpTests(HttpTests):
    def test_home_shows_nudge(self):
        self.conn.execute(
            "INSERT INTO questions(question,answer,status,created) VALUES('为什么天会黑','',?,?)",
            ("draft", server.now_iso()))
        self.conn.commit()
        code, body = self.get("/")
        html = body.decode("utf-8")
        self.assertIn("1 个问题等你处理", html)
        self.assertIn('id="more"', html)

    def test_no_nudge_when_none(self):
        code, body = self.get("/")
        self.assertNotIn("问题等你处理", body.decode("utf-8"))
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k "nudge or count_includes"`
Expected: FAIL / AttributeError。

- [ ] **Step 3: 实现**

- `open_questions_count` 查询 `status IN ('ask','pending','draft')`。
- `questions_for_parent`：
  ```
  开放项：WHERE status IN ('ask','pending','draft') ORDER BY id DESC
  已答：WHERE status='answered' ORDER BY id DESC LIMIT 8
  ```
- `render_page` 生成 `nudge_html`（0 时为空字符串）和 `questions_html`；保留 `POST /parent/question` 手动输入表单与问题卡预览。
- PAGE 把现有问题/识字/周回顾/审批/阶段/习惯/设置/设备 section 收进**一个** `<details class="more" id="more"><summary>更多</summary>…</details>`；首屏保留三张卡 + NUDGE + More。
- 保留 `{{NOTEBANNER}}` 为非卡片细提示；不改变现有 note 逻辑。

- [ ] **Step 4: 运行测试**

Run: `python3 -m unittest discover -s tests -v -k "nudge or count_includes"`，再 `make test`。
Expected: PASS/全绿。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: consolidate parent page details and add open-question nudge"
```

---

### Task 4: 今日记录/撤回、预置语音横向滚动、惊喜已给状态

**Files:**
- Modify: `server.py`（`today_records_html`、`render_page`、`PAGE`、`slots_html`/CSS）
- Test: `tests/test_server.py`

**Interfaces:**
- Produces: `today_records_html(conn) -> str`，在「更多 → 习惯开关」里列出今天的打卡与 `/parent/revoke` 表单。
- PAGE 中首屏不再渲染 `/parent/revoke`；More 中恰好渲染一次。

- [ ] **Step 1: 写失败测试**

```python
class MoreHabitsTests(HttpTests):
    def test_today_records_and_revoke_live_in_more(self):
        server.check_in(self.conn, "brush", "poc")
        code, body = self.get("/")
        html = body.decode("utf-8")
        self.assertIn('id="more"', html)
        self.assertIn("今天的记录", html)
        self.assertEqual(html.count('action="/parent/revoke"'), 1)

    def test_all_praise_slots_are_rendered(self):
        code, body = self.get("/")
        html = body.decode("utf-8")
        self.assertEqual(html.count('data-slot="'), len(server.PRESET_PRAISE))

    def test_used_surprise_has_no_dead_skip_button(self):
        server.claim_surprise(self.conn)
        server.mark_surprise(self.conn, "skip", "")
        code, body = self.get("/")
        html = body.decode("utf-8")
        self.assertNotIn('action="/parent/surprise_skip"', html)
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k "today_records or all_praise or used_surprise"`
Expected: FAIL（revoke 在首屏、没有"今天的记录"、used 状态仍可能有表单）。

- [ ] **Step 3: 实现**

- 首屏 `habits_html` 去掉 revoke 表单；`today_records_html` 在 More→习惯开关里生成。
- `.slots` 改为横向滚动：`display:flex; overflow-x:auto; white-space:nowrap;`，10 个 chip 全部保留。
- `render_page` 的 used surprise 分支不渲染 `surprise_skip` 表单；只显示状态文字。
- 保留 `/parent/revoke` 路由与测试。

- [ ] **Step 4: 运行测试**

Run: 上述 `-k` 测试，再 `make test`。
Expected: PASS/全绿。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: move revoke to More, scroll praise chips, remove dead surprise action"
```

---

### Task 5: 服务端音量设置与 VOL 协议

**Files:**
- Modify: `server.py`（`get_volume`、`/parent/volume`、`state_as_text`、`render_page`、PAGE 设备区）
- Test: `tests/test_server.py`

**Interfaces:**
- Produces: `get_volume(conn) -> int`（0–100，默认 60）；PAGE 表单 `POST /parent/volume`；`state.as_text` 追加 `VOL=`.
- Consumes: `settings` 表。

- [ ] **Step 1: 写失败测试**

```python
class VolumeTests(Base):
    def test_default_is_60(self):
        self.assertEqual(server.get_volume(self.conn), 60)

    def test_clamps_and_state_line(self):
        server.set_setting(self.conn, "volume", "-10")
        self.conn.commit()
        self.assertEqual(server.get_volume(self.conn), 0)
        server.set_setting(self.conn, "volume", "500")
        self.conn.commit()
        self.assertEqual(server.get_volume(self.conn), 100)
        self.assertIn("VOL=100", server.state_as_text(self.conn))


class VolumeHttpTests(HttpTests):
    def test_parent_volume_route(self):
        code, _ = self.post("/parent/volume", {"volume": "45"})
        self.assertEqual(code, 200)
        self.assertEqual(server.get_volume(self.conn), 45)
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k "volume"`
Expected: FAIL（函数/路由/协议缺失）。

- [ ] **Step 3: 实现**

- `get_volume` clamp；`/parent/volume` POST 读取 form、保存、PRG 到 `/?sent=volsaved`。
- `state_as_text` 在 `PACK=` 前追加 `VOL=%d`。
- `render_page` 替换 `{{VOLUME}}`；PAGE 的「更多 → 设备」加 slider（`name="volume" min=0 max=100 value="{{VOLUME}}"`）和一个保存按钮。
- `test_unknown_protocol_lines_are_ignored` 的 known 列表加 `VOL=`。
- 测试辅助：`tests/test_server.py` 加 `import urllib.parse`；把 `HttpTests.post(self, path)` 扩展为 `post(self, path, data=None)`，有 data 时 `urllib.parse.urlencode(data).encode("utf-8")`；现有调用不用改。

- [ ] **Step 4: 运行测试**

Run: `python3 -m unittest discover -s tests -v -k "volume or unknown_protocol"`，再 `make test`。
Expected: PASS/全绿。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: add parent volume setting and VOL protocol line"
```

---

### Task 6: 固件音量应用与夜间 60%

**Files:**
- Modify: `firmware/habit_coach/habit_coach.ino`
- Test: `tests/test_server.py`（FirmwareContractTests 源契约）

**Interfaces:**
- Produces: `volPct`（0–100，默认 60）、`applyVolume()`、`isNightVolume()`；`syncState()` 解析 `VOL=`。
- Consumes: `VOL=` from `state.txt`。

- [ ] **Step 1: 写失败源契约测试**

```python
class FirmwareVolumeTests(unittest.TestCase):
    def test_firmware_parses_vol_and_applies_volume(self):
        src = open(os.path.join(os.path.dirname(HERE), "firmware", "habit_coach", "habit_coach.ino"), encoding="utf-8").read()
        self.assertIn('line.startsWith("VOL=")', src)
        self.assertIn("void applyVolume()", src)
        self.assertIn("audio.setVolume", src)
        self.assertIn("mp3.volume", src)
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k firmware_parses_vol`
Expected: FAIL。

- [ ] **Step 3: 实现**

- 全局 `int volPct = 60; int volApplied = -1;`。
- `applyVolume()`：
  - `int v = volPct; if (isNightVolume()) v = v * 60 / 100;`
  - `USE_SD_AUDIO`: `audio.setVolume(map(v, 0, 100, 0, 21));`
  - `USE_DFPLAYER`: `mp3.volume(map(v, 0, 100, 0, 30));`
- `isNightVolume()` 用 `getLocalTime`；无时间返回 false。
- `syncState()` 解析 `VOL=`，合法 0–100 才更新并 `applyVolume()`。
- `audioInit`/DFPlayer setup 中用 `applyVolume()` 代替硬编码 12/22。

- [ ] **Step 4: 运行测试和三种板编译**

Run: `python3 -m unittest discover -s tests -v -k firmware_parses_vol`，再 `./tools/build.sh all`。
Expected: PASS；三种板编译成功。

- [ ] **Step 5: commit**

```bash
git add firmware/habit_coach/habit_coach.ino tests/test_server.py
git commit -m "feat: apply parent volume in firmware with night cap"
```

---

### Task 7: /quick 与 /album 暖纸/深蓝 + 手动主题

**Files:**
- Modify: `server.py`（`QUICK_HTML`、`ALBUM_CSS`、`render_album`、`/quick` handler、`PAGE` 的 theme toggle JS）
- Test: `tests/test_server.py`

**Interfaces:**
- Produces: `THEME_JS`（localStorage override + `#themeToggle`）；`QUICK_HTML`/album 使用 `{{THEME}}`、`{{SUN}}`。
- `render_album(conn)` 接受/使用 `theme_now()`；打印 CSS 强制浅色。

- [ ] **Step 1: 写失败测试**

```python
class QuickAlbumThemeTests(HttpTests):
    def test_quick_has_theme_and_toggle(self):
        code, body = self.get("/quick")
        html = body.decode("utf-8")
        self.assertIn('class="%s"' % server.theme_now(), html)
        self.assertIn("coach-theme", html)

    def test_album_has_theme_and_print_light(self):
        code, body = self.get("/album")
        html = body.decode("utf-8")
        self.assertIn('class="%s"' % server.theme_now(), html)
        self.assertIn("@media print", html)
```

- [ ] **Step 2: 运行并确认失败**

Run: `python3 -m unittest discover -s tests -v -k "quick_has_theme or album_has_theme"`
Expected: FAIL。

- [ ] **Step 3: 实现**

- 把 day/night token 抽成共享 CSS（PAGE/QUICK/ALBUM 复用或内联同值）。
- `THEME_JS`：读取 `localStorage['coach-theme']`，覆盖 body class；`#themeToggle` 切换并写回。
- `/quick` handler：在现有 `{{RECENT}}`/`{{PENDING}}` 替换之外，追加 `.replace("{{THEME}}", theme_now()).replace("{{THEME_JS}}", THEME_JS)`。
- `render_album` 使用 `theme_now()` 和 `THEME_JS`；album `@media print` 强制 day token。
- PAGE 的“更多”里加 `#themeToggle` 按钮并内联 `THEME_JS`。
- 页面标题/按钮左对齐；不影响现有表单/PRG。

- [ ] **Step 4: 运行测试**

Run: `python3 -m unittest discover -s tests -v -k "quick_has_theme or album_has_theme"`，再 `make test`。
Expected: PASS/全绿。

- [ ] **Step 5: commit**

```bash
git add server.py tests/test_server.py
git commit -m "feat: apply parent theme to quick and album pages"
```

---

### Task 8: 全量验证与收尾

**Files:** 无新文件。

- [ ] **Step 1: 全部测试**

Run: `make test`
Expected: 所有测试通过（至少 69 + 新增）。

- [ ] **Step 2: 语法/差异检查**

Run: `make check && git diff --check`
Expected: 无输出错误。

- [ ] **Step 3: 三种板编译**

Run: `./tools/build.sh all`
Expected: DevKitC / 1.85 / 1.85C 全部编译成功。

- [ ] **Step 4: 手动冒烟（本机浏览器）**

- 打开 `http://127.0.0.1:8099/`：day/night 主题、日期/时间、状态胶囊、三张卡、问题提示、更多。
- `/quick`、`/album`：主题一致；album 打印预览为浅色。
- 音量滑块保存后，`/api/state.txt` 包含 `VOL=`。

- [ ] **Step 5: 最终提交**

```bash
git add -A
git commit -m "feat: warm parent page with day/night theme, status and volume"
```
