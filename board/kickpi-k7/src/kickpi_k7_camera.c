/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_camera.c
 *
 * MIPI CSI 摄像头（Sony IMX415）的传感器探测。
 *
 * ★ 本文件只做到「确认传感器活着」，不出图像。
 *
 *   完整取图通路是 imx415 -> csi2_dcphy0 -> mipi0_csi2 -> rkcif -> rkisp，
 *   四级都要驱动，工作量远大于本轮范围。先把最前一级坐实：给传感器
 *   供电、按规程唤醒、读芯片 ID —— 这一步通过就说明 I2C 通路、供电、
 *   外部时钟三者都正常，后续做 CSI 接收端时不必再怀疑它们。
 *
 * 板级依据全部来自厂商 Armbian 源码的
 * patch/kernel/rk35xx-vendor-6.1/dt/rk3576-kickpi-k7-cam0.dtsi：
 *
 *   imx415_0@37   挂在 I2C4（pinctrl 用 i2c4m3_xfer = GPIO3_B0/A7 功能 11）
 *   xvclk         板上 37.125MHz 固定晶振，不由 SoC 提供，无需配时钟
 *   avdd-supply   vcc_mipidcphy0，由 GPIO0_D2 控制，高有效
 *   data-lanes    4
 *   power-domains RK3576_PD_VI
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_i2c.h"
#include <arch/board/board.h>
#include "kickpi_k7.h"

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAM_I2C_ADDR       0x37

/* ★ 板上有三个 CSI 接口，各走不同的 I2C 总线与供电脚。模组插在哪一个
 *   事先不知道，逐个试比要求用户说明更可靠 —— 而且"三个都没应答"和
 *   "某一个有"是完全不同的结论，遍历一次就能分清。
 *
 *   出处：厂商 Armbian 的 rk3576-kickpi-k7-cam{0,1,3}.dtsi
 */

struct cam_port_s
{
  const char *name;
  uint8_t     bus;
  int8_t      pwr_bank;      /* PDN 脚（低电平=掉电），高有效使能 */
  int8_t      pwr_pin;
  int8_t      clk_bank;      /* MCLK 输出脚 */
  int8_t      clk_pin;
  uint8_t     clk_sel_con;   /* CLK_MIPI_CAMERAOUT_Mx 的 CLKSEL_CON */
  uint8_t     clk_gate_bit;  /* 同上，在 CLKGATE_CON(6) 里的位号     */
};

static const struct cam_port_s g_cam_ports[] =
{
  /* 名称  I2C  PDN 脚      MCLK 脚      CLKSEL  门控位 */

  { "cam0", 4, 0, 26,      3, 31,       38,     3 },
  { "cam1", 5, 4, 22,      4,  0,       39,     4 },
  { "cam3", 8, 0, 23,      4,  1,       40,     5 },
};

#define CAM_NPORTS (sizeof(g_cam_ports) / sizeof(g_cam_ports[0]))

/* 传感器寄存器（Linux drivers/media/i2c/imx415.c）
 *
 *   0x3000 MODE          bit0: 1=待机 0=工作
 *   0x3f12 SENSOR_INFO   16 位小端，低 12 位为型号，IMX415 = 0x514
 *
 * ★ SENSOR_INFO 在待机状态下读不出来 —— 驱动里专门有一句注释说明
 *   这一点。必须先把 MODE 写 0 唤醒并等待，否则读到的是垃圾值，
 *   会被误判成"传感器不在"。
 */

#define IMX415_REG_MODE    0x3000
#define IMX415_MODE_OPER   0x00
#define IMX415_MODE_STBY   0x01
#define IMX415_REG_INFO    0x3f12
#define IMX415_CHIP_ID     0x514

/* ★ 摄像头模组的 INCK 由 SoC 输出，不是板上晶振。
 *
 *   原理图上模组接口 Pin17 是 MIPI_MCLK0，由 MIPI_DPHY_CSI0_CAM_CLKOUT
 *   驱动；厂商 dtsi 把它写成 fixed-clock(37.125MHz) 只是内核里的建模
 *   方式，物理上必须由 SoC 的 CAM_CLK0_OUT 送出去。
 *
 *   IMX415 没有 INCK 就**不会在 I2C 上应答** —— 现象与"模组没插"完全
 *   一样，事后无法区分。所以探测前必须先把时钟开起来。
 *
 *   时钟树（clk-rk3576.c）：
 *     COMPOSITE(CLK_MIPI_CAMERAOUT_M0, mux_24m_spll_gpll_cpll_p,
 *               RK3576_CLKSEL_CON(38), 8, 2, MFLAGS,  选源 bit[9:8]
 *                                      0, 8, DFLAGS,  分频 bit[7:0]
 *               RK3576_CLKGATE_CON(6), 3, GFLAGS)
 *     父时钟表 {xin24m, spll, gpll, cpll}
 *
 *   37.125MHz 从哪来：gpll 1188MHz / 32 = 37.125MHz，整除。
 *   24M 和 spll(702M) 都除不出这个频率。
 *
 *   引脚（原厂 dtb 的 cam_clk0m0-clk0 等）：
 *     cam0 <3 31 3>  = GPIO3_D7 功能 3
 *     cam1 <4  0 3>  = GPIO4_A0 功能 3
 *     cam3 <4  1 3>  = GPIO4_A1 功能 3
 */

#define CAM_CLK_GATE_CON   6
#define CAM_CLK_MUX_SHIFT  8
#define CAM_CLK_MUX_GPLL   2
#define CAM_CLK_DIV_SHIFT  0
#define CAM_CLK_GPLL_HZ    1188000000u
#define CAM_CLK_TARGET_HZ  37125000u
#define CAM_CLK_PIN_FUNC   3

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: imx415_write8 / imx415_read8
 *
 * Description:
 *   IMX415 用 16 位寄存器地址、高字节在前。
 *
 ****************************************************************************/

static int imx415_write8(struct i2c_master_s *i2c, uint16_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[3];

  buf[0] = (uint8_t)(reg >> 8);
  buf[1] = (uint8_t)(reg & 0xff);
  buf[2] = val;

  msg.frequency = 400000;
  msg.addr      = CAM_I2C_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = sizeof(buf);

  return I2C_TRANSFER(i2c, &msg, 1);
}

static int imx415_read8(struct i2c_master_s *i2c, uint16_t reg, uint8_t *val)
{
  struct i2c_msg_s msg[2];
  uint8_t regaddr[2];

  regaddr[0] = (uint8_t)(reg >> 8);
  regaddr[1] = (uint8_t)(reg & 0xff);

  msg[0].frequency = 400000;
  msg[0].addr      = CAM_I2C_ADDR;
  msg[0].flags     = 0;
  msg[0].buffer    = regaddr;
  msg[0].length    = 2;

  msg[1].frequency = 400000;
  msg[1].addr      = CAM_I2C_ADDR;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = val;
  msg[1].length    = 1;

  return I2C_TRANSFER(i2c, msg, 2);
}

/****************************************************************************
 * Name: cam_probe_port
 *
 * Description:
 *   在一个 CSI 接口上尝试识别 IMX415。返回 OK 表示识别成功。
 *
 ****************************************************************************/

static int cam_probe_port(const struct cam_port_s *port)
{
  struct i2c_master_s *i2c;
  uint8_t lo;
  uint8_t hi;
  uint16_t info;
  int ret;

  /* 传感器模拟供电。dtsi 里是个 regulator-fixed，使能脚高有效。
   * 不开的话 I2C 上没有任何应答 —— 与触摸那次"没供电扫不到器件"
   * 是同一类问题。
   */

  rk3576_pinmux_set(port->pwr_bank, port->pwr_pin, RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(port->pwr_bank, port->pwr_pin, true);
  rk3576_gpio_write(port->pwr_bank, port->pwr_pin, true);
  up_mdelay(20);

  /* ★ 再开 MCLK。传感器靠它产生内部时序，没有它连 I2C 都不会应答。
   * 选源 gpll，分频 32 -> 37.125MHz。分频字段写 (n-1)。
   */

  {
    uint32_t div = CAM_CLK_GPLL_HZ / CAM_CLK_TARGET_HZ;

    rk3576_pinmux_set(port->clk_bank, port->clk_pin, CAM_CLK_PIN_FUNC);
    rk3576_clk_setmux(port->clk_sel_con, CAM_CLK_MUX_SHIFT, 2,
                      CAM_CLK_MUX_GPLL);
    rk3576_clk_setmux(port->clk_sel_con, CAM_CLK_DIV_SHIFT, 8, div - 1);
    rk3576_clk_gate(CAM_CLK_GATE_CON, port->clk_gate_bit, true);

    syslog(LOG_INFO,
           "摄像头: %s MCLK gpio%d-%d 功能%d，gpll/%" PRIu32
           " = %" PRIu32 "Hz\n",
           port->name, port->clk_bank, port->clk_pin, CAM_CLK_PIN_FUNC,
           div, CAM_CLK_GPLL_HZ / div);

    /* 传感器上电后需要若干个 INCK 周期才能响应，给足余量。 */

    up_mdelay(30);
  }

  i2c = rk3576_i2cbus_initialize(port->bus);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: %s 所在 I2C%d 初始化失败\n",
             port->name, port->bus);
      return -ENODEV;
    }

  /* ★ 型号寄存器在待机状态下读不出来（Linux 驱动里专门注释了这一点），
   * 必须先写 MODE=0 退出待机，否则读到的是垃圾值，会被误判成不在位。
   */

  ret = imx415_write8(i2c, IMX415_REG_MODE, IMX415_MODE_OPER);
  if (ret < 0)
    {
      return ret;
    }

  /* 手册说退出待机等 63us，但 Linux 驱动实测要到毫秒量级，取 80ms。 */

  up_mdelay(80);

  ret = imx415_read8(i2c, IMX415_REG_INFO, &lo);
  if (ret >= 0)
    {
      ret = imx415_read8(i2c, IMX415_REG_INFO + 1, &hi);
    }

  if (ret < 0)
    {
      return ret;
    }

  info = (uint16_t)lo | ((uint16_t)hi << 8);

  if ((info & 0xfff) != IMX415_CHIP_ID)
    {
      syslog(LOG_WARNING,
             "摄像头: %s 上有器件应答，但型号 0x%04x 不是 IMX415"
             "（期望低 12 位 0x%03x）\n",
             port->name, info, IMX415_CHIP_ID);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "摄像头: IMX415 已识别 %s @I2C%d:0x%02x 型号=0x%03x（4 lane）\n",
         port->name, port->bus, CAM_I2C_ADDR, info & 0xfff);

  /* 放回待机 —— 取图通路（CSI2/CIF/ISP）尚未实现，让它一直工作没有意义。 */

  imx415_write8(i2c, IMX415_REG_MODE, IMX415_MODE_STBY);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_camera_initialize(void)
{
  int found = 0;
  int i;

  for (i = 0; i < (int)CAM_NPORTS; i++)
    {
      if (cam_probe_port(&g_cam_ports[i]) == OK)
        {
          found++;
        }
    }

  if (found == 0)
    {
      /* 三个接口都没应答。到这一步能确定的是：I2C 控制器和引脚复用
       * 都正常（其余总线上的器件都扫得到），所以问题在模组侧 ——
       * 没插好、供电脚编号不对，或模组不是 IMX415。
       */

      syslog(LOG_WARNING,
             "摄像头: cam0/cam1/cam3 三个接口均未发现 IMX415\n");
      return -ENODEV;
    }

  syslog(LOG_INFO, "摄像头: 共发现 %d 个 IMX415\n", found);
  return OK;
}

#endif /* CONFIG_RK3576_I2C */
