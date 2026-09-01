#!/usr/bin/env python3
"""找出"代码里有数值兜底、而 .config 里没有"的配置项。

为什么需要它
------------
本工程的 make olddefconfig 跑不通（upstream 的 arch/tricore/Kconfig 有语法
错误，级联导致 apps 段 endmenu 失配，且失败时会删掉 include/nuttx/config.h，
见 DEBUG-CASES 案例 6），配置只能手改 .config。

手改的代价是子选项不会自动补全。最危险的一类是**代码里带数值兜底**的项：
缺失时不报错，直接用一个可能不适合本平台的值。

本端口踩过四次，后两次是运行时静默损坏：
  DEV_GPIO_NSIGNALS 等          编译报错（好抓）
  MMCSD_BLOCK_WDATADELAY 等     编译报错（好抓）
  TESTING_OSTEST_STACKSIZE      填了 Kconfig 默认 8192，arm64 上不够 → 挂死
  TESTING_OSTEST_FPUSTACKSIZE   缺失 → 代码兜底值 2048 → 栈溢出写坏相邻
                                内存 → 互斥锁结构损坏的断言

★ 只报数值兜底，不报布尔特性开关。
  形如 #ifndef CONFIG_I2C_POLLED 的是特性判断，"未定义"就是正确状态，
  报出来只会淹没真正的问题。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
WS = os.path.abspath(os.path.join(ROOT, '..', '..'))
DOTC = os.path.join(WS, 'nuttx', '.config')

# #ifndef CONFIG_X  换行  #  define CONFIG_X <数值或标识符>
PAT = re.compile(
    r'#ifndef\s+(CONFIG_[A-Z0-9_]+)\s*\n\s*#\s*define\s+\1\s+([^\n/]+)')

NUM = re.compile(r'^\s*\(?\s*[0-9]')


def load_config(path):
    on = set()
    val = {}
    with open(path, errors='replace') as f:
        for line in f:
            m = re.match(r'(CONFIG_[A-Z0-9_]+)=(.*)', line.strip())
            if m:
                on.add(m.group(1))
                val[m.group(1)] = m.group(2)
    return on, val


def compiled_sources(ws):
    """本次构建实际编译了哪些源文件。

    判据取自构建产物：每个 .o 对应一个源文件。用"配置项前缀是否启用"
    之类的启发式过滤太松 —— CONFIG_ARCH_BOARD 一旦启用，别的板子的
    CONFIG_ARCH_BOARD_* 全会被误报，真正的问题被淹没。

    NuttX 的目标文件名有两种形态：
        foo.o
        foo.c.<路径中的点分形式>.o     （apps 侧为避免重名）
    两种都取出最前面的源文件名。
    """
    names = set()
    for dirpath, _dirs, files in os.walk(ws):
        if '/.git' in dirpath:
            continue
        for fn in files:
            if not fn.endswith('.o'):
                continue
            base = fn[:-2]
            if '.c.' in base:
                base = base.split('.c.')[0] + '.c'
            elif not base.endswith('.c'):
                base = base + '.c'
            names.add(base)
    return names


def main():
    if not os.path.exists(DOTC):
        sys.exit('找不到 %s' % DOTC)

    on, _ = load_config(DOTC)
    built = compiled_sources(WS)
    if not built:
        sys.exit('构建产物为空 —— 请先 make 一次，本检查依赖 .o 判断'
                 '哪些源文件真正参与了编译')
    hits = {}

    for base in (os.path.join(WS, 'apps'), os.path.join(WS, 'nuttx')):
        for dirpath, _dirs, files in os.walk(base):
            if '/.git' in dirpath:
                continue
            for fn in files:
                if not fn.endswith('.c') or fn not in built:
                    continue          # 没参与编译，与本配置无关
                p = os.path.join(dirpath, fn)
                try:
                    with open(p, errors='replace') as f:
                        text = f.read()
                except OSError:
                    continue
                for key, fallback in PAT.findall(text):
                    fallback = fallback.strip()
                    if key in on:
                        continue
                    if not NUM.match(fallback):
                        continue          # 非数值兜底，跳过
                    hits.setdefault(key, (fallback, os.path.relpath(p, WS)))

    if not hits:
        print('✓ 没有发现"缺失且带数值兜底"的配置项')
        return

    print('以下配置项不在 .config 里，代码会使用自带的数值兜底：\n')
    for key in sorted(hits):
        fb, where = hits[key]
        print('  ✗ %-42s 兜底值 %-10s  %s' % (key, fb, where))

    print('\n这些项不会报错，但兜底值未必适合本平台 —— arm64 的栈帧比 32 位')
    print('大得多，栈相关项尤其要核对。按 Kconfig 的 default 补进 .config')
    print('与 board 的 defconfig，然后 cd nuttx && make include/nuttx/config.h')


main()
