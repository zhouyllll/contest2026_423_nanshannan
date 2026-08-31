/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/scripts/dramboot.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

OUTPUT_ARCH(aarch64)

ENTRY(__start)

PHDRS
{
  text PT_LOAD ;
}

SECTIONS
{
  /* 链接地址 0x40480000 —— 必须等于 U-Boot 实际搬运后的执行地址。
   *
   * ★ 板上实测的完整加载链（2026-08-31）：
   *
   *     mmc read -> 0x40400000        (kernel_addr_r，eMMC uboot 分区
   *                                    明文环境变量读出；主线
   *                                    rk3576_common.h 写 0x42000000，
   *                                    板厂改过，以实测为准)
   *     U-Boot 按 ARM64 Linux Image 协议搬运：
   *         DRAM 基址 0x40000000 + Image 头 text_offset 0x480000
   *       = 0x40480000                ← CPU 真正开始取指的地址
   *
   *   串口原文：
   *     == DO RELOCATE == Kernel from 0x40400000 to 0x40480000
   *
   *   text_offset 位于 Image 头偏移 8（8 字节小端），NuttX 在
   *   arch/arm64/src/common/arm64_head.S 里硬编码为 0x480000。
   *
   * ★★ 曾经踩过的坑：一度以为「Rockchip U-Boot 2017.09 不搬运，
   *    原地执行」，于是把链接地址填成 0x40400000。表面上能跑完
   *    汇编启动和 arm64_chip_boot()，因为搬运后 0x40400000 上还残留
   *    着一份内容完全相同的原始副本，所有 .data/.bss/向量表的绝对
   *    引用都打在那份副本上，看起来一切正常。
   *    真正的爆点在 nx_start()：堆从 _ebss（按 0x40400000 算，约
   *    0x4044C000）起分配，而正在执行的代码就在 0x40480000 —— 堆一
   *    扩张就把自己覆盖掉，表现为 C 运行时静默挂死、无任何异常报告。
   *    结论：链接地址要填搬运后的地址，不是 mmc read 的地址。
   *
   * ★★★ RK3576 的 DRAM 从 0x40000000 起，与 RK3568（从 0 起，
   *    kernel_addr_r=0x02000000）和 RK3399（0x02080000）都不同。
   *    RK3576 把外设放在低位地址（0x22000000~0x2b060000），
   *    照抄 RK3568 的 0x02000000 会落进外设区，表现是完全没有输出。
   *
   *   BootROM -> TPL(ddr.bin) -> SPL -> U-Boot -> booti -> 搬运 -> 本镜像
   *
   * ★ 本值必须与另外两处保持一致，改一处就要改三处
   *   （scripts/check-addr.sh 会校验）：
   *   - arch/arm64/include/rk3576/chip.h  的 CONFIG_LOAD_BASE
   *   - configs/nsh/defconfig             的 CONFIG_RAM_START
   *   - 本文件下面这一行
   *
   * 地址填错的表现是串口完全没有输出且无任何报错 —— CPU 跳到的位置
   * 根本不是你的代码。上板后先用 md 确认镜像落点再 booti。
   */
  . = 0x40480000;
  _start = .;
  .text : {
        _stext = .;            /* Text section */
       *(.start .start.*)      /* Place __start here */
       *(.text)
       *(.text.cold)
       *(.text.unlikely)
       *(.fixup)
       *(.gnu.warning)
  } :text = 0x9090

  . = ALIGN(4096);

  .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
  }

  . = ALIGN(4096);

  .vector : {
        _vector_start = .;
        KEEP(*(.exc_vector_table))
        KEEP(*(".exc_vector_table.*"))
        KEEP(*(.vectors))
       _vector_end = .;
  } :text
  . = ALIGN(4096);
  _etext = .; /* End_1 of .text */
  _sztext = _etext - _stext;

  . = ALIGN(4096);
  .rodata : {
        _srodata = .;          /* Read-only data */
       *(.rodata)
       *(.rodata.*)
       *(.data.rel.ro)
       *(.data.rel.ro.*)
  } :text
  . = ALIGN(4096);
  _erodata = .;                /* End of read-only data */
  _szrodata = _erodata - _srodata;
  _eronly = .;  /* End of read-only data */

  . = ALIGN(4096);
  .data : {                    /* Data */
        _sdata = .;
       *(.data.page_aligned)
       *(.data)
       . = ALIGN(8);
       *(.data.rel)
       *(.data.rel.*)
       CONSTRUCTORS
  } :text
  _edata = .; /* End+1 of .data */

  .bss : {                     /* BSS */
       . = ALIGN(8);
       _sbss = .;
       *(.bss)
       . = ALIGN(8);
  } :text
  . = ALIGN(4096);
  _ebss = .;
  _szbss = _ebss - _sbss;

  .initstack : {             /* INIT STACK */
       _s_initstack = .;
       *(.initstack)
       . = ALIGN(16);
  } :text
  . = ALIGN(4096);
  _e_initstack = . ;
  g_idle_topstack = . ;

  _szdata = _e_initstack - _sdata;

  /* Sections to be discarded */
  /DISCARD/ : {
       *(.exit.text)
       *(.exit.data)
       *(.exitcall.exit)
       *(.eh_frame)
  }

  /* Stabs debugging sections.  */
  .stab 0 : { *(.stab) }
  .stabstr 0 : { *(.stabstr) }
  .stab.excl 0 : { *(.stab.excl) }
  .stab.exclstr 0 : { *(.stab.exclstr) }
  .stab.index 0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment 0 : { *(.comment) }
}
