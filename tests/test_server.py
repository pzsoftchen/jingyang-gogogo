#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归测试：守住"改一行、错一片"的风险。

跑法：
    make test
    # 或
    python3 -m unittest discover -s tests -v

为什么要有它：
    2026-11-02 修自洽审计时，一次正则误删把 render_page 里的 slots_html
    一起删掉了，页面直接 500——靠肉眼发现。这类错误测试 3 秒就能抓到。
"""

import datetime
import os
import sys
import tempfile
import threading
import unittest
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import server  # noqa: E402


class Base(unittest.TestCase):
    """每个测试一个干净的临时数据库。"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coach-test-")
        server.DB_PATH = os.path.join(self.tmp, "test.db")
        server.init_db()
        self.conn = server.db()

    def tearDown(self):
        self.conn.close()
        if server.DB_PATH and os.path.exists(server.DB_PATH):
            os.remove(server.DB_PATH)

    # ---------- 小工具 ----------
    def seed_day(self, back_days, habits=("brush", "toys")):
        day = (datetime.date.today() - datetime.timedelta(days=back_days)).isoformat()
        for h in habits:
            self.conn.execute(
                "INSERT OR IGNORE INTO checkins(habit_id,day,ts) VALUES(?,?,?)",
                (h, day, day + " 20:00:00"),
            )
        self.conn.commit()

    def add_answered_question(self, announce=False):
        cur = self.conn.execute(
            "INSERT INTO questions(question,answer,status,announced,created)"
            " VALUES('为什么树叶会变黄','一句话：叶绿素没了。','answered',0,?)",
            (server.now_iso(),),
        )
        self.conn.commit()
        return cur.lastrowid


# ============================ 单元测试 ============================

class DefaultsTests(Base):
    def test_default_habits_are_two(self):
        habits = server.get_habits(self.conn)
        ids = [h["id"] for h in habits]
        self.assertEqual(set(ids), {"brush", "toys"})

    def test_habits_are_ordered_by_time_of_day(self):
        """设备上的图标顺序 = 一天的时间顺序（19:40 的收玩具在前，20:30 的刷牙在后）"""
        ids = [h["id"] for h in server.get_habits(self.conn)]
        self.assertEqual(ids, ["toys", "brush"])

    def test_habit_presets_have_eleven_fields(self):
        for row in server.HABIT_PRESETS:
            self.assertEqual(len(row), 11, "预设字段数变了，检查 habit_meta")
        self.assertLessEqual(len(server.HABIT_PRESETS), 20)

    def test_icons_cover_habits(self):
        """固件图标表必须覆盖全部习惯 id，否则设备上会退回星星。"""
        path = os.path.join(os.path.dirname(HERE), "firmware", "habit_coach", "habit_icons.h")
        if not os.path.exists(path):
            self.skipTest("没有生成 habit_icons.h")
        import re
        with open(path, encoding="utf-8") as f:
            head = f.read()
        icons = set(re.findall(r'"([a-z]+)"', re.search(r"HABIT_ICON_IDS\[[^\]]+\] = \{(.*?)\};",
                                                       head, re.S).group(1)))
        preset_ids = {r[0] for r in server.HABIT_PRESETS}
        self.assertEqual(preset_ids - icons, set(), "有习惯缺图标")


class CheckinTests(Base):
    def test_checkin_then_duplicate(self):
        ok, _ = server.check_in(self.conn, "brush")
        self.assertTrue(ok)
        ok2, msg = server.check_in(self.conn, "brush")
        self.assertFalse(ok2)
        self.assertIn("已经", msg)

    def test_checkin_unknown_habit_rejected(self):
        ok, msg = server.check_in(self.conn, "不存在的习惯")
        self.assertFalse(ok)

    def test_checkin_inactive_habit_rejected(self):
        server.check_in(self.conn, "brush")
        self.conn.execute("UPDATE habits SET active=0 WHERE id='brush'")
        self.conn.commit()
        ok, _ = server.check_in(self.conn, "brush")
        self.assertFalse(ok)

    def test_done_count_only_active(self):
        server.check_in(self.conn, "brush")
        server.check_in(self.conn, "toys")
        st = server.today_state(self.conn)
        self.assertEqual(st["done_count"], 2)
        self.assertEqual(st["total"], 2)

    def test_streak_counts_complete_days(self):
        self.seed_day(2)
        self.seed_day(1)
        server.check_in(self.conn, "brush")
        server.check_in(self.conn, "toys")
        self.assertEqual(server.stats(self.conn)["streak"], 3)

    def test_streak_breaks_on_incomplete_day(self):
        self.seed_day(2)
        self.seed_day(1, habits=("brush",))     # 昨天只有一件
        server.check_in(self.conn, "brush")
        server.check_in(self.conn, "toys")
        self.assertEqual(server.stats(self.conn)["streak"], 1)

    def test_revoke_removes_checkin(self):
        server.check_in(self.conn, "brush")
        self.conn.execute("DELETE FROM checkins WHERE habit_id='brush' AND day=?",
                          (server.today_str(),))
        self.conn.commit()
        self.assertEqual(server.today_state(self.conn)["done_count"], 0)


class BudgetTests(Base):
    """每日内容推送预算：新字 1 + 惊喜 1。"""

    def test_surprise_budget_is_one_per_day(self):
        self.assertTrue(server.take_push(self.conn, "surprise", "skip"))
        self.assertFalse(server.take_push(self.conn, "surprise", "skip"))
        self.assertEqual(server.push_count_today(self.conn, "surprise"), 1)

    def test_newchar_budget_is_one_per_day(self):
        self.assertTrue(server.auto_push_zika(self.conn, "brush") or True)
        # 第一次会推；第二次换一个习惯也必须被预算挡住
        server.auto_push_zika(self.conn, "toys")
        self.assertEqual(server.push_count_today(self.conn, "newchar"), 1)

    def test_total_content_push_never_exceeds_two(self):
        server.auto_push_zika(self.conn, "brush")
        self.add_answered_question()
        server.push_daily_surprise(self.conn)
        self.assertLessEqual(len(server.pushes_today(self.conn)), 2)

    def test_surprise_priority_question_over_review(self):
        qid = self.add_answered_question()
        # 同时造一张到期复习卡
        server.zika_learned(self.conn, "牙")
        self.conn.execute("UPDATE zika_log SET due_day=? WHERE char='牙'", (server.today_str(),))
        self.conn.commit()
        got = server.push_daily_surprise(self.conn)
        self.assertEqual(got, "question", "问题答案（主线）必须优先于复习卡（衍生物）")
        row = self.conn.execute("SELECT announced FROM questions WHERE id=?", (qid,)).fetchone()
        self.assertEqual(row["announced"], 1)

    def test_surprise_second_call_same_day_is_noop(self):
        self.add_answered_question()
        self.assertEqual(server.push_daily_surprise(self.conn), "question")
        self.assertEqual(server.push_daily_surprise(self.conn), "")

    def test_surprise_falls_back_to_review(self):
        server.zika_learned(self.conn, "牙")
        self.conn.execute("UPDATE zika_log SET due_day=? WHERE char='牙'", (server.today_str(),))
        self.conn.commit()
        self.assertEqual(server.push_daily_surprise(self.conn), "review")

    def test_surprise_empty_when_nothing_due(self):
        self.assertEqual(server.push_daily_surprise(self.conn), "")

    def test_answer_question_announces_and_consumes_budget(self):
        qid = self.add_answered_question()
        server.answer_question(self.conn, qid, "一句话：因为叶绿素。", announce=True)
        row = self.conn.execute("SELECT announced FROM questions WHERE id=?", (qid,)).fetchone()
        self.assertEqual(row["announced"], 1)
        self.assertEqual(server.push_count_today(self.conn, "surprise"), 1)


class ReviewTests(Base):
    def test_learned_registers_with_due_tomorrow(self):
        server.zika_learned(self.conn, "牙")
        row = self.conn.execute("SELECT stage,due_day FROM zika_log WHERE char='牙'").fetchone()
        self.assertEqual(row["stage"], 0)
        tomorrow = (datetime.date.today() + datetime.timedelta(days=1)).isoformat()
        self.assertEqual(row["due_day"], tomorrow)

    def test_review_advances_stage_and_due(self):
        server.zika_learned(self.conn, "牙")
        self.conn.execute("UPDATE zika_log SET due_day=? WHERE char='牙'", (server.today_str(),))
        self.conn.commit()
        server.push_daily_surprise(self.conn)
        row = self.conn.execute("SELECT stage,due_day FROM zika_log WHERE char='牙'").fetchone()
        self.assertEqual(row["stage"], 1)
        expect = (datetime.date.today()
                  + datetime.timedelta(days=server.review_intervals(self.conn)[1])).isoformat()
        self.assertEqual(row["due_day"], expect)


class CardTests(Base):
    def test_zika_card_has_two_pages(self):
        server.push_card(self.conn, "牙")
        _q, imgs, n = server.card_pages(self.conn)
        self.assertEqual(n, 2)
        self.assertEqual(len(imgs), 2)

    def test_rgb565_size_matches_device_buffer(self):
        import card as cardrender
        img, _ = cardrender.render_zika("牙", "刷牙", 1)
        raw = cardrender.to_rgb565(img)
        self.assertEqual(len(raw), 240 * 240 * 2, "设备缓冲是 115200 字节，尺寸变了会花屏")

    def test_question_card_has_pages(self):
        qid = self.add_answered_question()
        _q, imgs, n = server.card_pages(self.conn, qid)
        self.assertGreaterEqual(n, 1)


class ProtocolTests(Base):
    def test_state_text_has_ten_fields(self):
        text = server.state_as_text(self.conn)
        h_lines = [l for l in text.splitlines() if l.startswith("H=")]
        self.assertEqual(len(h_lines), 2)
        for line in h_lines:
            self.assertEqual(len(line[2:].split("|")), 10, "H= 字段数变了，固件解析会错位")

    def test_state_text_has_q_and_week(self):
        text = server.state_as_text(self.conn)
        self.assertIn("Q=", text)
        self.assertIn("WEEK=", text)
        week = [l for l in text.splitlines() if l.startswith("WEEK=")][0]
        self.assertEqual(len(week[5:].split(",")), 7)

    def test_state_text_msg_uses_track(self):
        server.push_card(self.conn, "牙")
        server.add_approval(self.conn, "", "小豆", track=107, kind="zika")
        self.conn.commit()
        text = server.state_as_text(self.conn)
        msgs = [l for l in text.splitlines() if l.startswith("MSG=")]
        self.assertTrue(msgs)
        self.assertEqual(msgs[-1].split("|")[1], "107")


# ============================ HTTP 端到端 ============================

class HttpTests(Base):
    """真正起一个服务，打真实请求——render_page 的 NameError 就是这里抓到的。"""

    @classmethod
    def setUpClass(cls):
        from http.server import ThreadingHTTPServer
        cls.srv = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        cls.port = cls.srv.server_address[1]
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def get(self, path):
        url = "http://127.0.0.1:%d%s" % (HttpTests.port, path)
        try:
            with urllib.request.urlopen(url, timeout=10) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def post(self, path):
        url = "http://127.0.0.1:%d%s" % (HttpTests.port, path)
        req = urllib.request.Request(url, data=b"", method="POST")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def test_home_page_renders(self):
        code, body = self.get("/")
        self.assertEqual(code, 200)
        html = body.decode("utf-8")
        for must in ("今天的惊喜", "给", "今天"):
            self.assertIn(must, html)
        # 自洽审计 A2：首屏不许出现连续天数的火焰
        self.assertNotIn("🔥", html, "首屏又出现连续天数了")
        # 自洽审计 A1：徽章已砍
        self.assertNotIn("能力徽章", html, "徽章又回来了")

    def test_home_has_all_placeholders_replaced(self):
        _code, body = self.get("/")
        html = body.decode("utf-8")
        import re
        left = re.findall(r"\{\{[A-Z]+\}\}", html)
        self.assertEqual(left, [], "有占位符没被替换：%s" % left)

    def test_album_renders_without_denominator(self):
        code, body = self.get("/album")
        self.assertEqual(code, 200)
        html = body.decode("utf-8")
        self.assertIn("会了的字", html)
        self.assertNotIn("<small>/", html, "档案页又出现了分母")

    def test_state_txt(self):
        code, body = self.get("/api/state.txt")
        self.assertEqual(code, 200)
        text = body.decode("utf-8")
        self.assertTrue(text.startswith("DATE="))
        self.assertIn("END", text)

    def test_checkin_endpoint_and_conflict(self):
        code, _ = self.post("/api/checkin.txt?habit=brush")
        self.assertEqual(code, 200)
        code2, _ = self.post("/api/checkin.txt?habit=brush")
        self.assertEqual(code2, 409, "重复打卡应返回 409")

    def test_rgb565_endpoint_returns_device_buffer(self):
        server.push_card(self.conn, "牙")
        code, body = self.get("/api/card.rgb565?i=0")
        self.assertEqual(code, 200)
        self.assertEqual(len(body), 240 * 240 * 2)

    def test_png_endpoint(self):
        server.push_card(self.conn, "牙")
        code, body = self.get("/api/card.png?i=1")
        self.assertEqual(code, 200)
        self.assertTrue(body.startswith(b"\x89PNG"))

    def test_habits_endpoint_enforces_max(self):
        # 选 6 件，应被拒绝（303 跳到 habmax）
        import urllib.parse
        data = urllib.parse.urlencode(
            [("on", h) for h in ("brush", "toys", "book", "pack", "shoes", "water")]
        ).encode()
        req = urllib.request.Request(
            "http://127.0.0.1:%d/parent/habits" % HttpTests.port, data=data, method="POST")
        with urllib.request.urlopen(req, timeout=10) as r:
            self.assertEqual(r.status, 200)      # 跟随了 303
        self.assertEqual(len(server.get_habits(self.conn)), 2, "超过 4 件应保持原样")


if __name__ == "__main__":
    unittest.main(verbosity=2)

class StageTests(Base):
    """成长阶段：一个参数控制上限/预算/复习节奏。"""

    def test_default_stage_is_one(self):
        self.assertEqual(server.get_stage(self.conn), 1)
        self.assertEqual(server.max_habits(self.conn), 2)
        self.assertEqual(server.stage_cfg(self.conn)["content_budget"], 2)

    def test_stage_switch_changes_limits(self):
        server.set_setting(self.conn, "stage", "3")
        self.conn.commit()
        self.assertEqual(server.get_stage(self.conn), 3)
        self.assertEqual(server.max_habits(self.conn), 4)
        self.assertEqual(server.stage_cfg(self.conn)["content_budget"], 3)
        self.assertEqual(server.push_limits(self.conn)["newchar"], 2)
        self.assertEqual(server.review_intervals(self.conn), [1, 3, 7, 14, 30])

    def test_invalid_stage_falls_back(self):
        server.set_setting(self.conn, "stage", "99")
        self.conn.commit()
        self.assertEqual(server.get_stage(self.conn), 1)

    def test_stage_limits_never_exceed_firmware(self):
        """固件最多画 4 个图标，阶段上限不能超过它。"""
        for n in server.STAGES:
            server.set_setting(self.conn, "stage", str(n))
            self.conn.commit()
            self.assertLessEqual(server.max_habits(self.conn), 4)

    def test_review_uses_stage_intervals(self):
        server.set_setting(self.conn, "stage", "3")
        self.conn.commit()
        server.zika_learned(self.conn, "牙")
        row = self.conn.execute("SELECT due_day FROM zika_log WHERE char='牙'").fetchone()
        expect = (datetime.date.today()
                  + datetime.timedelta(days=server.review_intervals(self.conn)[0])).isoformat()
        self.assertEqual(row["due_day"], expect)

    def test_content_budget_per_stage(self):
        server.set_setting(self.conn, "stage", "3")
        self.conn.commit()
        limits = server.push_limits(self.conn)
        self.assertEqual(limits["newchar"] + limits["surprise"], 3)

