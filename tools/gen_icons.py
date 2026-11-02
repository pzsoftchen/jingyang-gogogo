#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把习惯 emoji 渲染成 40×40 的 RGB565 图标表，生成固件头文件。

为什么要预渲染？
  设备端没有 emoji 字体（Arduino_GFX 只带 ASCII 位图），也放不下彩色字库。
  所以用 macOS 的 Apple Color Emoji 在电脑上渲染好，直接给设备贴图。

用法：python3 tools/gen_icons.py
产物：firmware/habit_coach/habit_icons.h
"""

import os

from PIL import Image, ImageDraw, ImageFont

SIZE = 40                     # 图标边长（px）
BG = (20, 26, 34)             # 与设备背景接近，用于合成透明区域
EMOJI_FONT = "/System/Library/Fonts/Apple Color Emoji.ttc"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "firmware", "habit_coach", "habit_icons.h")

# id 必须和 server.py 里 HABIT_PRESETS 的 id 一致
ICONS = [
    ("brush",    "🪥"),
    ("toys",     "🧸"),
    ("bathflow", "🛁"),
    ("laundry",  "🧺"),
    ("wake",     "🌞"),
    ("book",     "📖"),
    ("water",    "💧"),
    ("outdoor",  "🏃"),
    ("help",     "🍽"),
    ("shoes",    "👟"),
    ("dress",    "👕"),
    ("pack",     "🎒"),
    ("morning",  "🚪"),
]


def render_emoji(ch):
    """Apple Color Emoji 只有固定字号的 strike，先按 160 渲染再缩放。"""
    f = ImageFont.truetype(EMOJI_FONT, 160)
    big = Image.new("RGBA", (220, 220), (0, 0, 0, 0))
    ImageDraw.Draw(big).text((16, 16), ch, font=f, embedded_color=True)
    bbox = big.getbbox()
    if bbox:
        big = big.crop(bbox)
    # 等比缩放到 SIZE×SIZE，居中
    w, h = big.size
    scale = min(SIZE / w, SIZE / h)
    small = big.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), BG + (255,))
    canvas.alpha_composite(small, ((SIZE - small.width) // 2, (SIZE - small.height) // 2))
    return canvas.convert("RGB")


def to_rgb565(img):
    px = []
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = img.getpixel((x, y))
            px.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return px


def main():
    data = []
    for hid, ch in ICONS:
        data.append(to_rgb565(render_emoji(ch)))

    lines = [
        "// 由 tools/gen_icons.py 自动生成，不要手改。",
        "// 习惯 emoji 图标：%d 个 × %d×%d RGB565，小端字节序。" % (len(ICONS), SIZE, SIZE),
        "#pragma once",
        "",
        "#define HABIT_ICON_W %d" % SIZE,
        "#define HABIT_ICON_H %d" % SIZE,
        "#define HABIT_ICON_COUNT %d" % len(ICONS),
        "",
        "static const char* HABIT_ICON_IDS[HABIT_ICON_COUNT] = {",
        "  " + ", ".join('"%s"' % hid for hid, _ in ICONS),
        "};",
        "",
        "static const uint8_t HABIT_ICON_PIX[HABIT_ICON_COUNT][HABIT_ICON_W * HABIT_ICON_H * 2] PROGMEM = {",
    ]
    for idx, px in enumerate(data):
        # 每行 16 个像素，读起来不至于爆炸
        rows = []
        for i in range(0, len(px), 16):
            chunk = px[i:i + 16]
            rows.append("    " + ",".join("0x%04x" % ((v >> 8) | ((v & 0xff) << 8)) if False else ("0x%02x,0x%02x" % (v & 0xff, v >> 8)) for v in chunk))
        lines.append("  {  // %s  %s" % (ICONS[idx][0], ICONS[idx][1]))
        lines.extend(rows)
        lines.append("  },")
    lines.append("};")
    lines.append("")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("已生成 %s（%d 个图标，%.1f KB）" % (OUT, len(ICONS), os.path.getsize(OUT) / 1024.0))

    # 顺便拼一张预览图，肉眼确认没问题
    cols = 7
    rows_n = (len(ICONS) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (SIZE + 10), rows_n * (SIZE + 10)), (8, 12, 18))
    for i, (hid, _) in enumerate(ICONS):
        r, c = divmod(i, cols)
        img = render_emoji(ICONS[i][1])
        sheet.paste(img, (c * (SIZE + 10) + 5, r * (SIZE + 10) + 5))
    prev = os.path.join(os.path.dirname(OUT), "..", "..", "docs", "img", "habit_icons.png")
    prev = os.path.abspath(prev)
    os.makedirs(os.path.dirname(prev), exist_ok=True)
    sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST).save(prev)
    print("预览图: %s" % prev)


if __name__ == "__main__":
    main()
