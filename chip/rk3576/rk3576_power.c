/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_power.c
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

/* RK3576 电源域控制（PMU）。
 *
 * 只做"打开"和"查询"，不做掉电 —— 本端口没有休眠需求，而掉电的握手
 * 更复杂（要先让 NIU 进 idle、等应答，还要保存/恢复 QoS），实现了也
 * 无处验证，徒增出错面。
 *
 * ★ 数据出处：Linux drivers/pmdomain/rockchip/pm-domains.c
 *
 *     static const struct rockchip_pmu_info rk3576_pmu = {
 *         .pwr_offset = 0x210,  .status_offset = 0x230,
 *         .req_offset = 0x110,  .idle_offset   = 0x128,
 *         .ack_offset = 0x120,  .repair_status_offset = 0x570,
 *         .clk_ungate_offset = 0x140,
 *     };
 *
 *     DOMAIN_RK3576(name, p_offset, pwr, status, r_status,
 *                   r_offset, req, idle, g_mask, wakeup)
 *
 *   PMU 基址取自主线 rk3576.dtsi 的 pmu: power-management@27380000。
 *
 * ★ 状态判读的两个反直觉之处，看错就会一直等下去：
 *
 *   1. repair_status：1 = 已上电，0 = 已断电（正逻辑）
 *   2. status：      0 = 已上电，1 = 已断电（反逻辑）
 *
 *   RK3576 的各域都给了 repair_status，因此本实现只用第 1 种。
 *
 * ★ 写寄存器一律带高 16 位写使能掩码，与 CRU/GPIO 同一套约定。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_power.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PMU_BASE                  RK3576_PMU_ADDR

#define PMU_REQ_OFFSET            0x0110   /* idle 请求        */
#define PMU_ACK_OFFSET            0x0120   /* idle 应答        */
#define PMU_IDLE_OFFSET           0x0128   /* idle 状态        */
#define PMU_CLK_UNGATE_OFFSET     0x0140   /* 时钟门控解除     */
#define PMU_PWR_OFFSET            0x0210   /* 电源开关         */
#define PMU_STATUS_OFFSET         0x0230
#define PMU_REPAIR_STATUS_OFFSET  0x0570   /* 1=已上电         */

#define PMU_POLL_TIMEOUT_US       10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_pd_info_s
{
  const char *name;
  uint32_t    pwr_mask;        /* PWR 寄存器里的位            */
  uint32_t    repair_mask;     /* REPAIR_STATUS 里的位        */
  uint32_t    req_offset;      /* ★ 每域各自的 req 寄存器偏移 */
  uint32_t    req_mask;        /* idle 请求位                 */
  uint32_t    idle_mask;       /* idle 应答/状态位            */
  uint32_t    clk_ungate_mask; /* 握手期间要解除门控的时钟    */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 逐项对应 pm-domains.c 的 rk3576_pm_domains[]。
 * 只收录本端口会用到的域，用不到的留空以免误用未经验证的参数。
 */

static const struct rk3576_pd_info_s g_pd_info[] =
{
  /* 字段顺序：name, pwr, repair, req_offset, req, idle, clk_ungate
   * 对应 DOMAIN_RK3576(name, p_offset, pwr, status, r_status,
   *                    r_offset, req, idle, g_mask, wakeup)
   */

  [RK3576_PD_NVM] =
  {
    /* DOMAIN_RK3576("nvm", 0x0, BIT(6), 0, BIT(6), 0x4, BIT(2), BIT(18), BIT(2)) */

    "nvm",   1u << 6,  1u << 6,  0x4, 1u << 2,  1u << 18, 1u << 2
  },
  [RK3576_PD_SDGMAC] =
  {
    /* DOMAIN_RK3576("sdgmac", 0x0, BIT(7), 0, BIT(7), 0x4, BIT(1), BIT(17), 0x6) */

    "sdgmac", 1u << 7, 1u << 7, 0x4, 1u << 1,  1u << 17, 0x6
  },
  [RK3576_PD_AUDIO] =
  {
    /* DOMAIN_RK3576("audio", 0x0, BIT(8), 0, BIT(8), 0x4, BIT(0), BIT(16), BIT(0)) */

    "audio", 1u << 8,  1u << 8,  0x4, 1u << 0,  1u << 16, 1u << 0
  },
  [RK3576_PD_VO0] =
  {
    /* DOMAIN_RK3576("vo0", 0x0, BIT(15), 0, BIT(15), 0x0, BIT(11), BIT(11), 0x6800) */

    "vo0",   1u << 15, 1u << 15, 0x0, 1u << 11, 1u << 11, 0x6800
  },
  [RK3576_PD_VOP] =
  {
    /* DOMAIN_RK3576("vop", 0x0, BIT(11), 0, BIT(11), 0x0, 0x6000, 0x6000, 0x6000) */

    "vop",   1u << 11, 1u << 11, 0x0, 0x6000,   0x6000,   0x6000
  },
};

#define RK3576_PD_COUNT (sizeof(g_pd_info) / sizeof(g_pd_info[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t pmu_getreg(uint32_t off)
{
  return getreg32(PMU_BASE + off);
}

static inline void pmu_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, PMU_BASE + off);
}

/****************************************************************************
 * Name: pmu_write_masked
 *
 * Description:
 *   带写使能掩码的位写。set 为 true 时置位，false 时清位。
 *
 ****************************************************************************/

static void pmu_write_masked(uint32_t off, uint32_t mask, bool set)
{
  pmu_putreg(off, (mask << 16) | (set ? mask : 0));
}

/****************************************************************************
 * Name: pmu_set_idle_request
 *
 * Description:
 *   向 NIU 发/撤 idle 请求，并等待应答到位。
 *
 *   上电流程里这一步是"撤销"（idle=false），必须在电源打开之后做；
 *   顺序颠倒会让总线在无电时收到事务。
 *
 ****************************************************************************/

static int pmu_set_idle_request(const struct rk3576_pd_info_s *pd, bool idle)
{
  uint32_t target;
  int us;

  if (pd->req_mask == 0)
    {
      return OK;
    }

  /* ★ req 寄存器地址 = 全局 req_offset + 本域各自的偏移。
   * 漏掉后半截时，req_offset 为 0 的域（VOP/VO0）恰好正确，
   * 而 0x4 的域（NVM/AUDIO）会写到别的域的请求位上。
   */

  pmu_write_masked(PMU_REQ_OFFSET + pd->req_offset, pd->req_mask, idle);

  target = idle ? pd->idle_mask : 0;
  for (us = 0; us < PMU_POLL_TIMEOUT_US; us++)
    {
      if ((pmu_getreg(PMU_ACK_OFFSET) & pd->idle_mask) == target)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "PMU: %s idle%s 应答超时 ACK=0x%08" PRIx32 "\n",
         pd->name, idle ? "请求" : "撤销", pmu_getreg(PMU_ACK_OFFSET));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool rk3576_power_is_on(int domain)
{
  if (domain < 0 || domain >= RK3576_PD_COUNT ||
      g_pd_info[domain].pwr_mask == 0)
    {
      return false;
    }

  /* repair_status 是正逻辑：1 = 已上电。
   * 注意不要用 PMU_STATUS_OFFSET —— 那个是反逻辑（0 才是已上电），
   * 两者搞混会得到完全相反的结论。
   */

  return (pmu_getreg(PMU_REPAIR_STATUS_OFFSET) &
          g_pd_info[domain].repair_mask) != 0;
}

int rk3576_power_on(int domain)
{
  const struct rk3576_pd_info_s *pd;
  int ret;
  int us;

  if (domain < 0 || domain >= RK3576_PD_COUNT ||
      g_pd_info[domain].pwr_mask == 0)
    {
      syslog(LOG_ERR, "PMU: 电源域 %d 未收录\n", domain);
      return -EINVAL;
    }

  pd = &g_pd_info[domain];

  if (rk3576_power_is_on(domain))
    {
      syslog(LOG_INFO, "PMU: %s 已上电\n", pd->name);
      return OK;
    }

  /* 1) 握手期间解除相关时钟的门控 —— NIU 要有时钟才能应答 */

  if (pd->clk_ungate_mask != 0)
    {
      pmu_write_masked(PMU_CLK_UNGATE_OFFSET, pd->clk_ungate_mask, true);
    }

  /* 2) 开电。电源位是反逻辑：写 0 为上电。 */

  pmu_write_masked(PMU_PWR_OFFSET, pd->pwr_mask, false);

  /* 3) 等上电完成 */

  ret = -ETIMEDOUT;
  for (us = 0; us < PMU_POLL_TIMEOUT_US; us++)
    {
      if (rk3576_power_is_on(domain))
        {
          ret = OK;
          break;
        }

      up_udelay(1);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "PMU: %s 上电超时 REPAIR=0x%08" PRIx32 "\n",
             pd->name, pmu_getreg(PMU_REPAIR_STATUS_OFFSET));
    }
  else
    {
      /* 4) 上电成功后才撤销 idle 请求，让总线开始接受事务 */

      ret = pmu_set_idle_request(pd, false);
    }

  /* 5) 恢复时钟门控 */

  if (pd->clk_ungate_mask != 0)
    {
      pmu_write_masked(PMU_CLK_UNGATE_OFFSET, pd->clk_ungate_mask, false);
    }

  if (ret == OK)
    {
      syslog(LOG_INFO, "PMU: %s 上电完成\n", pd->name);
    }

  return ret;
}
