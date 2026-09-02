#!/usr/bin/env python3
"""把一棵 Kconfig 子树的默认值导出成 .config 片段。

为什么需要它：本工作区的 `make olddefconfig` 跑不通 —— 上游多个
Kconfig（arch/tricore、frameworks/runtimes/quickapp 等）里有 `--help--`
这类语法错误，配套的 kconfig-conf 直接报错退出。于是新加子系统（如 LVGL）
时，它那几百个带默认值的符号不会被自动填进 .config，代码里的
`#ifdef CONFIG_LV_*` 全部落空，表现为一堆 -Werror=undef，而不是"没开这个
功能"。

本脚本只做 olddefconfig 在**单棵子树**上会做的那件事：把带无条件
`default` 的符号按默认值写出来，已经在 .config 里的一律不动。

  ./gen-kconfig-defaults.py <Kconfig 文件> <.config> [更多 .config …]

带条件的 default 只支持**单个符号**的形式（`default y if FOO` /
`default y if !FOO`），按目标 .config 里该符号的状态求值。更复杂的
表达式一律跳过 —— 求值需要完整符号表，猜错比不写更糟；这类项若确实
需要，手工补并写明出处。
"""
import re
import sys


def parse(kconfig_path):
    """返回 [(名字, 类型, 默认值, 条件, 依赖列表)]。"""
    out = []
    name = None
    ctype = None
    default = None
    depends = []

    def flush():
        if name and ctype and default is not None:
            out.append((name, ctype) + default + (list(depends),))

    with open(kconfig_path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()

            m = re.match(r"^config\s+([A-Za-z0-9_]+)$", line)
            if m:
                flush()
                name, ctype, default = m.group(1), None, None
                depends = []
                continue

            if re.match(r"^(menuconfig|choice|endchoice|menu|endmenu|if|endif|source|osource)\b", line):
                flush()
                name, ctype, default = None, None, None
                depends = []
                continue

            if name is None:
                continue

            m = re.match(r"^(bool|int|hex|string|tristate)\b", line)
            if m:
                ctype = m.group(1)
                continue

            m = re.match(r"^depends on\s+(.+)$", line)
            if m:
                # ★ 必须处理 depends on。被依赖项没开时，kconfig 根本不会
                # 产生这个符号；照默认值写出来会造出"父功能关着、子功能开着"
                # 的非法组合 —— 表现为调用未声明的函数，而不是配置报错。
                #
                # 只认由 && 连接的单符号（可带 !），出现 || 或括号一律
                # 视为无法判断，保守跳过整个符号。
                expr = m.group(1).strip()
                if "||" in expr or "(" in expr:
                    depends.append("?")
                else:
                    for part in expr.split("&&"):
                        part = part.strip()
                        if re.match(r"^!?[A-Za-z0-9_]+$", part):
                            depends.append(part)
                        else:
                            depends.append("?")
                continue

            m = re.match(r"^default\s+(.+)$", line)
            if m and default is None:
                val = m.group(1).strip()
                cond = None
                if " if " in val:
                    val, cond = val.split(" if ", 1)
                    val = val.strip()
                    cond = cond.strip()

                    # 只认单符号条件
                    if not re.match(r"^!?[A-Za-z0-9_]+$", cond):
                        continue

                default = (val, cond)
                continue

    flush()
    return out


def cond_true(cond, enabled):
    """求值单符号条件。未知符号按未开启处理。"""
    if cond is None:
        return True

    if cond.startswith("!"):
        return cond[1:] not in enabled

    return cond in enabled


def emit(entries, existing, enabled):
    lines = []
    for name, ctype, val, cond, depends in entries:
        key = "CONFIG_" + name
        if key in existing:
            continue

        if not cond_true(cond, enabled):
            continue

        if any(d == "?" or not cond_true(d, enabled) for d in depends):
            continue

        if ctype in ("bool", "tristate"):
            if val == "y":
                lines.append("%s=y" % key)
            elif val == "n":
                lines.append("# %s is not set" % key)
            # 其它取值（如引用别的符号）跳过
        elif ctype == "string" and val in ('""', "''"):
            # ★ 默认值是空字符串的，一律不写。
            #
            # 这类符号的本意是"不定义"，让代码走 #else 分支。写成
            #   CONFIG_LV_ATTRIBUTE_LARGE_CONST=""
            # 之后，config.h 里就有了 #define ... ""，代码里
            #   LV_ATTRIBUTE_LARGE_CONST const uint8_t bitmap[]
            # 展开成 `"" const uint8_t bitmap[]`，直接语法错误。
            continue
        else:
            lines.append("%s=%s" % (key, val))
    return lines


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    entries = parse(sys.argv[1])
    print("从 %s 解析出 %d 个带无条件默认值的符号" % (sys.argv[1], len(entries)))

    for cfg in sys.argv[2:]:
        existing = set()
        enabled = set()          # 值为 y 的符号，去掉 CONFIG_ 前缀
        with open(cfg, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("CONFIG_") and "=" in line:
                    k, v = line.split("=", 1)
                    existing.add(k)
                    if v == "y":
                        enabled.add(k[len("CONFIG_"):])
                elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
                    existing.add(line.split()[1])

        new = emit(entries, existing, enabled)
        if not new:
            print("  %s：无新增" % cfg)
            continue

        with open(cfg, "a", encoding="utf-8") as fh:
            fh.write("\n")
            fh.write("\n".join(new))
            fh.write("\n")
        print("  %s：追加 %d 项" % (cfg, len(new)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
