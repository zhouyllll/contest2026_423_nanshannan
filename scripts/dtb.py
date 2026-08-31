#!/usr/bin/env python3
"""最小 DTB 解析器：只做遍历 + 按名字/属性过滤，够查外设用。"""
import struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1,2,3,4,9

class DTB:
    def __init__(self, path):
        d = open(path,'rb').read(); self.d = d
        magic, totalsize, off_struct, off_strings = struct.unpack('>4I', d[0:16])
        assert magic == 0xd00dfeed
        self.off_struct, self.off_strings = off_struct, off_strings
        self.size_struct = struct.unpack('>I', d[36:40])[0]

    def s(self, off):
        e = self.d.index(b'\0', self.off_strings+off)
        return self.d[self.off_strings+off:e].decode()

    def walk(self):
        """yield (path, {prop: bytes})"""
        d, p = self.d, self.off_struct
        end = self.off_struct + self.size_struct
        # ★ 属性要按层维护成栈：进入子节点时若把 props 清空，
        #   父节点的属性就丢了（有子器件的总线会显示不出 compatible）。
        stack, propstack = [], []
        while p < end:
            tok = struct.unpack('>I', d[p:p+4])[0]; p += 4
            if tok == FDT_BEGIN_NODE:
                e = d.index(b'\0', p)
                name = d[p:e].decode()
                p = (e + 4) & ~3
                stack.append(name); propstack.append({})
            elif tok == FDT_PROP:
                ln, nameoff = struct.unpack('>II', d[p:p+8]); p += 8
                val = d[p:p+ln]; p = (p + ln + 3) & ~3
                propstack[-1][self.s(nameoff)] = val
            elif tok == FDT_END_NODE:
                yield '/'.join(stack), propstack[-1]
                stack.pop(); propstack.pop()
            elif tok == FDT_END:
                break
    @staticmethod
    def u32s(v): return list(struct.unpack('>%dI'%(len(v)//4), v[:len(v)//4*4]))
    @staticmethod
    def strs(v): return [x.decode() for x in v.split(b'\0') if x]
