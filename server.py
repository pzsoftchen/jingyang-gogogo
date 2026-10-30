#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
习惯伙伴 · 家长端小服务（零依赖，只用 Python 标准库）

启动：  python3 server.py
家长页：http://<这台机器的局域网IP>:8099
设备端：GET  /api/state.txt?device=coach
        POST /api/checkin.txt?habit=brush
        POST /api/ack.txt?id=12

设计要点：
1. 打卡记录在本地 SQLite，不联网、不上传，符合"自用 + 隐私"。
2. "爸妈的认可"不是 AI 夸孩子，而是把家长的话原样送到设备上播放——
   设备只是通道，家长才是权威。这是这个产品最重要的一个决定。
3. 设备端用纯文本协议（不是 JSON），ESP32 上更好解析、更省内存。
"""

import datetime
import json
import os
import io
import sqlite3
import urllib.parse

import card as cardrender
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(HERE, "coach.db")
PORT = 8099

# ----------------------------- 默认配置 -----------------------------
# 习惯预设表：按一天的时间顺序组织，每条 = 一颗星。
# 一条"流程"（如洗澡）只算一颗星，里面的步骤只影响语音引导，不单独打卡。
# (id, 名称, 图标, 提醒时, 提醒分, 提醒语音, 完成语音, 时段, 步骤说明, 语音序列, 最短秒数)
# 最短秒数 > 0 时：孩子按下按钮不会立刻亮星，而是开始倒计时，时间到才点亮。
HABIT_PRESETS = [
    # ---------- 早上 ----------
    ("wake",      "按时起床",   "🌞", 7,  20, 51, 52, "morning", "", "", 0),
    # 出门流程 07:30 开始，15 分钟准备：语音按 0/7/12 分钟分三句引导
    ("morning",   "出门流程",   "🚪", 7,  30, 53, 63, "morning",
     "穿衣服(0') → 穿鞋(7') → 背书包(12')", "53@0,43@7,62@12", 0),
    # ---------- 白天 ----------
    ("water",     "喝水",       "💧", 10, 0,  45, 46, "day", "", "", 0),
    ("outdoor",   "户外玩 30 分钟", "🏃", 16, 30, 57, 58, "day", "", "", 0),
    ("help",      "帮忙摆碗筷", "🍽", 17, 45, 59, 60, "day", "", "", 0),
    # ---------- 晚上（顺序 = 睡前流程）----------
    ("bathflow",  "洗澡流程",   "🛁", 19, 10, 47, 50, "night",
     "脏衣服放进洗衣机 → 洗澡", "47@0,49@1", 300),
    ("toys",      "收玩具",     "🧸", 19, 40, 4, 5, "night", "", "", 0),
    ("brush",     "刷牙",       "🪥", 20, 30, 2, 3, "night", "", "", 120),
    ("book",      "睡前绘本",   "📖", 20, 45, 6, 7, "night", "", "", 0),
    # ---------- 单独版（想拆开练时用）----------
    ("dress",     "自己穿衣服", "👕", 7,  30, 53, 54, "extra", "", "", 0),
    ("shoes",     "自己穿鞋",   "👟", 7,  37, 43, 44, "extra", "", "", 0),
    ("laundry",   "脏衣服入洗衣机", "🧺", 19, 10, 47, 48, "extra", "", "", 0),
    ("bath",      "晚上洗澡",   "🛁", 19, 20, 49, 50, "extra", "", "", 0),
    ("pack",      "收书包",     "🎒", 19, 45, 41, 42, "extra", "", "", 0),
]
DEFAULT_ACTIVE = ["brush", "toys"]
MAX_HABITS = 4        # 设备固件最多画 4 颗星，别超

PERIODS = [("morning", "早上"), ("day", "白天"), ("night", "晚上"), ("extra", "单独版（想拆开练时用）")]


def preset_of(hid):
    for row in HABIT_PRESETS:
        if row[0] == hid:
            return row
    return None


def habit_meta(hid):
    row = preset_of(hid)
    if not row:
        return {"h": 0, "m": 0, "rt": 0, "dt": 0, "seq": "", "steps": ""}
    minsec = row[10] if len(row) > 10 else 0
    return {"h": row[3], "m": row[4], "rt": row[5], "dt": row[6],
            "seq": row[9], "steps": row[8], "minsec": minsec,
            "zika": HABIT_ZIKA.get(hid, "")}
CHILD_NAME = os.environ.get("CHILD_NAME", "景阳")
COACH_NAME = os.environ.get("COACH_NAME", "小豆豆")
DAILY_LIMIT_MIN = 20          # 5 岁半：每天建议上限（分钟）
# 10 条默认认可台词，与 SD 卡里的 0031.mp3 ~ 0040.mp3 一一对应。
# 建议最终换成你自己录的声音（见 README「把爸妈的声音放进设备」）。
# 10 句「过程性表扬」，对应 SD 卡 0031~0040。
# 原则（德韦克实验）：描述行为，不评价人格。
#   ❌ 你真棒 / 你真聪明 / 你是最乖的
#   ✅ 我看到你自己去刷牙了，没有等人叫
PRESET_PRAISE = [
    "我看到你自己去刷牙了，没有等人叫。",
    "你今天先收了书包再玩，这是你自己安排的。",
    "今天有点累，你还是做完了，这叫坚持。",
    "你昨天说要做，今天真的做到了，说到做到。",
    "我看到你把书放回原位了，很细心。",
    "这一周你每天都记得，妈妈都看在眼里。",
    "你今天主动帮了妹妹，谢谢你。",
    "我不会催你，你自己决定什么时候开始。",
    "不管你完成没完成，我都很喜欢你。",
    "明天我们继续，慢慢来。",
]

# 里程碑：连续天数 → 语音编号（0021~0024）
MILESTONE_TRACKS = {3: 21, 7: 22, 14: 23, 30: 24}

# 能力徽章：描述"你会了什么"，不是"你得了多少分"
BADGES = [
    ("第一天", "完整做完一天", "days", 1),
    ("三天火苗", "连着三天没断", "best", 3),
    ("一周不倒", "连续七天", "best", 7),
    ("两周", "连续十四天", "best", 14),
    ("一个月", "连续三十天", "best", 30),
    ("习惯成自然", "累计打卡 50 次", "checkins", 50),
]


# ----------------------------- 数据库 -----------------------------
def db():
    conn = sqlite3.connect(DB_PATH, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


def init_db():
    conn = db()
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS habits(
            id      TEXT PRIMARY KEY,
            name    TEXT NOT NULL,
            emoji   TEXT DEFAULT '',
            sort    INTEGER DEFAULT 0,
            active  INTEGER DEFAULT 1,
            period  TEXT DEFAULT '',
            steps   TEXT DEFAULT '',
            rt_seq  TEXT DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS checkins(
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            habit_id  TEXT NOT NULL,
            day       TEXT NOT NULL,          -- YYYY-MM-DD
            ts        TEXT NOT NULL,          -- ISO 时间
            UNIQUE(habit_id, day)
        );
        CREATE TABLE IF NOT EXISTS approvals(
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            habit_id  TEXT DEFAULT '',
            message   TEXT NOT NULL,
            from_who  TEXT DEFAULT '爸爸妈妈',
            created   TEXT NOT NULL,
            slot      INTEGER DEFAULT 0,   -- 1~10 对应 SD 卡 0031~0040
            note      TEXT DEFAULT '',     -- 家长备注，只显示在家长页
            played    INTEGER DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS events(
            id      INTEGER PRIMARY KEY AUTOINCREMENT,
            kind    TEXT NOT NULL,
            detail  TEXT DEFAULT '',
            ts      TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS settings(
            key   TEXT PRIMARY KEY,
            value TEXT DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS zika_log(
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            char      TEXT NOT NULL UNIQUE,
            track     INTEGER NOT NULL,
            first_day TEXT NOT NULL,
            stage     INTEGER DEFAULT 0,     -- 已完成几次复习
            due_day   TEXT NOT NULL,         -- 下次复习日期
            last_day  TEXT DEFAULT '',
            retired   INTEGER DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS questions(
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            question  TEXT NOT NULL,
            answer    TEXT DEFAULT '',
            from_who  TEXT DEFAULT '孩子',
            status    TEXT DEFAULT 'ask',      -- ask / answered
            announced INTEGER DEFAULT 0,      -- 是否已经在设备上播过
            created   TEXT NOT NULL,
            answered  TEXT DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS milestones(
            day    TEXT NOT NULL,
            streak INTEGER NOT NULL,
            UNIQUE(day, streak)
        );
        """
    )
    # 旧库补列（本地自用，做一次幂等迁移即可）
    cols = {r["name"] for r in conn.execute("PRAGMA table_info(approvals)")}
    if "slot" not in cols:
        conn.execute("ALTER TABLE approvals ADD COLUMN slot INTEGER DEFAULT 0")
    if "note" not in cols:
        conn.execute("ALTER TABLE approvals ADD COLUMN note TEXT DEFAULT ''")
    if "track" not in cols:
        conn.execute("ALTER TABLE approvals ADD COLUMN track INTEGER DEFAULT 0")
        conn.execute("UPDATE approvals SET track = 30 + slot WHERE slot > 0")
    if "kind" not in cols:
        conn.execute("ALTER TABLE approvals ADD COLUMN kind TEXT DEFAULT 'praise'")
    qcols = {r["name"] for r in conn.execute("PRAGMA table_info(questions)")}
    if qcols and "announced" not in qcols:
        conn.execute("ALTER TABLE questions ADD COLUMN announced INTEGER DEFAULT 0")
    # 默认设置
    conn.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('child_name',?)", (CHILD_NAME,))
    conn.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('coach_name',?)", (COACH_NAME,))
    conn.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('zika_auto','1')")
    conn.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('card_kind','question')")
    conn.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('card_ref','')")

    cols = {r["name"] for r in conn.execute("PRAGMA table_info(habits)")}
    for c in ("period", "steps", "rt_seq"):
        if c not in cols:
            conn.execute("ALTER TABLE habits ADD COLUMN %s TEXT DEFAULT ''" % c)

    for i, row in enumerate(HABIT_PRESETS):
        hid, name, emoji = row[0], row[1], row[2]
        active = hid in DEFAULT_ACTIVE
        sort = (DEFAULT_ACTIVE.index(hid) + 1) if active else (100 + i)
        conn.execute(
            "INSERT OR IGNORE INTO habits(id,name,emoji,sort,active,period,steps,rt_seq)"
            " VALUES(?,?,?,?,?,?,?,?)",
            (hid, name, emoji, sort, 1 if active else 0, row[7], row[8], row[9]),
        )
        # 已在代码里删掉的预设，自动停用（比如"早饭"改到幼儿园吃）
        ids = tuple(row[0] for row in HABIT_PRESETS)
        conn.execute(
            "UPDATE habits SET active=0 WHERE id NOT IN (%s)" % ",".join("?" * len(ids)), ids
        )
        # 代码里的名称/图标/时段以代码为准（方便以后改文案）
        conn.execute(
            "UPDATE habits SET name=?, emoji=?, period=?, steps=?, rt_seq=? WHERE id=?",
            (name, emoji, row[7], row[8], row[9], hid),
        )
    conn.commit()
    conn.close()


def get_setting(conn, key, default=""):
    r = conn.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
    return (r["value"] if r and r["value"] else default)


def set_setting(conn, key, value):
    conn.execute(
        "INSERT INTO settings(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (key, (value or "")[:40]),
    )


def today_str():
    # 用一个明确的服务器时区，避免设备端时区问题
    return datetime.datetime.now().strftime("%Y-%m-%d")


def now_iso():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


# ----------------------------- 业务逻辑 -----------------------------
def get_habits(conn):
    """按一天的时间顺序返回启用中的习惯（设备上的星星也按这个顺序）。"""
    rows = conn.execute(
        "SELECT id,name,emoji,sort FROM habits WHERE active=1 ORDER BY sort"
    ).fetchall()
    out = [dict(r) for r in rows]
    def key(h):
        mt = habit_meta(h["id"])
        return ((mt["h"] * 60 + mt["m"]) if (mt["h"] or mt["m"]) else 9999)
    out.sort(key=key)
    return out


def today_state(conn):
    day = today_str()
    habits = get_habits(conn)
    done_rows = conn.execute(
        "SELECT habit_id, ts FROM checkins WHERE day=?", (day,)
    ).fetchall()
    done = {r["habit_id"]: r["ts"] for r in done_rows}

    active_ids = {h["id"] for h in habits}

    items = []
    for h in habits:
        items.append(
            {
                "id": h["id"],
                "name": h["name"],
                "emoji": h["emoji"],
                "done": h["id"] in done,
                "checked_at": done.get(h["id"]),
            }
        )

    streak = calc_streak(conn, day, active_ids)
    pending = conn.execute(
        "SELECT id, message, from_who, track, kind FROM approvals WHERE played=0 ORDER BY id LIMIT 5"
    ).fetchall()

    return {
        "day": day,
        "child_name": get_setting(conn, "child_name", CHILD_NAME),
        "coach_name": get_setting(conn, "coach_name", COACH_NAME),
        "habits": items,
        "done_count": len(active_ids & set(done.keys())),
        "total": len(habits),
        "streak": streak,
        "approvals": [dict(p) for p in pending],
        "server_time": now_iso(),
    }


def calc_streak(conn, day, active_ids):
    """连续「全部完成」的天数。今天没完成不算断，从昨天往回数。"""
    total = len(active_ids)
    if total <= 0:
        return 0
    d = datetime.date.fromisoformat(day)
    streak = 0
    first = True
    while True:
        ds = d.isoformat()
        rows = conn.execute("SELECT DISTINCT habit_id FROM checkins WHERE day=?", (ds,))
        n = len({r["habit_id"] for r in rows} & set(active_ids))
        if n >= total:
            streak += 1
        elif first:
            pass  # 今天还没做完，不打断连续记录
        else:
            break
        first = False
        d -= datetime.timedelta(days=1)
        if streak > 999:
            break
    return streak


def week_view(conn, days=7, active_ids=None):
    out = []
    today = datetime.date.today()
    for i in range(days - 1, -1, -1):
        d = (today - datetime.timedelta(days=i)).isoformat()
        rows = conn.execute("SELECT habit_id FROM checkins WHERE day=?", (d,)).fetchall()
        ids = {r["habit_id"] for r in rows}
        if active_ids is not None:
            ids = ids & active_ids
        out.append({"day": d, "count": len(ids)})
    return out


# ---------------- 识字包（5 岁半，30 个字）----------------
# 选字顺序不是按笔画，而是按"动机 > 生活会用到 > 象形":
#   先自己的名字和家人 → 身体 → 家里 → 自然 → 动作
# 语音编号 0101 起，一条一句"字 + 词例"。
ZIKA = [
    ("我", "我们", 101), ("爸", "爸爸", 102), ("妈", "妈妈", 103), ("家", "回家", 104),
    ("手", "小手", 105), ("口", "门口", 106), ("牙", "刷牙", 107), ("脚", "小脚", 108),
    ("头", "点头", 109),
    ("门", "大门", 118), ("床", "小床", 119), ("灯", "台灯", 120), ("杯", "水杯", 121),
    ("鞋", "鞋子", 122), ("衣", "衣服", 123),
    ("日", "生日", 110), ("月", "月亮", 111), ("山", "大山", 112), ("水", "喝水", 113),
    ("火", "火车", 114), ("木", "木头", 115), ("花", "花朵", 116), ("草", "小草", 117),
    ("走", "走路", 124), ("看", "看见", 125), ("吃", "吃饭", 126), ("玩", "玩具", 127),
    ("睡", "睡觉", 128), ("大", "大小", 129), ("小", "大小", 130),
]

# 习惯完成后自动推的字：在正确的场景里出现正确的字
HABIT_ZIKA = {
    "wake": "日",       # 起床 → 太阳的日
    "morning": "门",    # 出门 → 大门的门
    "water": "杯",      # 喝水 → 水杯的杯
    "outdoor": "草",    # 户外玩 → 小草的草
    "help": "手",       # 帮忙 → 小手的手
    "bathflow": "水",   # 洗澡 → 喝水的水
    "toys": "玩",       # 收玩具 → 玩具的玩
    "brush": "牙",      # 刷牙 → 刷牙的牙
    "book": "月",       # 睡前绘本 → 月亮的月
    "dress": "衣",      # 穿衣 → 衣服的衣
    "shoes": "鞋",      # 穿鞋 → 鞋子的鞋
}

# 间隔重复：学完的第二天、第 4 天、第 11 天、第 26 天各见一次，然后毕业。
# 5 岁半不做对错判定，只做"再见一次"的曝光式复习——不考试，只提醒。
REVIEW_INTERVALS = [1, 3, 7, 15]

QUESTION_PROMPT = """你是懂儿童发展的大人，正在帮一对父母回答 5 岁半男孩在公园提出的问题。

不要写百科全书。按下面三段输出，每段不超过两行，语言简单、具体、不吓人：

1) 一句话答案：孩子能听懂的具体事实，最好用一个他熟悉的比喻。
2) 一个反问：把球踢回去，让他继续想（不要反问他"你觉得呢"这种空话，要问具体的）。
3) 一个可以马上做的小动作：在公园或家里能立刻做的观察/实验。

如果问题涉及死亡、疾病、暴力、家庭困境、陌生人等敏感话题，在最前面加一行
【给家长】说明怎么回答更安全，再输出三段。"""


def llm_enabled():
    return bool(os.environ.get("OPENAI_API_KEY") or os.environ.get("DASHSCOPE_API_KEY")
                or os.environ.get("DEEPSEEK_API_KEY"))


def ask_ai(question):
    """调 LLM 生成"给家长用"的答案草稿。没配 key 返回 (None, 原因)。"""
    import urllib.request

    openai = os.environ.get("OPENAI_API_KEY")
    dash = os.environ.get("DASHSCOPE_API_KEY")
    deepseek = os.environ.get("DEEPSEEK_API_KEY")
    if openai:
        base, key, model = "https://api.openai.com/v1", openai, os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    elif dash:
        base, key, model = "https://dashscope.aliyuncs.com/compatible-mode/v1", dash, os.environ.get("DASHSCOPE_MODEL", "qwen-plus")
    elif deepseek:
        base, key, model = "https://api.deepseek.com/v1", deepseek, os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
    else:
        return None, "没有配置 API Key（OPENAI_API_KEY / DASHSCOPE_API_KEY / DEEPSEEK_API_KEY）"

    body = {
        "model": model,
        "messages": [
            {"role": "system", "content": QUESTION_PROMPT},
            {"role": "user", "content": "孩子问：" + question},
        ],
        "temperature": 0.7,
    }
    req = urllib.request.Request(
        base + "/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
    )
    try:
        with urllib.request.urlopen(req, timeout=40) as r:
            data = json.loads(r.read().decode("utf-8"))
        return data["choices"][0]["message"]["content"].strip(), ""
    except Exception as e:  # 网络/额度/key 错误都走这里
        return None, "调用失败：%s" % str(e)[:120]


def add_question(conn, question, from_who="孩子"):
    question = (question or "").strip()
    if not question:
        return False
    conn.execute(
        "INSERT INTO questions(question,from_who,status,created) VALUES(?,?,'ask',?)",
        (question[:300], from_who[:20], now_iso()),
    )
    conn.commit()
    return True


def answer_question(conn, qid, answer, announce=True):
    """保存答案。announce=True 时顺手给设备排一句语音提醒。"""
    answer = (answer or "").strip()
    if not answer:
        return False
    conn.execute(
        "UPDATE questions SET answer=?, status='answered', answered=? WHERE id=?",
        (answer[:2000], now_iso(), qid),
    )
    conn.commit()
    conn.execute("UPDATE questions SET announced=0 WHERE id=?", (qid,))
    conn.commit()
    if announce and claim_surprise(conn):
        clear_card_queue(conn)
        add_approval(conn, "", "小豆", track=64, kind="question")
        conn.execute("UPDATE questions SET announced=1 WHERE id=?", (qid,))
        mark_surprise(conn, "question", str(qid))
        conn.commit()
    return True


def latest_answered(conn, qid=None):
    if qid:
        row = conn.execute(
            "SELECT id,question,answer FROM questions WHERE id=?", (qid,)).fetchone()
    else:
        row = conn.execute(
            "SELECT id,question,answer FROM questions WHERE status='answered'"
            " ORDER BY id DESC LIMIT 1").fetchone()
    return dict(row) if row else None


def zika_of(idx):
    for row in ZIKA:
        if row[0] == idx or row[2] == idx:
            return row
    return None


def card_pages(conn, qid=None):
    """当前推送到设备的卡片：可能是一张问题卡，也可能是一张识字卡。"""
    kind = get_setting(conn, "card_kind", "question")
    if kind == "zika":
        row = zika_of(get_setting(conn, "card_ref", ""))
        if row:
            imgs = [cardrender.render_zika(row[0], row[1], pg)[0] for pg in (1, 2)]
            return {"id": row[2], "question": row[0], "answer": row[1]}, imgs, 2
        set_setting(conn, "card_kind", "question")
        conn.commit()

    q = latest_answered(conn, qid)
    if not q:
        return None, [], 0
    secs = cardrender.sections(q["answer"] or "")
    imgs = [cardrender.render_card(q["question"], q["answer"] or "", i + 1, len(secs))[0]
            for i in range(len(secs))]
    return q, imgs, len(imgs)


def pending_questions(conn, limit=6):
    rows = conn.execute(
        "SELECT id,question,answer,status,created,answered FROM questions"
        " ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    return [dict(r) for r in rows]


def auto_push_zika(conn, habit_id):
    """打卡成功后，顺手把这个习惯对应的字推到设备上。

    这就是"在正确的场景里出现正确的字"：刷完牙 → 星星亮 → 屏幕出现「牙」。
    """
    if get_setting(conn, "zika_auto", "1") != "1":
        return
    day = today_str()
    if get_setting(conn, "zika_new_day", "") == day:
        return          # 今天已经自动教过一个字了
    ch = HABIT_ZIKA.get(habit_id)
    if not ch:
        return
    row = zika_of(ch)
    if not row:
        return
    # 已经停在同一个字上就不重复推
    if get_setting(conn, "card_kind") == "zika" and get_setting(conn, "card_ref") == ch:
        return
    push_card(conn, ch, review=False)
    zika_learned(conn, ch)
    set_setting(conn, "zika_new_day", day)
    conn.commit()


def zika_learned(conn, char):
    """第一次教这个字时登记；已在队列里就不动。"""
    row = zika_of(char)
    if not row:
        return
    today = today_str()
    exists = conn.execute("SELECT 1 FROM zika_log WHERE char=?", (char,)).fetchone()
    if exists:
        return
    due = (datetime.date.today() + datetime.timedelta(days=REVIEW_INTERVALS[0])).isoformat()
    conn.execute(
        "INSERT OR IGNORE INTO zika_log(char,track,first_day,stage,due_day) VALUES(?,?,?,0,?)",
        (char, row[2], today, due),
    )
    conn.commit()


def due_reviews(conn, limit=5):
    today = today_str()
    rows = conn.execute(
        "SELECT char,track,stage,due_day,first_day FROM zika_log"
        " WHERE retired=0 AND due_day<=? ORDER BY due_day LIMIT ?", (today, limit)
    ).fetchall()
    return [dict(r) for r in rows]


def all_zika_log(conn, limit=30):
    rows = conn.execute(
        "SELECT char,track,stage,due_day,first_day,retired FROM zika_log"
        " ORDER BY retired, due_day LIMIT ?", (limit,)
    ).fetchall()
    return [dict(r) for r in rows]


def review_track(track):
    """0107（新学的牙）→ 0207（还记得吗？牙…）"""
    return 200 + (track - 100)


def clear_card_queue(conn):
    """卡片类消息只保留最新一条——旧的卡已经没意义，留着会挤掉新卡。"""
    conn.execute(
        "UPDATE approvals SET played=1 WHERE played=0"
        " AND kind IN ('zika','zika_review','question')"
    )


def push_card(conn, char, review=False):
    """把某张字卡设成"当前卡"，并排一句语音。"""
    row = zika_of(char)
    if not row:
        return False
    clear_card_queue(conn)
    set_setting(conn, "card_kind", "zika")
    set_setting(conn, "card_ref", char)
    track = review_track(row[2]) if review else row[2]
    add_approval(conn, "", "小豆", track=track, kind="zika_review" if review else "zika")
    conn.commit()
    return True


def advance_review(conn, char):
    """复习完一次，按间隔表排下一次；够了就毕业。"""
    r = conn.execute("SELECT stage FROM zika_log WHERE char=?", (char,)).fetchone()
    if not r:
        return
    stage = int(r["stage"]) + 1
    day = today_str()
    if stage >= len(REVIEW_INTERVALS):
        conn.execute("UPDATE zika_log SET stage=?, retired=1, last_day=? WHERE char=?",
                     (stage, day, char))
    else:
        nxt = (datetime.date.today()
               + datetime.timedelta(days=REVIEW_INTERVALS[stage])).isoformat()
        conn.execute("UPDATE zika_log SET stage=?, due_day=?, last_day=? WHERE char=?",
                     (stage, nxt, day, char))
    conn.commit()


def review_now(conn, char):
    r = conn.execute("SELECT 1 FROM zika_log WHERE char=? AND retired=0", (char,)).fetchone()
    if not r:
        return False
    push_card(conn, char, review=True)
    advance_review(conn, char)
    return True


def claim_surprise(conn):
    """每天只有一个"惊喜"名额。返回 True 表示今天还没用过。"""
    day = today_str()
    if get_setting(conn, "surprise_day", "") == day:
        return False
    set_setting(conn, "surprise_day", day)
    conn.commit()
    return True


def mark_surprise(conn, kind, ref=""):
    set_setting(conn, "surprise_kind", kind)
    set_setting(conn, "surprise_ref", ref)


def surprise_state(conn):
    """今天这个惊喜名额用掉了没有、用在了什么上。"""
    day = today_str()
    used = get_setting(conn, "surprise_day", "") == day
    kind = get_setting(conn, "surprise_kind", "") if used else ""
    ref = get_setting(conn, "surprise_ref", "") if used else ""
    names = {"question": "一个问题的答案", "review": "复习一张识字卡",
             "praise": "爸妈的一句话", "skip": "今天先不给"}
    return {
        "used": used,
        "kind": kind,
        "ref": ref,
        "label": names.get(kind, "已经用掉了"),
        "pending_question": conn.execute(
            "SELECT id,question FROM questions WHERE status='answered' AND announced=0 ORDER BY id LIMIT 1"
        ).fetchone(),
        "due_review": (due_reviews(conn, 1) or [None])[0],
    }


def push_daily_surprise(conn):
    """每天第一次同步时决定"今天的惊喜"。

    优先级就是产品主线：
      1. 孩子问的问题有答案了（好奇心 = 主线）
      2. 该复习的字卡到期（识字 = 顺带实现）
      3. 都没有 → 今天不给惊喜（安静也是一种设计）
    """
    day = today_str()
    if get_setting(conn, "surprise_day", "") == day:
        return ""
    st = surprise_state(conn)
    q = st["pending_question"]
    if q:
        clear_card_queue(conn)
        add_approval(conn, "", "小豆", track=64, kind="question")
        conn.execute("UPDATE questions SET announced=1 WHERE id=?", (q["id"],))
        mark_surprise(conn, "question", str(q["id"]))
        set_setting(conn, "surprise_day", day)
        conn.commit()
        return "question"
    r = st["due_review"]
    if r:
        push_card(conn, r["char"], review=True)
        advance_review(conn, r["char"])
        mark_surprise(conn, "review", r["char"])
        set_setting(conn, "surprise_day", day)
        conn.commit()
        return "review"
    return ""


def check_milestone(conn):
    """今天刚好全部完成，且连续天数撞上里程碑 → 排一条语音播报。"""
    s = today_state(conn)
    if s["total"] <= 0 or s["done_count"] < s["total"]:
        return
    st = s["streak"]
    track = MILESTONE_TRACKS.get(st)
    if not track:
        return
    day = today_str()
    if conn.execute("SELECT 1 FROM milestones WHERE day=? AND streak=?", (day, st)).fetchone():
        return
    conn.execute("INSERT INTO milestones(day,streak) VALUES(?,?)", (day, st))
    msg = "连续 %d 天达成！" % st
    add_approval(conn, msg, "小豆", track=track, kind="badge")
    conn.execute(
        "INSERT INTO events(kind,detail,ts) VALUES('milestone',?,?)", (str(st), now_iso())
    )
    conn.commit()


def stats(conn):
    """算连续天数、最长连续、累计完成天数、总打卡次数。"""
    active_ids = {h["id"] for h in get_habits(conn)}
    per_day = {}
    for r in conn.execute("SELECT day, habit_id FROM checkins").fetchall():
        if r["habit_id"] in active_ids:
            per_day[r["day"]] = per_day.get(r["day"], 0) + 1
    total = len(active_ids)
    total_checkins = sum(per_day.values())
    total_days = sum(1 for n in per_day.values() if n >= total) if total else 0

    best = 0
    run = 0
    prev = None
    for day in sorted(per_day):
        d = datetime.date.fromisoformat(day)
        if per_day[day] < total:
            run = 0
            prev = d
            continue
        if prev is not None and (d - prev).days == 1:
            run += 1
        else:
            run = 1
        best = max(best, run)
        prev = d

    cur = calc_streak(conn, today_str(), active_ids)
    best = max(best, cur)
    return {
        "streak": cur,
        "best": best,
        "days": total_days,
        "checkins": total_checkins,
    }


def badges(conn):
    st = stats(conn)
    out = []
    for name, desc, field, need in BADGES:
        out.append({
            "name": name,
            "desc": desc,
            "unlocked": st.get(field, 0) >= need,
            "progress": "%d/%d" % (min(st.get(field, 0), need), need),
        })
    return out


def review_text(conn):
    """给家长的一段周回顾——可直接截图发家庭群。"""
    habits = get_habits(conn)
    total = len(habits)
    active_ids = {h["id"] for h in habits}
    week = week_view(conn, 7, active_ids)
    st = stats(conn)
    if total <= 0:
        return "还没有设置习惯。"

    # 观察窗口：刚用几天就只按几天算，别拿 21 次去羞辱一个第 2 天的人
    first = conn.execute("SELECT MIN(day) AS d FROM checkins").fetchone()["d"]
    if first:
        observed = max(1, min(7, (datetime.date.today()
                                  - datetime.date.fromisoformat(first)).days + 1))
    else:
        observed = 7
    week = week[-observed:]

    done = sum(min(d["count"], total) for d in week)
    per_habit = {h["id"]: 0 for h in habits}
    names = {h["id"]: h["name"] for h in habits}
    for d in week:
        for r in conn.execute("SELECT habit_id FROM checkins WHERE day=?", (d["day"],)):
            if r["habit_id"] in per_habit:
                per_habit[r["habit_id"]] += 1

    lines = ["这 %d 天完成 %d/%d 次，连续 %d 天。"
             % (observed, done, total * observed, st["streak"])]
    counts = list(per_habit.values())
    if counts and max(counts) > 0:
        top = max(counts)
        if counts.count(top) == 1:
            b = max(per_habit, key=per_habit.get)
            lines.append("最稳的是「%s」（%d/%d）。" % (names[b], top, observed))
        else:
            lines.append("这几件事都保持在 %d/%d，很稳。" % (top, observed))
        low = min(counts)
        if low < top:
            worst = min(per_habit, key=per_habit.get)
            lines.append("最容易漏的是「%s」（%d/%d）——下次把提醒提前 10 分钟试试。"
                         % (names[worst], low, observed))

    # 最常漏的星期几
    miss = {}
    for d in week:
        if d["count"] < total:
            wd = datetime.date.fromisoformat(d["day"]).weekday()
            miss[wd] = miss.get(wd, 0) + (total - d["count"])
    if miss and max(miss.values()) >= 2:
        wd = max(miss, key=miss.get)
        lines.append("周%s最容易断，那天的节奏可能太赶。" % "一二三四五六日"[wd])
    if st["best"] >= 3:
        lines.append("最长连续 %d 天，这是你自己的记录。" % st["best"])
    return "\n".join(lines)


def album_data(conn):
    """成长档案：所有"沉淀"类的数据，不做任何考核。"""
    child = get_setting(conn, "child_name", CHILD_NAME)
    coach = get_setting(conn, "coach_name", COACH_NAME)

    # 字卡墙
    learned = {}
    for r in conn.execute("SELECT char,first_day,retired FROM zika_log"):
        learned[r["char"]] = dict(r)
    wall = []
    for row in ZIKA:
        ch = row[0]
        wall.append({
            "char": ch,
            "word": row[1],
            "learned": ch in learned,
            "first_day": learned[ch]["first_day"] if ch in learned else "",
            "retired": bool(learned[ch]["retired"]) if ch in learned else False,
        })

    # 问题册
    questions = [dict(r) for r in conn.execute(
        "SELECT id,question,answer,status,created,answered FROM questions ORDER BY id"
    )]

    # 习惯统计
    st = stats(conn)
    first_checkin = conn.execute("SELECT MIN(day) AS d FROM checkins").fetchone()["d"]
    first_zika = conn.execute("SELECT MIN(first_day) AS d FROM zika_log").fetchone()["d"]
    first_q = conn.execute("SELECT MIN(substr(created,1,10)) AS d FROM questions").fetchone()["d"]
    starts = [d for d in (first_checkin, first_zika, first_q) if d]
    start = min(starts) if starts else today_str()
    try:
        days = (datetime.date.today() - datetime.date.fromisoformat(start)).days + 1
    except ValueError:
        days = 1

    # 第一个"第一次"们
    firsts = []
    if first_checkin:
        row = conn.execute(
            "SELECT c.habit_id, h.name FROM checkins c LEFT JOIN habits h"
            " ON h.id=c.habit_id WHERE c.day=? ORDER BY c.id LIMIT 1", (first_checkin,)
        ).fetchone()
        firsts.append(("第一次做到", first_checkin, row["name"] if row else ""))
    if first_zika:
        row = conn.execute(
            "SELECT char FROM zika_log ORDER BY first_day, id LIMIT 1").fetchone()
        firsts.append(("认识的第一个字", first_zika, row["char"] if row else ""))
    if first_q:
        row = conn.execute(
            "SELECT question FROM questions ORDER BY id LIMIT 1").fetchone()
        firsts.append(("问的第一个问题", first_q, row["question"] if row else ""))

    return {
        "child": child, "coach": coach, "start": start, "days": days,
        "wall": wall,
        "learned_n": sum(1 for w in wall if w["learned"]),
        "total_chars": len(wall),
        "questions": questions,
        "q_answered": sum(1 for q in questions if q["answer"]),
        "checkins": st["checkins"], "best": st["best"], "cur": st["streak"],
        "firsts": firsts,
    }


ALBUM_CSS = """
:root{--ink:#111;--dim:#777;--line:#ddd;--gold:#b8860b}
*{box-sizing:border-box}
body{margin:0;background:#f7f5f0;color:var(--ink);
     font:16px/1.7 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:28px 20px 60px}
h1{font-size:26px;margin:0 0 4px;letter-spacing:1px}
.sub{color:var(--dim);font-size:13px}
.head{border-bottom:2px solid var(--ink);padding-bottom:14px;margin-bottom:26px}
.row3{display:flex;gap:22px;margin-top:14px;flex-wrap:wrap}
.stat b{font-size:26px;display:block;line-height:1.1}
.stat span{font-size:12px;color:var(--dim)}
h2{font-size:17px;margin:34px 0 14px;padding-left:10px;border-left:4px solid var(--gold)}
.wall{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}
.cell{aspect-ratio:1;border:1px solid var(--line);border-radius:10px;background:#fff;
      display:flex;flex-direction:column;align-items:center;justify-content:center;position:relative}
.cell.on{border-color:var(--gold);background:#fffdf6}
.cell b{font-size:30px;line-height:1;color:var(--gold)}
.cell i{font-style:normal;font-size:10px;color:var(--dim);margin-top:4px}
.cell.off b{color:#ddd}
.cell.off i{color:#ddd}
.cell .rk{position:absolute;top:4px;right:6px;font-size:9px;color:var(--gold)}
.q{border-left:3px solid var(--line);padding:2px 0 2px 14px;margin-bottom:22px}
.q .qt{font-size:16px;font-weight:600;margin-bottom:6px}
.q .qa{white-space:pre-wrap;font-size:14px;color:#333;background:#fff;border-radius:8px;padding:10px 12px}
.q .qw{font-size:13px;color:var(--dim)}
.q .qm{font-size:11px;color:var(--dim);margin-top:4px}
table.firsts{width:100%;border-collapse:collapse;font-size:14px}
table.firsts td{padding:8px 6px;border-bottom:1px solid var(--line)}
table.firsts td:first-child{color:var(--dim);width:130px}
.foot{margin-top:44px;padding-top:14px;border-top:1px solid var(--line);
      color:var(--dim);font-size:12px;text-align:center}
.nav{position:fixed;right:16px;bottom:16px;display:flex;gap:10px}
.nav a,.nav button{background:#111;color:#fff;border:0;border-radius:999px;
      padding:11px 16px;font-size:14px;text-decoration:none;cursor:pointer}
@media print{
  body{background:#fff}
  .nav{display:none}
  .wrap{padding:0;max-width:none}
  h2{page-break-after:avoid}
  .q{page-break-inside:avoid}
  .wall{page-break-inside:avoid}
}
"""


def render_album(conn):
    d = album_data(conn)
    wall_html = []
    for w in d["wall"]:
        cls = "cell on" if w["learned"] else "cell off"
        mark = '<span class="rk">✓</span>' if w["retired"] else ""
        wall_html.append('<div class="%s"><b>%s</b><i>%s</i>%s</div>'
                         % (cls, w["char"], w["word"], mark))

    firsts_html = "".join(
        "<tr><td>%s</td><td>%s · %s</td></tr>" % (name, day, val)
        for name, day, val in d["firsts"]
    )

    q_html = []
    for q in d["questions"]:
        if q["answer"]:
            body = '<div class="qa">%s</div>' % q["answer"].replace("<", "&lt;")
        else:
            body = '<div class="qw">还在等一个答案。</div>'
        q_html.append(
            '<div class="q"><div class="qt">「%s」</div>%s<div class="qm">%s</div></div>'
            % (q["question"], body, q["created"][:16])
        )
    q_block = "".join(q_html) if q_html else '<div class="qw">还没有问题。第一句很快就来了。</div>'

    return """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%(child)s 的成长档案</title><style>%(css)s</style></head><body>
<div class="wrap">
  <div class="head">
    <h1>%(child)s 的成长档案</h1>
    <div class="sub">从 %(start)s 开始 · 第 %(days)d 天 · 角色「%(coach)s」</div>
    <div class="row3">
      <div class="stat"><b>%(learned)d<small>/%(total)d</small></b><span>认识的字</span></div>
      <div class="stat"><b>%(qans)d<small>/%(qall)d</small></b><span>问过的问题</span></div>
      <div class="stat"><b>%(checkins)d</b><span>完成次数</span></div>
      <div class="stat"><b>%(best)d</b><span>最长连续</span></div>
    </div>
  </div>

  <h2>许多个「第一次」</h2>
  <table class="firsts">%(firsts)s</table>

  <h2>字卡墙 · 会了 %(learned)d 个字</h2>
  <div class="wall">%(wall)s</div>

  <h2>问题册 · 他问过的 %(qall)d 个问题</h2>
  %(questions)s

  <div class="foot">
    这份档案只存在这台电脑上 · 可以按右下角打印，装订成一本<br>
    它记录的不是成绩，是这一年他问过什么、做到了什么。
  </div>
</div>
<div class="nav"><a href="/">回主页</a><button onclick="window.print()">打印</button></div>
</body></html>""" % {
        "child": d["child"], "coach": d["coach"], "start": d["start"],
        "days": d["days"], "learned": d["learned_n"], "total": d["total_chars"],
        "qans": d["q_answered"], "qall": len(d["questions"]),
        "checkins": d["checkins"], "best": d["best"],
        "css": ALBUM_CSS, "firsts": firsts_html,
        "wall": "".join(wall_html), "questions": q_block,
    }


def check_in(conn, habit_id):
    day = today_str()
    exists = conn.execute(
        "SELECT 1 FROM habits WHERE id=? AND active=1", (habit_id,)
    ).fetchone()
    if not exists:
        return False, "未知习惯"
    try:
        conn.execute(
            "INSERT INTO checkins(habit_id,day,ts) VALUES(?,?,?)",
            (habit_id, day, now_iso()),
        )
        conn.execute(
            "INSERT INTO events(kind,detail,ts) VALUES('checkin',?,?)",
            (habit_id, now_iso()),
        )
        conn.commit()
        check_milestone(conn)
        auto_push_zika(conn, habit_id)
        return True, "ok"
    except sqlite3.IntegrityError:
        return False, "今天已经打过卡啦"


def add_approval(conn, message="", from_who="爸爸妈妈", habit_id="", slot=0,
                 note="", track=0, kind="praise"):
    """track = SD 卡里的语音编号（1~2999），设备直接播这一条。

    slot=1~10 是家长页的"第几句"，对应 0031~0040；
    track=19 是"爸妈给你留了一句话"的通用提示音。
    """
    message = (message or "").strip()
    if slot:
        slot = max(1, min(10, int(slot)))
        track = 30 + slot
        if not message:
            message = PRESET_PRAISE[slot - 1]
    if not track and message:
        track = 19
    if not message and not track:
        return False
    conn.execute(
        "INSERT INTO approvals(habit_id,message,from_who,created,slot,note,track,kind,played)"
        " VALUES(?,?,?,?,?,?,?,?,0)",
        (habit_id, message[:200], from_who[:20], now_iso(), slot,
         (note or "")[:200], int(track), kind),
    )
    conn.execute(
        "INSERT INTO events(kind,detail,ts) VALUES('approval',?,?)",
        (message[:60], now_iso()),
    )
    conn.commit()
    return True


def ack_approval(conn, approval_id):
    conn.execute("UPDATE approvals SET played=1 WHERE id=?", (approval_id,))
    conn.commit()
    return True


# ----------------------------- 设备端纯文本协议 -----------------------------
def state_as_text(conn):
    """ESP32 直接按行解析，不用 JSON 库，省内存也省事。

    DATE=2026-10-30
    CHILD=景阳
    COACH=豆豆
    STREAK=5
    DONE=2/3
    H=brush|刷牙|1|21:10
    H=pack|收书包|1|21:12
    H=read|亲子阅读|0|
    MSG=12|爸爸妈妈|今天你自己记得刷牙，爸爸看到了，很棒！
    END
    """
    s = today_state(conn)
    lines = [
        "DATE=" + s["day"],
        "CHILD=" + s["child_name"],
        "COACH=" + s["coach_name"],
        "STREAK=" + str(s["streak"]),
        "DONE=%d/%d" % (s["done_count"], s["total"]),
    ]
    # 最近一条审核过的问题卡有几页（设备据此进"看卡片"模式）
    _q, _imgs, _n = card_pages(conn)
    lines.append("Q=%d" % _n)

    # 近 7 天是否"全部完成"：给设备画 7 个格子（5 岁半看不懂数字，但看得懂格子填满）
    wk = week_view(conn, 7, set(h["id"] for h in get_habits(conn)))
    lines.append("WEEK=" + ",".join(
        "1" if (s["total"] > 0 and d["count"] >= s["total"]) else "0" for d in wk
    ))

    for h in s["habits"]:
        mt = habit_meta(h["id"])
        # H=id|名字|完成|时间|提醒语音|完成语音|时|分|语音序列|最短秒数
        lines.append(
            "H=%s|%s|%d|%s|%d|%d|%d|%d|%s|%d"
            % (
                h["id"], h["name"], 1 if h["done"] else 0,
                (h["checked_at"] or "")[-8:-3],
                mt["rt"], mt["dt"], mt["h"], mt["m"], mt["seq"], mt["minsec"],
            )
        )
    for a in s["approvals"]:
        lines.append("MSG=%d|%d|%s" % (a["id"], a.get("track", 0), a["from_who"]))
    lines.append("END")
    return "\n".join(lines) + "\n"


# ----------------------------- 家长端页面 -----------------------------
PAGE = """<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{{CHILD}}的习惯 · 家长端</title>
<style>
  :root{--bg:#111418;--card:#1b2027;--line:#2a313a;--fg:#e8edf3;--dim:#8b97a6;
        --ok:#3ddc97;--warn:#ffb454;--pink:#ff6b9d;--gold:#ffd166;}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font:16px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Helvetica Neue",sans-serif}
  .wrap{max-width:520px;margin:0 auto;padding:16px 16px 64px}
  h1{font-size:20px;margin:8px 0 2px}
  .sub{color:var(--dim);font-size:13px;margin-bottom:16px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;
        padding:16px;margin-bottom:14px}
  .streak{display:flex;align-items:baseline;gap:10px;justify-content:space-between}
  .streak b{font-size:40px;line-height:1;color:var(--warn)}
  .habit{display:flex;align-items:center;gap:12px;padding:10px 0;border-bottom:1px dashed var(--line)}
  .habit:last-child{border-bottom:0}
  .dot{width:26px;height:26px;border-radius:50%;border:2px solid var(--line);
       display:flex;align-items:center;justify-content:center;font-size:14px;color:var(--dim)}
  .dot.on{background:var(--ok);border-color:var(--ok);color:#06281c;font-weight:700}
  .name{flex:1}
  .time{color:var(--dim);font-size:13px}
  .week{display:grid;grid-template-columns:repeat(7,1fr);gap:8px;text-align:center}
  .day{background:#141a21;border-radius:10px;padding:8px 0;font-size:12px;color:var(--dim)}
  .day b{display:block;font-size:18px;color:var(--fg)}
  .day.full{background:#123a2b;color:#9fe6c6}
  .day.full b{color:var(--ok)}
  .badges{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  .badge{background:#141a21;border:1px solid var(--line);border-radius:12px;padding:10px 12px}
  .badge b{display:block;font-size:14px;color:var(--gold)}
  .badge span{font-size:11px;color:var(--dim)}
  .badge.locked{opacity:.32}
  .badge.locked b{color:var(--fg)}
  .review{white-space:pre-wrap;font-size:14px;line-height:1.75;background:#12171d;
          border-radius:10px;padding:12px;border-left:3px solid var(--pink)}
  .slots{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:10px}
  .slot{text-align:left;background:#232a33;color:var(--fg);border:1px solid var(--line);
        border-radius:12px;padding:9px 10px;font-size:12px;line-height:1.4;cursor:pointer}
  .slot:before{content:attr(data-slot)" · ";color:var(--dim);font-size:12px}
  .slot.on{border-color:var(--pink);background:#2a1a24}
  textarea,input[type=text]{width:100%;background:#12171d;color:var(--fg);border:1px solid var(--line);
        border-radius:12px;padding:12px;font:inherit}
  textarea{min-height:64px;resize:vertical}
  .send{width:100%;margin-top:10px;background:var(--pink);color:#2a0616;border:0;border-radius:12px;
        padding:13px;font-size:16px;font-weight:700;cursor:pointer}
  .send:disabled{opacity:.45}
  .sent{color:var(--ok);font-size:13px;min-height:20px;margin-top:8px}
  .msg{padding:10px 12px;background:#12171d;border-radius:10px;margin-bottom:8px;font-size:14px}
  .msg .who{color:var(--pink);font-size:12px}
  details summary{cursor:pointer;color:var(--dim);font-size:14px;list-style:none}
  details summary::-webkit-details-marker{display:none}
  details summary:before{content:"▸ ";color:var(--pink)}
  details[open] summary:before{content:"▾ "}
  details form{margin-top:12px}
  details label{display:block;font-size:13px;color:var(--dim);margin:10px 0 6px}
  .toggle{display:flex;align-items:center;gap:10px;padding:9px 0;border-bottom:1px dashed var(--line)}
  .toggle:last-of-type{border-bottom:0}
  .toggle input{width:22px;height:22px;accent-color:var(--ok)}
  .toggle span{flex:1;font-size:15px}
  .toggle em{font-style:normal;color:var(--dim);font-size:12px;white-space:nowrap}
  .group{font-size:12px;color:var(--dim);letter-spacing:2px;margin:14px 0 2px}
  .group:first-of-type{margin-top:2px}
  .stepsm{font-size:11px;color:var(--dim)}
  .q{border-top:1px dashed var(--line);padding:10px 0}
  .q:first-child{border-top:0}
  .qqt{font-size:15px;margin-bottom:6px}
  .qa{white-space:pre-wrap;font-size:14px;line-height:1.7;background:#12171d;
      border-radius:10px;padding:10px 12px;border-left:3px solid var(--ok)}
  .qwait{color:var(--dim);font-size:13px}
  .qbtns{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
  .qbtns button{background:#232a33;color:var(--fg);border:1px solid var(--line);
      border-radius:9px;padding:7px 11px;font-size:13px;cursor:pointer}
  .q textarea{min-height:90px;margin-top:8px}
  .zrow{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px dashed var(--line)}
  .zrow:last-child{border-bottom:0}
  .zrow b{font-size:20px;width:28px}
  .zst{flex:1;font-size:12px;color:var(--dim)}
  .mini{background:none;border:1px solid var(--line);color:var(--fg);border-radius:8px;
        padding:5px 9px;font-size:12px;cursor:pointer}
  .chips{display:flex;flex-wrap:wrap;gap:8px}
  .chip{width:44px;height:44px;font-size:21px;background:#232a33;color:var(--fg);
        border:1px solid var(--line);border-radius:10px;cursor:pointer}
  .chip:active{border-color:var(--gold)}
  .thumbs{display:flex;gap:10px;overflow-x:auto;padding-bottom:4px}
  .thumb{width:150px;height:150px;border-radius:50%;border:1px solid var(--line);flex:0 0 auto}
  .rv{margin:0}
  .rv button{background:none;border:1px solid var(--line);color:var(--dim);
             border-radius:8px;padding:4px 8px;font-size:12px;cursor:pointer}
  .foot{color:var(--dim);font-size:12px;text-align:center;margin-top:20px}
  .albumLink{display:block;background:var(--card);border:1px solid var(--gold);
        border-radius:14px;padding:14px 16px;margin-bottom:14px;color:var(--gold);
        text-decoration:none;font-size:15px}
</style></head><body><div class="wrap">
  <h1>{{CHILD}}</h1>
  <div class="sub">今天 {{DAY}} · 设备互动建议 ≤{{LIMIT}} 分钟</div>

  <!-- ================= 1. 今天的惊喜（一天只有一个） ================= -->
  <div class="card">
    <div class="sub" style="margin:0 0 8px">今天的惊喜 · 一天只有一个</div>
    {{SURPRISE}}
  </div>

  <!-- ================= 2. 今天要做的 ================= -->
  <div class="card">
    <div class="sub" style="margin:0 0 6px">{{DONE}}/{{TOTAL}} · 🔥{{STREAK}} 天</div>
    {{HABITS}}
  </div>

  <!-- ================= 3. 给他一句话 ================= -->
  <div class="card">
    <div class="sub" style="margin:0 0 10px">给 {{CHILD}} 的一句话 · 设备会念出来</div>
    <form method="post" action="/parent/praise">
      <input type="hidden" name="slot" id="slot" value="">
      <div class="slots">{{SLOTS}}</div>
      <textarea name="note" placeholder="备注（只显示在这里）"></textarea>
      <button class="send" id="sendBtn" type="submit" disabled>选一句，发到设备上 →</button>
    </form>
    <div class="sent">{{SENT}}</div>
  </div>

  <!-- ================= 4. 他的问题（主线：好奇心） ================= -->
  <details class="card">
    <summary>他的问题 · 问题罐（{{QCOUNT}} 条）</summary>
    <div style="margin-top:12px">
      <form method="post" action="/parent/question">
        <input type="text" name="question" placeholder="在公园问了什么？比如「为什么树叶会变黄」" maxlength="200">
        <button class="send" type="submit">记下来</button>
      </form>
      <div style="margin-top:6px">{{QUESTIONS}}</div>
      {{CARDS}}
    </div>
  </details>

  <!-- ================= 更多 ================= -->
  <a class="albumLink" href="/album">📖 成长档案 · 字卡墙 / 问题册 / 打印 →</a>

  <details class="card">
    <summary>更多：进度 / 识字 / 设置</summary>

    <div style="margin-top:14px">
      <div class="sub" style="margin:0 0 8px">最近 7 天</div>
      <div class="week">{{WEEK}}</div>
    </div>

    <div class="sub" style="margin:18px 0 8px">本周回顾</div>
    <div class="review">{{REVIEW}}</div>

    <div class="sub" style="margin:18px 0 8px">能力徽章</div>
    <div class="badges">{{BADGES}}</div>

    <div class="sub" style="margin:22px 0 8px">认一个字 · 只认不写</div>
    <div class="chips">{{ZIKA}}</div>
    <div class="sub" style="margin-top:10px">已和习惯绑定：{{ZIKAMAP}}</div>
    <div class="sub" style="margin-top:14px">复习队列 · {{REVIEWSUM}}</div>
    <div class="qlist">{{REVIEWLIST}}</div>
    <form method="post" action="/parent/zika_auto">
      <label class="toggle"><input type="checkbox" name="on" value="1" {{AUTOCHECK}}>
        <span>习惯完成后自动推一张字卡</span><em>推荐开</em></label>
      <button class="send" type="submit">保存</button>
    </form>

    <div class="sub" style="margin:22px 0 8px">最近发出的</div>
    {{APPROVALS}}

    <div class="sub" style="margin:22px 0 8px">这个月开几件事（{{ACTIVECOUNT}}/{{MAXH}}）</div>
    <form method="post" action="/parent/habits">
      {{HABITTOGGLES}}
      <button class="send" type="submit">保存</button>
    </form>
    <div class="sub" style="margin-top:8px">
      建议从 1 件开始，稳定两周再加。最多 {{MAXH}} 件。
    </div>

    <form method="post" action="/parent/settings" style="margin-top:20px">
      <div class="sub" style="margin:0 0 8px">名字</div>
      <label class="lbl">孩子</label>
      <input type="text" name="child_name" value="{{CHILD}}" maxlength="12">
      <label class="lbl">角色</label>
      <input type="text" name="coach_name" value="{{COACH}}" maxlength="12">
      <button class="send" type="submit">保存</button>
    </form>
  </details>

  <div class="foot">
    主线：习惯（每天做到）+ 好奇心（想问就问）· 识字是顺带实现的<br>
    数据只存在这台电脑上，不联网、不上传。
  </div>
</div>
<script>
  var slotInput = document.getElementById('slot');
  var sendBtn = document.getElementById('sendBtn');
  document.querySelectorAll('.slot').forEach(function(b){
    b.addEventListener('click', function(){
      document.querySelectorAll('.slot').forEach(function(x){ x.classList.remove('on'); });
      b.classList.add('on');
      slotInput.value = b.dataset.slot;
      sendBtn.disabled = false;
      sendBtn.textContent = '发送第 ' + b.dataset.slot + ' 句到设备 →';
    });
  });
</script>
</body></html>
"""


def habit_toggles(conn):
    """按"早上 / 白天 / 晚上 / 单独版"分组列出全部预设。"""
    act = {r["id"] for r in conn.execute("SELECT id FROM habits WHERE active=1")}
    out = []
    for key, title in PERIODS:
        rows = [r for r in HABIT_PRESETS if r[7] == key]
        if not rows:
            continue
        out.append('<div class="group">%s</div>' % title)
        for row in rows:
            hid, name, emoji = row[0], row[1], row[2]
            when = "不提醒" if row[3] == 0 else "%02d:%02d" % (row[3], row[4])
            if len(row) > 10 and row[10] > 0:
                when += " · 计时%d分" % max(1, round(row[10] / 60))
            steps = ('<br><span class="stepsm">%s</span>' % row[8]) if row[8] else ""
            out.append(
                '<label class="toggle"><input type="checkbox" name="on" value="%s" %s>'
                '<span>%s %s%s</span><em>%s</em></label>'
                % (hid, "checked" if hid in act else "", emoji, name, steps, when)
            )
    return "".join(out)


def render_page(conn, sent=""):
    s = today_state(conn)
    st = stats(conn)

    habits_html = []
    for h in s["habits"]:
        cls = "dot on" if h["done"] else "dot"
        mark = "✓" if h["done"] else ""
        t = (h["checked_at"] or "")[-8:-3] if h["done"] else "还没完成"
        pst = preset_of(h["id"])
        steps = ('<br><span class="stepsm">%s</span>' % pst[8]) if (pst and pst[8]) else ""
        rv = ""
        if h["done"]:
            rv = ('<form method="post" action="/parent/revoke" class="rv" '
                  'onsubmit="return confirm(\'撤回「%s」今天的记录？\')">'
                  '<input type="hidden" name="habit" value="%s">'
                  '<button type="submit">撤回</button></form>'
                  % (h["name"], h["id"]))
        habits_html.append(
            '<div class="habit"><div class="%s">%s</div>'
            '<div class="name">%s %s%s</div><div class="time">%s</div>%s</div>'
            % (cls, mark, h["emoji"], h["name"], steps, t, rv)
        )

    week_html = []
    for d in week_view(conn, 7):
        dd = datetime.date.fromisoformat(d["day"])
        label = "一二三四五六日"[dd.weekday()]
        full = s["total"] > 0 and d["count"] >= s["total"]
        week_html.append(
            '<div class="day %s">%s<b>%d</b></div>'
            % ("full" if full else "", label, d["count"])
        )

    badges_html = []
    for b in badges(conn):
        badges_html.append(
            '<div class="badge %s"><b>%s</b><span>%s · %s</span></div>'
            % ("" if b["unlocked"] else "locked",
               ("★ " if b["unlocked"] else "") + b["name"], b["desc"], b["progress"])
        )

    slots_html = "".join(
        '<button type="button" class="slot" data-slot="%d">%s</button>' % (i + 1, p)
        for i, p in enumerate(PRESET_PRAISE)
    )

    rows = conn.execute(
        "SELECT message,from_who,created,played,track,kind "
        "FROM approvals ORDER BY id DESC LIMIT 6"
    ).fetchall()
    if rows:
        approvals_html = "".join(
            '<div class="msg"><div class="who">%s · %s · %s %s</div>%s</div>'
            % (
                r["from_who"],
                r["created"][5:16],
                ("徽章 #%d" % r["track"]) if r["kind"] == "badge" else ("语音 #%d" % r["track"]),
                "· 已播放" if r["played"] else "· 待播放",
                r["message"] or "（语音）",
            )
            for r in rows
        )
    else:
        approvals_html = '<div class="sub">还没有，发第一句吧。</div>'

    qs_html = []
    for q in pending_questions(conn, 8):
        ans = q["answer"].strip()
        if ans:
            body = '<div class="qa">%s</div>' % ans.replace("<", "&lt;")
        else:
            body = '<div class="qwait">还没答案 · 点下面让 AI 先写一版，或者你自己写</div>'
        qs_html.append(
            '<details class="q"><summary class="qqt">「%s」<span class="qwait">%s</span></summary>'
            '%s'
            '<form method="post" action="/parent/answer">'
            '<input type="hidden" name="id" value="%d">'
            '<textarea name="answer" placeholder="写给家长自己看的答案（三段：一句话 / 反问 / 一起做的小动作）">%s</textarea>'
            '<div class="qbtns">'
            '<button type="submit">保存并在设备播报</button>'
            '</div></form>'
            '<form method="post" action="/parent/ask_ai"><input type="hidden" name="id" value="%d">'
            '<div class="qbtns"><button type="submit">让 AI 先写一版</button></div></form>'
            '</details>'
            % (q["question"], q["created"][5:16], body, q["id"], ans, q["id"])
        )
    questions_html = "".join(qs_html) if qs_html else '<div class="qwait">还没有问题。他在公园问的第一句，记在这里。</div>'

    sur = surprise_state(conn)
    sq = sur["pending_question"]
    dr = sur["due_review"]
    if sur["used"]:
        surprise_html = '<div style="font-size:15px">今天已经给了：<b>%s</b></div>' % sur["label"]
        nxt = []
        if sq:
            nxt.append("一个问题的答案")
        if dr:
            nxt.append("复习字卡「%s」" % dr["char"])
        surprise_html += ('<div class="sub" style="margin-top:6px">今天的名额用完了%s</div>'
                          % ("，明天再说：" + "、".join(nxt) if nxt else "。"))
    else:
        cand = []
        if sq:
            cand.append("一个问题的答案：「%s」" % sq["question"][:14])
        if dr:
            cand.append("复习字卡「%s」" % dr["char"])
        if cand:
            surprise_html = '<div style="font-size:15px">还没给 · 下一个：<b>%s</b></div>' % cand[0]
            surprise_html += ('<form method="post" action="/parent/surprise_push" style="margin-top:10px">'
                              '<div class="qbtns"><button type="submit">现在就给</button></div></form>')
            surprise_html += ('<form method="post" action="/parent/surprise_skip">'
                              '<div class="qbtns"><button type="submit">今天算了</button></div></form>')
        else:
            surprise_html = ('<div class="sub" style="margin:0">今天没有要给的惊喜。'
                             '安静也是一种设计——明天有了再说。</div>')

    q_count = conn.execute("SELECT COUNT(*) AS n FROM questions").fetchone()["n"]

    zika_map = " · ".join(
        "%s→%s" % (preset_of(hid)[1] if preset_of(hid) else hid, ch)
        for hid, ch in HABIT_ZIKA.items()
    )
    zika_auto_on = get_setting(conn, "zika_auto", "1") == "1"

    logs = all_zika_log(conn, 40)
    due_n = len(due_reviews(conn, 99))
    z_retired = sum(1 for l in logs if l["retired"])
    review_sum = "%d 个字学过 · %d 个今天到期 · %d 个已毕业" % (len(logs), due_n, z_retired)
    rl = []
    for l in logs:
        if l["retired"]:
            state_txt = "已毕业"
            extra = ""
        else:
            stage = int(l["stage"])
            state_txt = "第 %d/%d 次复习 · 下次 %s" % (
                min(stage + 1, len(REVIEW_INTERVALS)), len(REVIEW_INTERVALS), l["due_day"][5:])
            extra = ('<form method="post" action="/parent/review" style="display:inline">'
                     '<input type="hidden" name="char" value="%s">'
                     '<button class="mini">现在复习</button></form>' % l["char"])
        retire = ""
        if not l["retired"]:
            retire = ('<form method="post" action="/parent/zika_retire" style="display:inline">'
                      '<input type="hidden" name="char" value="%s">'
                      '<button class="mini">学会了</button></form>' % l["char"])
        rl.append('<div class="zrow"><b>%s</b><span class="zst">%s</span>%s%s</div>'
                  % (l["char"], state_txt, extra, retire))
    review_list = "".join(rl) if rl else '<div class="qwait">还没有学的字。点上面的字开始。</div>'

    zika_html = "".join(
        '<form method="post" action="/parent/zika" style="display:inline">'
        '<input type="hidden" name="idx" value="%s"><button class="chip" title="%s">%s</button></form>'
        % (row[2], row[1], row[0])
        for row in ZIKA
    )

    _q, _imgs, _n = card_pages(conn)
    if _imgs:
        thumbs = "".join(
            '<img class="thumb" src="/api/card.png?i=%d&t=%d" alt="卡片 %d">' % (i, int(_q["id"]), i + 1)
            for i in range(_n)
        )
        cards_html = (
            '<div class="sub" style="margin:12px 0 8px">问题卡预览（设备上就是这样显示的，%d 页）</div>'
            '<div class="thumbs">%s</div>'
            '<form method="post" action="/parent/push_card">'
            '<div class="qbtns"><button type="submit">在设备上播放这张卡</button></div></form>'
            % (_n, thumbs)
        )
    else:
        cards_html = '<div class="qwait" style="margin-top:10px">写一条答案后，这里会预览设备上的问题卡。</div>'

    page = PAGE
    for k, v in {
        "{{CHILD}}": s["child_name"],
        "{{COACH}}": s["coach_name"],
        "{{DAY}}": s["day"],
        "{{LIMIT}}": str(DAILY_LIMIT_MIN),
        "{{STREAK}}": str(st["streak"]),
        "{{BEST}}": str(st["best"]),
        "{{DONE}}": str(s["done_count"]),
        "{{TOTAL}}": str(s["total"]),
        "{{HABITS}}": "".join(habits_html),
        "{{WEEK}}": "".join(week_html),
        "{{BADGES}}": "".join(badges_html),
        "{{REVIEW}}": review_text(conn),
        "{{SLOTS}}": slots_html,
        "{{APPROVALS}}": approvals_html,
        "{{HABITTOGGLES}}": habit_toggles(conn),
        "{{ACTIVECOUNT}}": str(s["total"]),
        "{{MAXH}}": str(MAX_HABITS),
        "{{QUESTIONS}}": questions_html,
        "{{CARDS}}": cards_html,
        "{{ZIKA}}": zika_html,
        "{{SURPRISE}}": surprise_html,
        "{{QCOUNT}}": str(q_count),
        "{{ZIKAMAP}}": zika_map,
        "{{REVIEWSUM}}": review_sum,
        "{{REVIEWLIST}}": review_list,
        "{{AUTOCHECK}}": "checked" if zika_auto_on else "",
        "{{SENT}}": sent,
    }.items():
        page = page.replace(k, v)
    return page


# ----------------------------- HTTP -----------------------------
class Handler(BaseHTTPRequestHandler):
    server_version = "HabitCoach/1.0"

    def log_message(self, fmt, *args):  # 安静一点
        pass

    # ---------- 工具 ----------
    def _send(self, code, body, ctype="text/plain; charset=utf-8", headers=None):
        raw = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)

    def _query(self):
        u = urllib.parse.urlparse(self.path)
        return u.path, urllib.parse.parse_qs(u.query)

    @staticmethod
    def _int(v):
        try:
            return int((v or ["0"])[0])
        except (ValueError, TypeError):
            return 0

    def _form(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode("utf-8") if n else ""
        return urllib.parse.parse_qs(raw)

    # ---------- GET ----------
    def do_GET(self):
        path, q = self._query()
        conn = db()
        try:
            if path in ("/", "/index.html"):
                code = (q.get("sent") or [""])[0]
                sent = {
                    "1": "✅ 已发到设备上，等孩子下次完成任务就会听到",
                    "habsaved": "✅ 已保存，设备 15 秒内会自动同步（或长按 B 立即同步）",
                    "habmax": "⚠️ 最多只能开 %d 件，这次没有保存。先关掉几件再试。" % MAX_HABITS,
                    "hab0": "⚠️ 至少要开 1 件。",
                    "revoked": "↩️ 已撤回该记录（只在家长页留痕，设备端不会提示孩子）",
                    "qadd": "✅ 记下了。回家再一起找答案——别在路上就给答案，先问问他怎么想。",
                    "qai": "🤖 AI 写了一版草稿，改完再点「保存并在设备播报」。",
                    "qai_no": "⚠️ 没配置 AI 的 API Key，先自己写吧（或看 README 里怎么配）。",
                    "qans": "✅ 答案已保存，设备下次同步会提醒他「一起看看答案」。",
                    "qpush": "📺 已推送到设备：会先播一句，然后屏幕上出现问题卡（B 翻页，A 退出）。",
                    "qnon": "⚠️ 还没有审核过的答案，先写一条。",
                    "zikaauto": "✅ 已保存自动推送设置。",
                    "spush": "🎁 今天的惊喜已经推到设备上了。",
                    "snone": "今天没有可给的惊喜（问题答案和复习都没到期）。",
                    "sskip": "今天不给了。设备会安静一天。",
                    "zreview": "🔁 已推送复习卡（会播「还记得这个字吗？」）。",
                    "zretire": "🎓 这个字已标记学会，不再自动复习。",
                    "zika": "📖 已推到设备：先念这个字，屏幕上出现字卡（B 翻页，A 退出）。一天一个，明天先复习。",
                }.get(code, "")
                return self._send(200, render_page(conn, sent), "text/html; charset=utf-8")

            if path == "/album":
                return self._send(200, render_album(conn), "text/html; charset=utf-8")

            if path == "/api/card.png":
                qid = self._int(q.get("id"))
                idx = self._int(q.get("i"))
                _q, imgs, n = card_pages(conn, qid or None)
                if not imgs or idx >= len(imgs):
                    return self._send(404, "no card")
                buf = io.BytesIO()
                imgs[idx].save(buf, format="PNG")
                return self._send(200, buf.getvalue(), "image/png")

            if path == "/api/card.rgb565":
                qid = self._int(q.get("id"))
                idx = self._int(q.get("i"))
                _q, imgs, n = card_pages(conn, qid or None)
                if not imgs or idx >= len(imgs):
                    return self._send(404, "no card", "text/plain")
                raw = cardrender.to_rgb565(imgs[idx])   # 240*240*2 = 115200 字节
                return self._send(200, raw, "application/octet-stream")

            if path == "/api/state.txt":
                push_daily_surprise(conn)        # 每天一个惊喜，幂等
                return self._send(200, state_as_text(conn))

            if path == "/api/state":
                return self._send(200, json.dumps(today_state(conn), ensure_ascii=False), "application/json; charset=utf-8")

            if path == "/api/week":
                return self._send(200, json.dumps(week_view(conn, 14), ensure_ascii=False), "application/json; charset=utf-8")

            if path == "/healthz":
                return self._send(200, "ok")

            return self._send(404, "not found")
        finally:
            conn.close()

    # ---------- POST ----------
    def do_POST(self):
        path, q = self._query()
        conn = db()
        try:
            if path == "/api/checkin.txt" or path == "/api/checkin":
                habit = (q.get("habit") or [""])[0]
                ok, msg = check_in(conn, habit)
                # 设备端只关心结果，返回一行
                return self._send(200 if ok else 409, ("OK" if ok else "ERR " + msg) + "\n")

            if path == "/api/ack.txt" or path == "/api/ack":
                try:
                    aid = int((q.get("id") or ["0"])[0])
                except ValueError:
                    aid = 0
                ack_approval(conn, aid)
                return self._send(200, "OK\n")

            if path == "/api/praise":
                form = self._form()
                slot = self._int(form.get("slot"))
                text = (form.get("message") or [""])[0]
                add_approval(conn, text, form.get("from_who", ["爸爸妈妈"])[0],
                             slot=slot, note=(form.get("note") or [""])[0])
                return self._send(200, '{"ok":true}', "application/json; charset=utf-8")

            if path == "/parent/habits":
                form = self._form()
                on = [h for h in (form.get("on") or []) if any(r[0] == h for r in HABIT_PRESETS)]
                if not on:
                    return self._send(303, "", headers={"Location": "/?sent=hab0"})
                if len(on) > MAX_HABITS:
                    return self._send(303, "", headers={"Location": "/?sent=habmax"})
                conn.execute("UPDATE habits SET active=0")
                for i, hid in enumerate(on):
                    conn.execute("UPDATE habits SET active=1, sort=? WHERE id=?", (i + 1, hid))
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=habsaved"})

            if path == "/parent/surprise_push":
                got = push_daily_surprise(conn)
                code = "spush" if got else "snone"
                return self._send(303, "", headers={"Location": "/?sent=" + code})

            if path == "/parent/surprise_skip":
                if claim_surprise(conn):
                    mark_surprise(conn, "skip")
                    conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=sskip"})

            if path == "/parent/review":
                form = self._form()
                review_now(conn, (form.get("char") or [""])[0])
                return self._send(303, "", headers={"Location": "/?sent=zreview"})

            if path == "/parent/zika_retire":
                form = self._form()
                ch = (form.get("char") or [""])[0]
                conn.execute("UPDATE zika_log SET retired=1 WHERE char=?", (ch,))
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=zretire"})

            if path == "/parent/zika_auto":
                form = self._form()
                on = "1" if (form.get("on") or []) and "1" in form.get("on") else "0"
                set_setting(conn, "zika_auto", on)
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=zikaauto"})

            if path == "/parent/zika":
                form = self._form()
                idx = self._int(form.get("idx"))
                row = zika_of(idx)
                if not row:
                    return self._send(303, "", headers={"Location": "/?sent=qnon"})
                push_card(conn, row[0], review=False)
                zika_learned(conn, row[0])
                return self._send(303, "", headers={"Location": "/?sent=zika"})

            if path == "/parent/push_card":
                q, imgs, n = card_pages(conn)
                if not imgs:
                    return self._send(303, "", headers={"Location": "/?sent=qnon"})
                clear_card_queue(conn)
                add_approval(conn, "", "小豆", track=64, kind="question")
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=qpush"})

            if path == "/parent/question":
                form = self._form()
                add_question(conn, (form.get("question") or [""])[0])
                return self._send(303, "", headers={"Location": "/?sent=qadd"})

            if path == "/parent/ask_ai":
                form = self._form()
                qid = self._int(form.get("id"))
                row = conn.execute("SELECT question FROM questions WHERE id=?", (qid,)).fetchone()
                if not row:
                    return self._send(303, "", headers={"Location": "/?sent=qai_no"})
                draft, err = ask_ai(row["question"])
                if not draft:
                    return self._send(303, "", headers={"Location": "/?sent=qai_no"})
                conn.execute("UPDATE questions SET answer=?, status='draft' WHERE id=?",
                             (draft[:2000], qid))
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=qai"})

            if path == "/parent/answer":
                form = self._form()
                qid = self._int(form.get("id"))
                txt = (form.get("answer") or [""])[0]
                answer_question(conn, qid, txt, announce=True)
                return self._send(303, "", headers={"Location": "/?sent=qans"})

            if path == "/parent/revoke":
                form = self._form()
                hid = (form.get("habit") or [""])[0]
                day = (form.get("day") or [today_str()])[0]
                conn.execute("DELETE FROM checkins WHERE habit_id=? AND day=?", (hid, day))
                conn.execute("INSERT INTO events(kind,detail,ts) VALUES('revoke',?,?)",
                             ("%s@%s" % (hid, day), now_iso()))
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=revoked"})

            if path == "/parent/settings":
                form = self._form()
                set_setting(conn, "child_name", (form.get("child_name") or [""])[0])
                set_setting(conn, "coach_name", (form.get("coach_name") or [""])[0])
                conn.commit()
                return self._send(303, "", headers={"Location": "/?sent=1"})

            if path == "/parent/praise":
                form = self._form()
                slot = self._int(form.get("slot"))
                text = (form.get("message") or [""])[0]
                add_approval(conn, text, slot=slot, note=(form.get("note") or [""])[0])
                # PRG（Post/Redirect/Get），避免刷新重复提交
                return self._send(303, "", headers={"Location": "/?sent=1"})

            return self._send(404, "not found")
        finally:
            conn.close()


def main():
    init_db()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("=" * 56)
    print(" 习惯伙伴 · 家长端已启动")
    print(" 本机打开：  http://127.0.0.1:%d" % PORT)
    print(" 手机打开：  http://<这台电脑的局域网IP>:%d" % PORT)
    print(" 设备接口：  /api/state.txt")
    print(" 数据文件：  %s" % DB_PATH)
    print("=" * 56)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")


if __name__ == "__main__":
    main()
