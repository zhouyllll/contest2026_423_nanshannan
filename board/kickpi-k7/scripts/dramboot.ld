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
  /* 镜像加载地址 0x42000000。
   *
   * 来源：U-Boot 主线 include/configs/rk3576_common.h（Rockchip 官方提交）
   *   #define CFG_SYS_SDRAM_BASE   0x40000000
   *   ENV_MEM_LAYOUT_SETTINGS: "kernel_addr_r=0x42000000"
   *
   * ★★ 注意 RK3576 的 DRAM 从 0x40000000 起，与 RK3568（从 0 起，
   *    kernel_addr_r=0x02000000）和 RK3399（0x02080000）都不同。
   *    RK3576 把外设放在了低位地址（0x22000000~0x2b060000），
   *    照抄 RK3568 的 0x02000000 会落进外设区，表现是完全没有输出。
   *
   * 加载模型（与 RK3399/RK3568 端口一致）：把 nuttx.bin 当作 Linux 的
   * Image，由 U-Boot 的 booti 加载到 kernel_addr_r 运行。
   *
   *   BootROM -> TPL(ddr.bin) -> SPL -> U-Boot -> booti ${kernel_addr_r}
   *
   * 板厂可能改过该环境变量，上板后用 printenv kernel_addr_r 复核一次
   * （RECON B3）。
   *
   * ★ 本值必须与另外两处保持一致，改一处就要改三处：
   *   - arch/arm64/include/rk3576/chip.h  的 CONFIG_LOAD_BASE
   *   - configs/nsh/defconfig             的 CONFIG_RAM_START
   *   - 本文件下面这一行
   *
   * 地址填错的表现是串口完全没有输出，且没有任何报错 —— 因为 CPU
   * 跳到的位置根本不是你的代码。这正是 M1 阶段最难排查的一类问题，
   * 所以先用 U-Boot 的 md 命令确认镜像确实落在预期地址上再 booti。
   */
  . = 0x42000000;  /* U-Boot loads NuttX at this address (kernel_addr_r) */
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
