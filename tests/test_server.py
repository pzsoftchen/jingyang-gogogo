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
import re
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

    def _insert_answered(self, question, answer, minute):
        return self.conn.execute(
            "INSERT INTO questions(question,answer,status,announced,created)"
            " VALUES(?,?,?,?,?)",
            (question, answer, "answered", 0, "2026-11-06 08:%02d:00" % minute),
        ).lastrowid

    def test_surprise_card_matches_the_announced_question(self):
        """播报哪一条问题的答案，屏幕就必须显示哪一条。"""
        first = self._insert_answered("问题一", "一句话：答案一。", 0)
        self._insert_answered("问题二", "一句话：答案二。", 30)
        self.conn.commit()

        self.assertEqual(server.push_daily_surprise(self.conn), "question")
        q, _imgs, _n = server.card_pages(self.conn)
        self.assertEqual(q["id"], first, "卡片必须绑定到刚刚播报的那条问题")

    def test_answer_announcement_binds_card_to_that_question(self):
        """后答旧问题时，屏幕不能跳到更新的那条答案。"""
        older = self._insert_answered("问题一", "一句话：旧答案。", 0)
        self._insert_answered("问题二", "一句话：新答案。", 30)
        self.conn.commit()

        server.answer_question(self.conn, older, "一句话：改过的旧答案。", announce=True)
        q, _imgs, _n = server.card_pages(self.conn)
        self.assertEqual(q["id"], older, "卡片必须绑定到刚刚播报的那条问题")

    def test_missing_card_ref_falls_back_to_latest_answer(self):
        """旧库没有 card_ref：不能因为没有绑定就显示不出卡。"""
        self._insert_answered("问题一", "一句话：旧答案。", 0)
        newer = self._insert_answered("问题二", "一句话：新答案。", 30)
        self.conn.execute("DELETE FROM settings WHERE key='card_ref'")
        self.conn.commit()

        q, _imgs, _n = server.card_pages(self.conn)
        self.assertEqual(q["id"], newer)


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

    def test_state_text_sends_one_message_per_sync(self):
        """设备端 say() 会打断上一条，批量下发会让前面的消息永久丢失。"""
        server.add_approval(self.conn, "第一句", "爸爸妈妈", track=31)
        server.add_approval(self.conn, "第二句", "爸爸妈妈", track=32)
        self.conn.commit()

        msgs = [l for l in server.state_as_text(self.conn).splitlines()
                if l.startswith("MSG=")]
        self.assertEqual(len(msgs), 1, "一次同步只能送一条待播消息")
        self.assertEqual(msgs[0].split("|")[1], "31", "先送最早的一条")

        mid = int(msgs[0][4:].split("|")[0])
        server.ack_approval(self.conn, mid)
        msgs2 = [l for l in server.state_as_text(self.conn).splitlines()
                 if l.startswith("MSG=")]
        self.assertEqual(len(msgs2), 1)
        self.assertEqual(msgs2[0].split("|")[1], "32", "ack 之后才送下一条")


class FirmwareContractTests(unittest.TestCase):
    """固件没法在主机上跑，至少守住“所有设备 API 请求都带身份”这个契约。"""

    def _source(self):
        path = os.path.join(os.path.dirname(HERE), "firmware", "habit_coach",
                            "habit_coach.ino")
        with open(path, encoding="utf-8") as f:
            return f.read()

    def test_all_device_api_calls_use_identity_helper(self):
        src = self._source()
        self.assertIn("String apiUrl(", src, "缺少统一的 API URL 构造器")
        self.assertIn("url += deviceQuery();", src, "apiUrl 没有真正拼上身份参数")
        calls = re.findall(r"http\.begin\(\s*(.*?)\s*\);", src, re.S)
        self.assertTrue(calls)
        for arg in calls:
            self.assertTrue(
                arg.startswith("apiUrl("),
                "设备 API 请求必须走 apiUrl()，否则会漏掉 d/t 参数：%s" % arg,
            )

    def test_device_query_percent_encodes_identity(self):
        """base64 token 常含 + / =，不编码会被服务器解析错（+ 变空格）。"""
        src = self._source()
        self.assertIn("String urlEncodeComponent(", src)
        self.assertIn("urlEncodeComponent(DEVICE_ID)", src)
        self.assertIn("urlEncodeComponent(tk)", src)

    def test_185c_shared_i2c_uses_sda_11_scl_10(self):
        """1.85C 的 TCA9554/CST816 共用 SDA=11 / SCL=10；写反会 cfg=FF。"""
        src = self._source()
        self.assertIn("#define SYS_SDA   11", src)
        self.assertIn("#define SYS_SCL   10", src)

    def test_canvas_does_not_reinitialize_qspi(self):
        """Arduino_Canvas::begin() 默认会再调 gfx->begin()，QSPI 会 abort。"""
        src = self._source()
        self.assertIn("canvas->begin(GFX_SKIP_OUTPUT_BEGIN)", src)

    def test_185c_resets_lcd_and_touch_via_p0_p1(self):
        """1.85C 的 LCD/触摸复位在 TCA9554 P0/P1；P0 不能配成输入。"""
        src = self._source()
        self.assertIn("tcaWrite(0x03, 0b11110000)", src)
        self.assertIn("tcaWrite(0x01, 0b00001111)", src)


class DeviceStatusTests(Base):
    def test_rssi_parser_rejects_invalid_values(self):
        self.assertEqual(server.rssi_of({"rssi": ["-61"]}), -61)
        self.assertIsNone(server.rssi_of({"rssi": ["abc"]}))
        self.assertIsNone(server.rssi_of({"rssi": ["5"]}))
        self.assertIsNone(server.rssi_of({}))

    def test_touch_device_updates_ip_and_rssi(self):
        server.touch_device(self.conn, "poc", "192.168.8.253", -61)
        row = self.conn.execute(
            "SELECT ip,rssi FROM devices WHERE id='poc'").fetchone()
        self.assertEqual(row["ip"], "192.168.8.253")
        self.assertEqual(row["rssi"], -61)


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
        # 协议第 1 行永远是版本号，第 2 行才是 DATE（"只追加不改动"约定）
        self.assertTrue(text.startswith("V=2\nDATE="), text[:40])
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


    def test_card_size_360_for_185_board(self):
        """1.85 寸屏要 360×360，字节数必须是 259200。"""
        server.push_card(self.conn, "牙")
        code, body = self.get("/api/card.rgb565?i=0&s=360")
        self.assertEqual(code, 200)
        self.assertEqual(len(body), 360 * 360 * 2)

    def test_card_size_falls_back_to_240(self):
        server.push_card(self.conn, "牙")
        code, body = self.get("/api/card.rgb565?i=0&s=999")
        self.assertEqual(code, 200)
        self.assertEqual(len(body), 240 * 240 * 2)

    def test_png_endpoint(self):
        server.push_card(self.conn, "牙")
        code, body = self.get("/api/card.png?i=1")
        self.assertEqual(code, 200)
        self.assertTrue(body.startswith(b"\x89PNG"))

    def test_push_card_switches_from_zika_to_question(self):
        """当前停在识字卡时，点「播放这张问题卡」必须切回问题卡。"""
        server.push_card(self.conn, "牙")
        qid = self.add_answered_question()

        code, _ = self.post("/parent/push_card")
        self.assertEqual(code, 200)
        q, _imgs, _n = server.card_pages(self.conn)
        self.assertEqual(q["id"], qid, "设备上的卡必须是被播报的那条问题")

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

    def test_quick_page_and_post(self):
        code, body = self.get("/quick")
        self.assertEqual(code, 200)
        self.assertIn("记一个问题", body.decode("utf-8"))
        import urllib.parse
        data = urllib.parse.urlencode({"question": "为什么天是蓝的"}).encode()
        req = urllib.request.Request(
            "http://127.0.0.1:%d/quick" % HttpTests.port, data=data, method="POST")
        with urllib.request.urlopen(req, timeout=10) as r:
            self.assertEqual(r.status, 200)
        row = self.conn.execute(
            "SELECT question FROM questions ORDER BY id DESC LIMIT 1").fetchone()
        self.assertEqual(row["question"], "为什么天是蓝的")

    def test_device_can_record_pending_question(self):
        code, _ = self.post("/api/question_quick")
        self.assertEqual(code, 200)
        pend = server.pending_questions_to_fill(self.conn)
        self.assertEqual(len(pend), 1)
        # 页面上应出现"待补内容"
        _c, body = self.get("/")
        self.assertIn("待补内容", body.decode("utf-8"))

    def test_fill_pending_question(self):
        server.add_question_pending(self.conn, "动物")
        pend = server.pending_questions_to_fill(self.conn)
        qid = pend[0]["id"]
        ok = server.fill_pending_question(self.conn, qid, "为什么猫会呼噜")
        self.assertTrue(ok)
        self.assertEqual(server.pending_questions_to_fill(self.conn), [])
        row = self.conn.execute("SELECT status,question FROM questions WHERE id=?",
                                (qid,)).fetchone()
        self.assertEqual(row["status"], "ask")

    def test_child_note_banner(self):
        code, _ = self.post("/api/child_note")
        self.assertEqual(code, 200)
        _c, body = self.get("/")
        self.assertIn("有件事想跟你说", body.decode("utf-8"))
        self.post("/parent/notes_seen")
        _c2, body2 = self.get("/")
        self.assertNotIn("有件事想跟你说", body2.decode("utf-8"))

    def test_protocol_has_version_line(self):
        """V= 必须是第一行：固件据此判断服务器能力。"""
        code, body = self.get("/api/state.txt")
        self.assertEqual(code, 200)
        first = body.decode("utf-8").splitlines()[0]
        self.assertEqual(first, "V=2")

    def test_protocol_has_content_pack_line(self):
        code, body = self.get("/api/state.txt")
        text = body.decode("utf-8")
        self.assertIn("PACK=voice.", text)
        self.assertLess(text.index("PACK="), text.index("END"))

    def test_unknown_protocol_lines_are_ignored(self):
        """老固件遇到新字段要能忽略——这条保证'只追加不改动'的约定可验证。"""
        text = server.state_as_text(self.conn)
        known = ("V=", "DATE=", "CHILD=", "COACH=", "STREAK=", "DONE=", "Q=",
                 "WEEK=", "H=", "MSG=", "PACK=", "END")
        for line in text.splitlines():
            if not line:
                continue
            self.assertTrue(line.startswith(known), "出现了未登记的协议行: %r" % line)

    def test_device_registered_on_touch(self):
        self.get("/api/state.txt?d=s3-test-1")
        row = self.conn.execute("SELECT id FROM devices WHERE id='s3-test-1'").fetchone()
        self.assertIsNotNone(row)

    def test_parent_page_shows_online_device_status(self):
        """家长页首屏要能看到设备在线、IP 和 Wi-Fi 信号。"""
        code, _ = self.get("/api/state.txt?d=poc&rssi=-61")
        self.assertEqual(code, 200)
        code, body = self.get("/")
        self.assertEqual(code, 200)
        html = body.decode("utf-8")
        self.assertIn("设备在线", html)
        self.assertIn("127.0.0.1", html)
        self.assertIn("-61 dBm", html)

    def test_parent_page_shows_offline_device_status(self):
        self.conn.execute(
            "UPDATE devices SET last_seen='2026-01-01 00:00:00' WHERE id='poc'")
        self.conn.commit()
        code, body = self.get("/")
        self.assertEqual(code, 200)
        html = body.decode("utf-8")
        self.assertIn("设备离线", html)
        self.assertIn("最后同步 01-01 00:00", html)

    def test_checkin_records_device(self):
        self.post("/api/checkin.txt?habit=brush&d=s3-abc")
        row = self.conn.execute(
            "SELECT device_id FROM checkins ORDER BY id DESC LIMIT 1").fetchone()
        self.assertEqual(row["device_id"], "s3-abc")

    def test_token_enforced_when_configured(self):
        os.environ["DEVICE_TOKEN"] = "s3cr3t"
        try:
            code, _ = self.get("/api/state.txt")
            self.assertEqual(code, 401, "配了令牌就必须校验")
            code2, _ = self.get("/api/state.txt?t=s3cr3t")
            self.assertEqual(code2, 200)
        finally:
            os.environ.pop("DEVICE_TOKEN", None)

    def test_parent_pages_never_need_token(self):
        os.environ["DEVICE_TOKEN"] = "s3cr3t"
        try:
            code, _ = self.get("/")
            self.assertEqual(code, 200, "家长页不能被设备令牌挡住")
        finally:
            os.environ.pop("DEVICE_TOKEN", None)

    def test_token_with_reserved_characters_must_be_percent_encoded(self):
        """设备端必须对 token 做 URL 编码；未编码的 + 会被 parse_qs 解析成空格。"""
        os.environ["DEVICE_TOKEN"] = "ab+cd/ef="
        try:
            code, _ = self.get("/api/state.txt?t=ab%2Bcd%2Fef%3D")
            self.assertEqual(code, 200, "百分号编码后的 token 应通过")
            code2, _ = self.get("/api/state.txt?t=ab+cd/ef=")
            self.assertEqual(code2, 401, "未编码的 token 不应被接受")
        finally:
            os.environ.pop("DEVICE_TOKEN", None)


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

