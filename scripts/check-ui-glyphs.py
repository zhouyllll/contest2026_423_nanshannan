#!/usr/bin/env python3
"""检查 kickpi_ui 界面字符串里有没有字库 lv_font_k7_cjk_20 缺的字形。

缺的字在屏上显示成方框（例：等待提示里的"→"、唤醒提示里的"「」"）。
改界面文字后跑一下：python3 scripts/check-ui-glyphs.py
缺字时退出码为 1；补字形改 scripts/gen-cjk-font.sh 后重新生成字库。
只查源码里的字符串字面量（跳过注释）；模型回答里的生僻字查不到。
"""
import pathlib
import re
import sys

ui = pathlib.Path(__file__).resolve().parent.parent / "app" / "kickpi_ui"
font = (ui / "lv_font_k7_cjk_20.c").read_text(encoding="utf-8")
have = {int(x, 16) for x in re.findall(r"/\* U\+([0-9A-F]+)", font)}

# 会上屏的字符串所在的文件：界面本身，以及错误提示会作为回答气泡显示的
# k7_vision.c。k7_wake.c 里是唤醒词匹配表、k7_tts.c 只打日志，不查。
SHOWN = ["kickpi_ui_main.c", "k7_vision.c"]

missing = {}
for src in (ui / name for name in SHOWN):
    code = re.sub(r"/\*.*?\*/", "", src.read_text(encoding="utf-8"), flags=re.S)
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', code):
        for ch in lit:
            if ord(ch) >= 0x80 and ord(ch) not in have:
                missing.setdefault(ch, set()).add(f"{src.name}: {lit[:40]}")

for ch, where in sorted(missing.items()):
    print(f"缺 U+{ord(ch):04X} {ch}  <- " + " | ".join(sorted(where)))
print(f"字库 {len(have)} 个字形，界面缺字 {len(missing)} 个")
sys.exit(1 if missing else 0)
