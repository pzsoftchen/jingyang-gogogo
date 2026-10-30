#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""问题卡渲染器：把「家长审核过的问答」画成 240×240 的图。

为什么要服务器渲染？
  设备端只有 Arduino_GFX 的 ASCII 字库，显示中文需要塞点阵字库 + 自己排版。
  改成服务器出图，设备只负责下载 115200 字节的 RGB565 直接贴到屏上：
  中文排版随便调，设备端零字库、零排版逻辑。

用法（命令行调试）：
  python3 card.py "为什么树叶会变黄" "一句话：...\\n反问：...\\n一起做：..." out.png
"""

import re
import sys

from PIL import Image, ImageDraw, ImageFont

W = H = 240
SAFE = 26                     # 圆屏内容安全边距
FONT_PATH = "/System/Library/Fonts/PingFang.ttc"

BG = (13, 17, 23)
FG = (232, 237, 243)
DIM = (139, 151, 166)
GOLD = (255, 209, 102)
PINK = (255, 107, 157)
GREEN = (61, 220, 151)

_font_cache = {}


def font(size):
    if size not in _font_cache:
        try:
            _font_cache[size] = ImageFont.truetype(FONT_PATH, size)
        except OSError:
            _font_cache[size] = ImageFont.load_default()
    return _font_cache[size]


def split_blocks(answer):
    """把答案拆成 一句话 / 反问 / 一起做 三段；拆不出来就整段当一段。"""
    lines = [l.strip() for l in (answer or "").splitlines() if l.strip()]
    blocks = []
    for l in lines:
        blocks.append(l)
    if not blocks:
        return [], [], []

    one, ask, do = [], [], []
    for l in blocks:
        if re.match(r"^(反问|问问他)", l):
            ask.append(re.sub(r"^(反问[：:]?|问问他[：:]?)", "", l).strip())
        elif re.match(r"^(一起|动手|试试|可以做|一起做)", l):
            do.append(re.sub(r"^(一起做[：:]?|一起[：:]?|动手[：:]?|试试[：:]?)", "", l).strip())
        elif re.match(r"^(一句话|答案|一句话答案)", l):
            one.append(re.sub(r"^(一句话答案|一句话|答案)[：:]?", "", l).strip())
        else:
            # 没有标签的行，按出现顺序补
            if not one:
                one.append(l)
            elif not ask:
                ask.append(l)
            else:
                do.append(l)
    return one, ask, do


def wrap(text, f, max_w):
    """按像素宽度折行（中文逐字折）。"""
    out, cur = [], ""
    for ch in text:
        if ch == "\n":
            out.append(cur)
            cur = ""
            continue
        if f.getlength(cur + ch) > max_w and cur:
            out.append(cur)
            cur = ch
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out


def _text_block(d, x, y, lines, f, fill, line_h, bottom):
    """画多行文字，超过安全底线就截断。返回 (结束 y, 是否被截断)。"""
    n = 0
    for ln in lines:
        if y + line_h > bottom:
            return y, True
        d.text((x, y), ln, font=f, fill=fill)
        y += line_h
        n += 1
    return y, False


def sections(answer):
    """把答案拆成若干页：答案 / 你可以问问看 / 一起做。一页一个想法。"""
    one, ask, do = split_blocks(answer)
    secs = []
    if one:
        secs.append(("答案", one))
    if ask:
        secs.append(("你可以问问看", ask))
    if do:
        secs.append(("一起做", do))
    if not secs:
        secs.append(("答案", [answer or "（还没有答案）"]))
    return secs[:3]


def render_card(question, answer, page=1, total=None):
    """一页一个想法：第 1 页答案，第 2 页反问，第 3 页一起做。"""
    secs = sections(answer)
    total = total or len(secs)
    page = max(1, min(page, total))
    label, lines = secs[page - 1]

    f_label = font(12)
    f_q = font(18)
    f_body = font(15)
    f_sec = font(13)

    max_w = W - SAFE * 2
    q_lines = wrap(question, f_q, max_w)[:2]

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.ellipse([2, 2, W - 3, H - 3], fill=(20, 26, 34))

    y = 26
    d.text((SAFE, y), "我的问题", font=f_label, fill=DIM)
    d.text((W - SAFE - 34, y), "%d/%d" % (page, total), font=f_label, fill=DIM)
    y += 20

    for ln in q_lines:
        d.text((SAFE, y), ln, font=f_q, fill=PINK)
        y += 24

    y += 6
    d.line([SAFE, y, W - SAFE, y], fill=(42, 49, 58), width=1)
    y += 14

    d.text((SAFE, y), label, font=f_sec, fill=GOLD)
    y += 22

    bottom = H - 46
    wrapped = wrap(" ".join(lines), f_body, max_w)
    cut = False
    for ln in wrapped:
        if y + 24 > bottom:
            cut = True
            break
        d.text((SAFE, y), ln, font=f_body, fill=FG)
        y += 24
    if cut:
        d.text((W - SAFE - 12, bottom - 16), "…", font=f_body, fill=DIM)

    for i in range(total):
        cx = int(W // 2 + (i - (total - 1) / 2) * 18)
        color = GOLD if i == page - 1 else (42, 49, 58)
        d.ellipse([cx - 4, H - 30, cx + 4, H - 22], fill=color)

    return img, total


def to_rgb565(img):
    """转成设备端直接 memcpy 的字节流（小端 RGB565）。"""
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    v = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
    return v.astype("<u2").tobytes()


if __name__ == "__main__":
    q = sys.argv[1] if len(sys.argv) > 1 else "为什么树叶会变黄"
    a = sys.argv[2] if len(sys.argv) > 2 else (
        "一句话答案：叶子里的绿色会被太阳慢慢吃掉，黄色就露出来了。\n"
        "反问：你摸摸这两片叶子，哪一片更薄？\n"
        "一起做：捡三片不同颜色的叶子，按颜色排排队。"
    )
    out = sys.argv[3] if len(sys.argv) > 3 else "card_sample.png"
    im1, total = render_card(q, a, 1, 2)
    im2, _ = render_card(q, a, 2, 2)
    canvas = Image.new("RGB", (W * 2 + 16, H), (0, 0, 0))
    canvas.paste(im1, (0, 0))
    canvas.paste(im2, (W + 16, 0))
    canvas.save(out)
    print("已生成", out, "页数", total)

# ============================ 识字卡 ============================

def render_zika(char, word, page=1, total=2):
    """识字卡：第 1 页看字形，第 2 页看在词语里的样子。

    5 岁半不要求会写、不要求会拼，只建立"字—音—义"和"字在词里"的联结。
    """
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.ellipse([2, 2, W - 3, H - 3], fill=(20, 26, 34))

    f_label = font(12)
    d.text((SAFE, 26), "认一个字", font=f_label, fill=DIM)
    d.text((W - SAFE - 34, 26), "%d/%d" % (page, total), font=f_label, fill=DIM)

    if page == 1:
        # 大字：撑满画面的字
        f_big = font(128)
        bbox = d.textbbox((0, 0), char, font=f_big)
        cw, ch = bbox[2] - bbox[0], bbox[3] - bbox[1]
        d.text(((W - cw) / 2 - bbox[0], 62 + (150 - ch) / 2 - bbox[1]),
               char, font=f_big, fill=GOLD)
        # 词例
        f_w = font(17)
        wb = d.textbbox((0, 0), word, font=f_w)
        d.text(((W - (wb[2] - wb[0])) / 2 - wb[0], 186),
               word, font=f_w, fill=(160, 172, 186))
    else:
        d.text((SAFE, 58), "这个词里有它", font=font(13), fill=DIM)
        f_word = font(56)
        # 逐字画，目标字高亮
        widths = [d.textlength(c, font=f_word) for c in word]
        total_w = sum(widths)
        x = (W - total_w) / 2
        for c, cwid in zip(word, widths):
            d.text((x, 88), c, font=f_word, fill=GOLD if c == char else FG)
            x += cwid
        d.line([SAFE + 24, 168, W - SAFE - 24, 168], fill=(42, 49, 58), width=1)
        f_hint = font(14)
        hint = "找一找：家里哪里有它？"
        hb = d.textbbox((0, 0), hint, font=f_hint)
        d.text(((W - (hb[2] - hb[0])) / 2 - hb[0], 182), hint, font=f_hint, fill=GREEN)

    for i in range(total):
        cx = int(W // 2 + (i - (total - 1) / 2) * 18)
        color = GOLD if i == page - 1 else (42, 49, 58)
        d.ellipse([cx - 4, H - 30, cx + 4, H - 22], fill=color)
    return img, total
