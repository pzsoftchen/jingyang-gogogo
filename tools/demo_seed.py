#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""写入一套演示数据，方便先看页面效果（正式使用前跑 ./reset.sh 清空）。

演示内容：最近 5 天
  今天 / 昨天 / 前天：2 件事全做完  → 连续 3 天
  3 天前：只做了 1 件           → 用来展示"断了"的样子
  4 天前：全做完
外加 2 条家长认可 + 1 条里程碑播报（展示"徽章/成就"长什么样）。
"""
import datetime
import os
import sqlite3

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB = os.path.join(ROOT, "coach.db")
HABITS = ["brush", "toys"]
PLAN = {
    0: HABITS,        # 今天
    1: HABITS,        # 昨天
    2: HABITS,        # 前天
    3: HABITS[:1],    # 断掉的一天（只做了一件）
    4: HABITS,
}


def main():
    if not os.path.exists(DB):
        raise SystemExit("还没有 coach.db，先在项目根目录运行一次 python3 server.py 再跑我。")
    conn = sqlite3.connect(DB)
    today = datetime.date.today()
    for back, done in PLAN.items():
        day = (today - datetime.timedelta(days=back)).isoformat()
        for i, h in enumerate(done):
            hh = 20
            mm = 30 + i * 10
            conn.execute(
                "INSERT OR IGNORE INTO checkins(habit_id,day,ts) VALUES(?,?,?)",
                (h, day, "%s %02d:%02d:00" % (day, hh, mm)),
            )
    conn.execute(
        "INSERT INTO approvals(habit_id,message,from_who,created,slot,note,track,kind,played)"
        " VALUES('brush','你自己去刷牙了，没有等妈妈叫。','妈妈',"
        "?,1,'',31,'praise',1)",
        ((today - datetime.timedelta(days=1)).isoformat() + " 20:41:00",),
    )
    conn.execute(
        "INSERT INTO approvals(habit_id,message,from_who,created,slot,note,track,kind,played)"
        " VALUES('toys','你把玩具都放回箱子里了，我看到了。','爸爸',"
        "?,3,'今天老师说他主动帮忙了',33,'praise',0)",
        (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),),
    )
    # 演示一条"里程碑自动播报"（真实场景由服务器在撞到 3/7/14/30 天时自动生成）
    if not conn.execute("SELECT 1 FROM approvals WHERE kind='badge'").fetchone():
        conn.execute(
            "INSERT INTO approvals(habit_id,message,from_who,created,slot,note,track,kind,played)"
            " VALUES('','连续 3 天达成！','豆豆',?,0,'',21,'badge',0)",
            (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),),
        )
    conn.commit()
    conn.close()
    print("已写入 5 天演示数据，刷新家长页看看效果。")
    print("正式使用前：./tools/reset.sh   （清空数据）")


if __name__ == "__main__":
    main()
