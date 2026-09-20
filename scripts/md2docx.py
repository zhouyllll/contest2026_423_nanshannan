#!/usr/bin/env python3
"""把技术报告的 Markdown 转成 .docx（提交材料要 Word 版）。

用法：scripts/md2docx.py <输入.md> [输出.docx]

本机没有 pandoc，且 apt 装不动；用 python-docx 直接渲染。支持报告里实际用到的
语法：# ~ #### 标题、段落、| 表格 |、![图](png)、- 无序列表、1. 有序列表、
> 引用，以及行内的 **粗体** 与 `等宽`。不做完整 Markdown 实现 —— 只覆盖这份
文档用到的部分，遇到没覆盖的语法会原样输出而不是静默丢掉。

★ 中文字体要单独设 eastAsia：python-docx 的 font.name 只改 ascii/hAnsi，
  中文仍会落到主题字体上，Word 里表现为中英文混排字体不一致。
"""

import re
import sys
from pathlib import Path

from docx import Document
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.shared import Inches, Pt

LATIN = 'Calibri'
CJK = '等线'
MONO = 'Consolas'
BODY_PT = 10.5


def set_font(run, name=LATIN, mono=False):
    run.font.name = MONO if mono else name
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn('w:rFonts'))
    if rfonts is None:
        rfonts = rpr.makeelement(qn('w:rFonts'), {})
        rpr.append(rfonts)
    rfonts.set(qn('w:eastAsia'), MONO if mono else CJK)


def add_runs(par, text, size=BODY_PT):
    """处理行内 **粗体** 与 `等宽`。"""
    for piece in re.split(r'(\*\*[^*]+\*\*|`[^`]+`)', text):
        if not piece:
            continue
        if piece.startswith('**') and piece.endswith('**'):
            r = par.add_run(piece[2:-2]); r.bold = True; set_font(r)
        elif piece.startswith('`') and piece.endswith('`'):
            r = par.add_run(piece[1:-1]); set_font(r, mono=True)
            r.font.size = Pt(size - 0.5)
            continue
        else:
            r = par.add_run(piece); set_font(r)
        r.font.size = Pt(size)


def split_row(line):
    return [c.strip() for c in line.strip().strip('|').split('|')]


def main():
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix('.docx')
    lines = src.read_text(encoding='utf-8').split('\n')

    doc = Document()
    normal = doc.styles['Normal']
    normal.font.name = LATIN
    normal.font.size = Pt(BODY_PT)
    normal.element.rPr.rFonts.set(qn('w:eastAsia'), CJK)

    i = 0
    while i < len(lines):
        line = lines[i].rstrip()

        if not line.strip():
            i += 1
            continue

        m = re.match(r'^(#{1,4})\s+(.*)$', line)
        if m:
            doc.add_heading(m.group(2).replace('**', ''), level=len(m.group(1)))
            i += 1
            continue

        m = re.match(r'^!\[([^\]]*)\]\(([^)]+)\)\s*$', line)
        if m:
            img = src.parent / m.group(2)
            if img.exists():
                doc.add_picture(str(img), width=Inches(6.0))
                doc.paragraphs[-1].alignment = WD_ALIGN_PARAGRAPH.CENTER
                cap = doc.add_paragraph()
                cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
                add_runs(cap, m.group(1), size=9)
            i += 1
            continue

        if line.lstrip().startswith('|'):
            block = []
            while i < len(lines) and lines[i].lstrip().startswith('|'):
                block.append(lines[i])
                i += 1
            rows = [split_row(b) for b in block
                    if not re.match(r'^\|[\s:|-]+\|$', b.strip())]
            if not rows:
                continue
            ncol = max(len(r) for r in rows)
            table = doc.add_table(rows=0, cols=ncol)
            table.style = 'Table Grid'
            for ri, row in enumerate(rows):
                cells = table.add_row().cells
                for ci in range(ncol):
                    text = row[ci] if ci < len(row) else ''
                    par = cells[ci].paragraphs[0]
                    add_runs(par, text, size=9)
                    if ri == 0:
                        for r in par.runs:
                            r.bold = True
            doc.add_paragraph()
            continue

        m = re.match(r'^(\s*)[-*]\s+(.*)$', line)
        if m:
            par = doc.add_paragraph(style='List Bullet')
            add_runs(par, m.group(2))
            i += 1
            continue

        m = re.match(r'^(\s*)\d+\.\s+(.*)$', line)
        if m:
            par = doc.add_paragraph(style='List Number')
            add_runs(par, m.group(2))
            i += 1
            continue

        if line.lstrip().startswith('>'):
            par = doc.add_paragraph()
            par.paragraph_format.left_indent = Inches(0.3)
            add_runs(par, line.lstrip()[1:].strip())
            for r in par.runs:
                r.italic = True
            i += 1
            continue

        par = doc.add_paragraph()
        add_runs(par, line.strip())
        i += 1

    doc.save(str(dst))
    print(f'{dst}: {dst.stat().st_size} 字节，'
          f'{len(doc.paragraphs)} 段 / {len(doc.tables)} 表')


if __name__ == '__main__':
    main()
