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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>
#include <syslog.h>
#include <stdlib.h>
#include <nuttx/kmalloc.h>
#include <nuttx/cache.h>

#include <nuttx/i2c/i2c_master.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_i2c.h"
#include <arch/board/board.h>
#include "kickpi_k7.h"
#include "rk3576_cif.h"
#include "rk3576_vop2.h"
#include "rk3576_csidphy.h"
#include "rk3576_csihost.h"
#include "imx415_regs.h"

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

/* 探测成功的那一路。出流要用同一条 I2C 与同一个传感器，
 * 分开记比每次重新遍历可靠 —— 遍历会把已经配好的那颗再唤醒一遍。
 */

static const struct cam_port_s *g_cam_found;
static struct i2c_master_s     *g_cam_i2c;

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

  /* 放回待机。取图通路还没通，让它一直出流只是白耗电、白发热；
   * 真要取图时由 kickpi_camera_stream() 重新配置。
   */

  imx415_write8(i2c, IMX415_REG_MODE, IMX415_MODE_STBY);

  g_cam_found = port;
  g_cam_i2c   = i2c;
  return OK;
}

/****************************************************************************
 * Name: cam_write_array
 *
 * Description:
 *   顺序写一张寄存器表。任何一条失败都立刻停下并报出**是哪一条** ——
 *   一张表上百条，只说"配置失败"等于没说。
 *
 ****************************************************************************/

static int cam_write_array(struct i2c_master_s *i2c,
                           const struct imx415_reg_s *regs, size_t n,
                           const char *tag)
{
  size_t i;
  int ret;

  for (i = 0; i < n; i++)
    {
      ret = imx415_write8(i2c, regs[i].addr, regs[i].val);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: 摄像头 %s 表第 %zu/%zu 条失败 "
                 "(0x%04x=0x%02x): %d\n",
                 tag, i, n, regs[i].addr, regs[i].val, ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_camera_stream
 *
 * Description:
 *   让已识别的 IMX415 开始或停止在 MIPI 上输出图像。
 *
 *   ★ 这一步单独拿出来，是因为它是整条取图链路里**唯一现在就能验证**
 *     的一级。接收端（D-PHY RX → CSI-2 host → CIF）还没写，所以拿不到
 *     图像；但"传感器有没有按要求配置好并开始推数据"这件事，可以只靠
 *     I2C 回读确认，不依赖接收端。
 *
 *     先把这一级坐实，后面调接收端时就不必再怀疑发送端 —— 否则两端
 *     同时不确定，任何现象都有两个解释。
 *
 * Input Parameters:
 *   on - true 出流，false 回待机
 *
 ****************************************************************************/

int kickpi_camera_stream(bool on)
{
  uint8_t v;
  int ret;

  if (g_cam_found == NULL || g_cam_i2c == NULL)
    {
      syslog(LOG_ERR, "摄像头: 没有识别到传感器，无法出流\n");
      return -ENODEV;
    }

  if (!on)
    {
      return imx415_write8(g_cam_i2c, IMX415_REG_MODE, IMX415_MODE_STBY);
    }

  /* 先退待机再写表：待机状态下部分寄存器写不进去。 */

  ret = imx415_write8(g_cam_i2c, IMX415_REG_MODE, IMX415_MODE_OPER);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(80);

  /* global 在前、mode 在后 —— 反了会被 global 覆盖。 */

  ret = cam_write_array(g_cam_i2c, g_imx415_global,
                        sizeof(g_imx415_global) /
                        sizeof(g_imx415_global[0]), "global");
  if (ret < 0)
    {
      return ret;
    }

  ret = cam_write_array(g_cam_i2c, g_imx415_mode_1932x1096,
                        sizeof(g_imx415_mode_1932x1096) /
                        sizeof(g_imx415_mode_1932x1096[0]), "mode");
  if (ret < 0)
    {
      return ret;
    }

  /* ★ 回读核对，不能只看写返回值。
   *
   *   I2C 写返回成功只说明从机应答了地址与数据，不说明寄存器接受了这个
   *   值 —— 传感器在错误的状态下会静默丢弃写入。挑模式表里一条有代表性
   *   的（0x3024 是 VMAX 低字节，这一档是 0x5D）读回来对一下，写没生效
   *   时能当场发现，而不是等到接收端收不到数据再回头猜。
   */

  ret = imx415_read8(g_cam_i2c, 0x3024, &v);
  if (ret < 0)
    {
      return ret;
    }

  if (v != 0x5d)
    {
      syslog(LOG_ERR,
             "ERROR: 摄像头 VMAX 回读 0x%02x，期望 0x5d —— "
             "寄存器没写进去\n", v);
      return -EIO;
    }

  /* MODE=0 才真正开始推数据 */

  ret = imx415_write8(g_cam_i2c, IMX415_REG_MODE, IMX415_MODE_OPER);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "摄像头: 已出流 %dx%d RAW%d %d lane %dMbps/lane，单帧 %d 字节\n",
         IMX415_MODE_WIDTH, IMX415_MODE_HEIGHT, IMX415_MODE_BPP,
         IMX415_MODE_LANES, IMX415_MODE_MBPS, IMX415_FRAME_BYTES);
  return OK;
}

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

/****************************************************************************
 * Name: kickpi_camera_receiver
 *
 * Description:
 *   打开取图链路的接收端：D-PHY RX + CSI-2 host。
 *
 *   ★ 与出流分开下令，不是图省事
 *
 *     两端同时打开的话，接收端在发送端还没稳定时就开始收，会记下一批
 *     启动瞬态造成的错误 —— 那些错误是假的，但和真错误在寄存器里长得
 *     一模一样。分开下令，中间留出时间，读到的错误才有意义。
 *
 *   本板摄像头挂在 csi2_dphy3 -> mipi3_csi2，出处厂商
 *   rk3576-kickpi-k7-cam3.dtsi。
 *
 ****************************************************************************/

#define KICKPI_CAM_DPHY_INDEX  3
#define KICKPI_CAM_CSI_HOST    3

int kickpi_camera_receiver(bool on)
{
  int ret;

  if (!on)
    {
      rk3576_csihost_stop(KICKPI_CAM_CSI_HOST);
      rk3576_csidphy_stop(KICKPI_CAM_DPHY_INDEX);
      return OK;
    }

  /* 先 PHY 后 host：host 放开复位时 PHY 应当已经在跟踪信号了，
   * 顺序反过来 host 会在 PHY 还没锁住时就开始判包。
   */

  ret = rk3576_csidphy_start(KICKPI_CAM_DPHY_INDEX, IMX415_MODE_LANES,
                             IMX415_MODE_MBPS);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_csihost_start(KICKPI_CAM_CSI_HOST, IMX415_MODE_LANES);
}

/****************************************************************************
 * Name: kickpi_camera_status
 ****************************************************************************/

int kickpi_camera_status(void)
{
  return rk3576_csihost_status(KICKPI_CAM_CSI_HOST);
}

/****************************************************************************
 * 取图缓冲
 ****************************************************************************/

/* 非压缩 RAW12：每像素 16 位 */

#define CAM_FRAME_BYTES  (IMX415_MODE_WIDTH * IMX415_MODE_HEIGHT * 2)

/* DMA 缓冲要按缓存行对齐。不对齐时失效缓存会连带影响相邻数据 ——
 * 那种破坏是随机的、事后极难定位。
 */

#define CAM_BUF_ALIGN    64

static uint8_t *g_cam_buf[2];

/* 直方图与百分位。直方图放静态区而不是栈上 —— 1KB 的局部数组对
 * 板级初始化任务的栈来说不算小，而这个函数一次只跑一遍，没必要省。
 */

static uint32_t g_cam_hist[256];

/* 刚写完的是哪一个乒乓缓冲。统计和送屏都要读这一个，读另一个就是在读
 * 正被 DMA 改写的内存。
 */

static int g_cam_ready;
static uint16_t g_cam_p1;
static uint16_t g_cam_p50;
static uint16_t g_cam_p99;

/* 最近一帧的动态范围，显示时用来做线性拉伸 */

static uint16_t g_cam_min;
static uint16_t g_cam_max;

/****************************************************************************
 * Name: kickpi_camera_capture
 *
 * Description:
 *   启动 CIF 把图像写进 DDR，并报告一帧的统计特征。
 *
 *   ★ 为什么报统计而不是直接送屏
 *
 *     "有没有取到真实图像"和"显示对不对"是两个问题。先用统计量回答
 *     第一个：全 0 说明 DMA 没动，全同一个值说明取到的是常量而不是
 *     图像，有合理的动态范围才说明是真数据。这一步过了再谈显示，
 *     否则屏上一片黑时分不清是没取到图还是显示环节错了。
 *
 ****************************************************************************/

int kickpi_camera_capture(void)
{
  uint32_t stat;
  uint64_t sum = 0;   /* 211 万个像素，32 位会溢出 */
  uint16_t mn = 0xffff;
  uint16_t mx = 0;
  uint16_t *p;
  size_t npix = IMX415_MODE_WIDTH * IMX415_MODE_HEIGHT;
  size_t zeros = 0;
  size_t last = 0;
  size_t i;
  int ret;

  if (g_cam_buf[0] == NULL)
    {
      /* 两个都要分配：硬件在 FRM0/FRM1 之间乒乓，只给一个的话
       * 第二帧会写到地址 0，把 DDR 起始处冲掉且不报错。
       */

      g_cam_buf[0] = kmm_memalign(CAM_BUF_ALIGN, CAM_FRAME_BYTES);
      g_cam_buf[1] = kmm_memalign(CAM_BUF_ALIGN, CAM_FRAME_BYTES);

      if (g_cam_buf[0] == NULL || g_cam_buf[1] == NULL)
        {
          syslog(LOG_ERR, "摄像头: 取图缓冲分配失败（每个 %d 字节）\n",
                 CAM_FRAME_BYTES);
          return -ENOMEM;
        }
    }

  /* ★ 两个缓冲都要清。
   *
   *   硬件在 FRM0/FRM1 之间乒乓，先写完哪一个事先并不知道。只清 0 号的
   *   话，一旦这次等到的是 1 号，"零像素占比"这个判据读的就是上一轮
   *   残留的数据 —— 一个用来判断"DMA 有没有写满整帧"的指标，本身却
   *   依赖于内存是干净的。
   */

  memset(g_cam_buf[0], 0, CAM_FRAME_BYTES);
  memset(g_cam_buf[1], 0, CAM_FRAME_BYTES);
  up_clean_dcache((uintptr_t)g_cam_buf[0],
                  (uintptr_t)g_cam_buf[0] + CAM_FRAME_BYTES);
  up_clean_dcache((uintptr_t)g_cam_buf[1],
                  (uintptr_t)g_cam_buf[1] + CAM_FRAME_BYTES);

  ret = rk3576_cif_start(KICKPI_CAM_CSI_HOST,
                         (uintptr_t)g_cam_buf[0], (uintptr_t)g_cam_buf[1],
                         IMX415_MODE_WIDTH, IMX415_MODE_HEIGHT);
  if (ret < 0)
    {
      return ret;
    }

  /* ★ 等帧结束标志，不再盲等固定时长。
   *
   *   原来是 mdelay(300) 之后直接读。问题不在于 300ms 够不够 —— 而在于
   *   读到的可能是写了一半的一帧，撕裂的画面很像图像处理写错了，会把
   *   排查引到完全无关的地方。等标志则是确定的：置位就说明整帧已落盘。
   *
   *   顺带解决了另一件事：硬件在 FRM0/FRM1 之间乒乓，等待函数会告诉
   *   我们刚写完的是**哪一个**，避免去读正在被 DMA 改写的那块。
   *
   *   超时给 500ms，30fps 下一帧 33ms，足够容下启动瞬态。
   */

  ret = rk3576_cif_wait_frame(KICKPI_CAM_CSI_HOST, 0, 500, &stat);
  if (ret < 0)
    {
      rk3576_cif_stop(KICKPI_CAM_CSI_HOST);
      syslog(LOG_ERR,
             "摄像头: 没等到帧结束，INTSTAT=0x%08" PRIx32 "\n", stat);
      return ret;
    }

  g_cam_ready = ret;

  /* ★ 取够了就把 DMA 停掉。
   *
   *   之前一直不停，后果有两个，都不显眼：
   *
   *     一是 cam show 在读缓冲的同时 CIF 还在往里写，读到的是半新半旧
   *       的画面 —— 而这种撕裂很容易被当成"图像处理没写对"。
   *     二是这块缓冲永远处于"随时可能被硬件改写"的状态，任何时候释放
   *       或复用它都是错的。
   *
   *   取一帧的语义就该是取完即停。要连续预览时再单独提供一个流式接口，
   *   而不是让"取一帧"悄悄留下一个一直在跑的 DMA。
   */

  rk3576_cif_stop(KICKPI_CAM_CSI_HOST);

  /* ★ 读之前必须失效缓存。CIF 是绕过 CPU 缓存直接写 DDR 的，
   *   不失效的话读到的是写入前留在缓存里的旧内容（这里就是全 0），
   *   会得出"一个字节都没收到"的错误结论。
   */

  up_invalidate_dcache((uintptr_t)g_cam_buf[0],
                       (uintptr_t)g_cam_buf[0] + CAM_FRAME_BYTES);

  /* ★ 直方图 + "最后一个非零像素"，比 min/max/均值 有用得多。
   *
   *   min/max 只反映**两个**像素，一个亮点加一个暗点就能撑出"动态范围
   *   正常"的假象；均值又被大片零拉低。这次屏幕全黑而 min=0 max=65520
   *   均值=6958，光看这三个数根本分不清是
   *     (a) 场景确实暗、拉伸没做够
   *     (b) DMA 只填了一部分，剩下全是 memset 留下的 0
   *   而这两种情况要改的地方毫无关系。
   *
   *   记 last —— 最后一个非零像素的下标 —— 就把 (b) 直接判掉了：
   *   它除以行宽就是 DMA 实际写到了第几行。
   */

  memset(g_cam_hist, 0, sizeof(g_cam_hist));
  last = 0;

  p = (uint16_t *)g_cam_buf[0];
  for (i = 0; i < npix; i++)
    {
      uint16_t v = p[i];

      sum += v;
      if (v < mn)
        {
          mn = v;
        }

      if (v > mx)
        {
          mx = v;
        }

      if (v == 0)
        {
          zeros++;
        }
      else
        {
          last = i;
        }

      g_cam_hist[v >> 8]++;
    }

  syslog(LOG_INFO,
         "摄像头: 取图 %dx%d INTSTAT=0x%08" PRIx32
         " 像素 min=%u max=%u 均值=%u\n",
         IMX415_MODE_WIDTH, IMX415_MODE_HEIGHT, stat,
         mn, mx, (unsigned)(sum / npix));

  syslog(LOG_INFO,
         "  零像素 %lu/%lu（%lu%%），最后一个非零像素在第 %lu 行/%d\n",
         (unsigned long)zeros, (unsigned long)npix,
         (unsigned long)(zeros * 100 / npix),
         (unsigned long)(last / IMX415_MODE_WIDTH), IMX415_MODE_HEIGHT);

  /* 百分位。用 256 桶的直方图求，够精度，也不用排序两百万个数。 */

  {
    size_t acc2 = 0;
    int    b;

    g_cam_p1 = 0;
    g_cam_p50 = 0;
    g_cam_p99 = 65535;

    for (b = 0; b < 256; b++)
      {
        size_t prev = acc2;

        acc2 += g_cam_hist[b];

        if (prev < npix / 100 && acc2 >= npix / 100)
          {
            g_cam_p1 = (uint16_t)(b << 8);
          }

        if (prev < npix / 2 && acc2 >= npix / 2)
          {
            g_cam_p50 = (uint16_t)(b << 8);
          }

        if (prev < npix - npix / 100 && acc2 >= npix - npix / 100)
          {
            g_cam_p99 = (uint16_t)(b << 8);
          }
      }
  }

  syslog(LOG_INFO, "  百分位 p1=%u p50=%u p99=%u\n",
         g_cam_p1, g_cam_p50, g_cam_p99);

  /* 直方图只打非空的桶，否则 256 行刷屏还看不出重点。 */

  {
    int b;

    for (b = 0; b < 256; b++)
      {
        if (g_cam_hist[b] * 1000 / npix > 0)
          {
            syslog(LOG_INFO, "  桶[%3d] %5u.. 占 %lu%%\n",
                   b, (unsigned)(b << 8),
                   (unsigned long)(g_cam_hist[b] * 100 / npix));
            up_mdelay(2);
          }
      }
  }

  /* ★ 拉伸用 p1~p99，不用 min~max。
   *
   *   min/max 是两个极值像素说了算的：一个坏点归零、一个高光饱和，
   *   span 就撑满了整个量程，"线性拉伸"退化成右移 8 位 —— 正是这次
   *   屏幕全黑的直接原因（均值 6958 映射过去只有 27/255）。
   *   百分位由大多数像素决定，个别极值动不了它。
   */

  g_cam_min = g_cam_p1;
  g_cam_max = g_cam_p99;

  if (mx == 0)
    {
      syslog(LOG_WARNING,
             "  全 0 —— DMA 没有写入，查 CIF 输入时钟与地址配置\n");
    }
  else if (mn == mx)
    {
      syslog(LOG_WARNING,
             "  全部是同一个值 0x%04x —— 取到的是常量不是图像\n", mn);
    }
  else if (zeros * 10 > npix)
    {
      syslog(LOG_WARNING,
             "  零像素超过一成 —— DMA 很可能没写满整帧，看上面的行号\n");
    }
  else
    {
      syslog(LOG_INFO, "  动态范围正常，看起来是真实图像\n");
    }

  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_show
 *
 * Description:
 *   把最近取到的一帧送到 /dev/fb0 显示。
 *
 *   ★ 只做灰度，不做去马赛克
 *
 *     传感器出的是 Bayer 阵列，正确还原颜色需要去马赛克 —— 那是画质
 *     问题。当前要回答的是"这条链路搬回来的是不是真实图像"，灰度足够
 *     回答，而且少一个环节就少一处可能出错的地方。彩色留到链路确认
 *     无误之后。
 *
 *   ★ 按实测动态范围做线性拉伸
 *
 *     默认曝光下 12 位像素只占到 196~299 这一小段，直接取高 8 位得到的
 *     是 12~18，屏上几乎全黑 —— 那会让"取到了图"看起来像"没取到图"。
 *     用上一帧实测的 min/max 拉伸到 0~255，图像才看得出来。这是显示
 *     处理，不改动原始数据。
 *
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_camera_fbinfo
 *
 * Description:
 *   只读出帧缓冲参数并打印，不做任何写入。
 *
 *   ★ 为什么要单独一个命令
 *
 *     上一版把参数打印和像素写入放在同一个函数里，结果崩溃时那条
 *     syslog 一个字都没出来 —— 串口是中断发送的，排队中的内容在异常
 *     里丢掉了。于是"参数对不对"和"写入越不越界"这两件事全都看不到。
 *
 *     拆开之后，这个命令只读、立刻返回，无论如何都能把参数带回来。
 *
 ****************************************************************************/

int kickpi_camera_fbinfo(void)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  int fd;

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_ERR, "摄像头: 打不开 /dev/fb0: %d\n", errno);
      return -errno;
    }

  memset(&vinfo, 0, sizeof(vinfo));
  memset(&pinfo, 0, sizeof(pinfo));

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0)
    {
      syslog(LOG_ERR, "摄像头: VIDEOINFO 失败 %d\n", errno);
      close(fd);
      return -errno;
    }

  syslog(LOG_INFO, "摄像头: vinfo %ux%u fmt=%u\n",
         vinfo.xres, vinfo.yres, vinfo.fmt);

  if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      syslog(LOG_ERR, "摄像头: PLANEINFO 失败 %d\n", errno);
      close(fd);
      return -errno;
    }

  syslog(LOG_INFO,
         "摄像头: pinfo mem=%p len=%zu stride=%u bpp=%u disp=%u\n",
         pinfo.fbmem, (size_t)pinfo.fblen, (unsigned)pinfo.stride,
         pinfo.bpp, pinfo.display);

  close(fd);
  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_fbtest
 *
 * Description:
 *   往帧缓冲填固定图案，不涉及任何摄像头数据。
 *
 *   ★ 它把两件事分开
 *
 *     "屏上不对"可能是显示通路的问题（fb 地址、跨距、缓存刷回、VOP2
 *     扫描的是不是这块内存），也可能是图像处理的问题（缩放、Bayer、
 *     对比拉伸）。盯着一幅暗淡或花乱的图像，这两者分不开。
 *
 *     填一个已知图案就分开了：图案正确显示 -> 通路全对，问题在处理；
 *     图案也不对 -> 处理再改也没用，先修通路。
 *
 * Input Parameters:
 *   pattern - 0 纯白、1 纯黑、2 竖条纹（能同时暴露跨距算错）
 *
 ****************************************************************************/

int kickpi_camera_fbtest(int pattern)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  uint32_t *fb;
  uint32_t rowpix;
  int fd;
  int dx;
  int dy;

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&vinfo, 0, sizeof(vinfo));
  memset(&pinfo, 0, sizeof(pinfo));

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      close(fd);
      return -errno;
    }

  fb     = (uint32_t *)pinfo.fbmem;
  rowpix = pinfo.stride / 4;

  if (fb == NULL || rowpix == 0)
    {
      close(fd);
      return -EINVAL;
    }

  for (dy = 0; dy < (int)vinfo.yres; dy++)
    {
      if (((uint32_t)dy * rowpix + vinfo.xres) * 4 > pinfo.fblen)
        {
          break;
        }

      for (dx = 0; dx < (int)vinfo.xres; dx++)
        {
          uint32_t c;

          switch (pattern)
            {
              case 1:
                c = 0xff000000u;                       /* 黑 */
                break;

              case 2:
                /* 32 像素宽的竖条纹。跨距若算错，条纹会倾斜 ——
                 * 纯色图案看不出跨距问题，条纹能。
                 */

                c = ((dx / 32) & 1) ? 0xffffffffu : 0xff000000u;
                break;

              case 3:
                /* 上半白、下半黑。
                 *
                 * ★ 这个图案不需要细看就能判断，而且能一次分清三种情况：
                 *     上白下黑 -> 行映射正确，问题在别处
                 *     左白右黑 -> 缓冲被按列扫描，行列搞反了
                 *     斜分界   -> 硬件每行的像素数与我写的不一致，
                 *                 分界线的斜率直接给出差值
                 *   竖条纹要判断"倾斜多少度"，这个只要说白色在哪半边。
                 */

                c = (dy < (int)vinfo.yres / 2) ? 0xffffffffu : 0xff000000u;
                break;

              case 4:
                /* 左半白、右半黑 —— 与图案 3 配对，专测**列**映射。
                 *
                 * ★ 为什么不用 32 像素竖条纹来测
                 *
                 *   细图案的判读要靠估计角度，而角度受任何轻微的缩放、
                 *   偏移影响都会变；一条粗分界线只要回答"白色在左还是
                 *   在右、分界是直的还是斜的"，不需要估角度。
                 *   先用粗图案定性，确认无误后再用细图案看精度。
                 */

                c = (dx < (int)vinfo.xres / 2) ? 0xffffffffu : 0xff000000u;
                break;

              case 5:
                /* 只把缓冲的**第一行**（前 xres 个像素）涂白，其余全黑。
                 *
                 * ★ 这个图案能直接量出硬件每行的像素数 R
                 *
                 *   我写进去的是连续 720 个白像素，从缓冲偏移 0 开始。
                 *   屏幕把缓冲当成每行 R 像素来扫：
                 *     R = 720 -> 顶部整行全白
                 *     R > 720 -> 只有顶行左边一段白，白段占屏宽的 720/R
                 *     R < 720 -> 顶行全白，且溢出到第二行左边一段
                 *
                 *   前面的半屏图案分辨不出 R（上白下黑只取决于缓冲是否
                 *   被完整扫描），细条纹又要估角度。这个只要看白段占了
                 *   多长，就能反推 R。
                 */

                c = (dy == 0) ? 0xffffffffu : 0xff000000u;
                break;

              case 6:
                /* 缓冲左上区域的一个 100x100 白方块（行 100~200、列 100~200），
                 * 其余全黑。
                 *
                 * ★ 为什么改用方块而不是条纹
                 *
                 *   条纹类图案只能看出"斜不斜"，要靠估角度，而且同一个
                 *   现象可以由好几种映射产生。方块给的是**位置**：它出现
                 *   在屏幕哪里，直接就是缓冲坐标到屏幕坐标的映射结果。
                 *
                 *     出现在左上、大小不变 -> 映射正确，只差整体偏移
                 *     被拉成一条横条      -> 一行的像素被摊到多行上
                 *     出现在别处/分裂多块 -> 行长与我写的不一致，
                 *                            位置差值直接给出偏差
                 */

                c = (dy >= 100 && dy < 200 && dx >= 100 && dx < 200) ?
                    0xffffffffu : 0xff000000u;
                break;

              default:
                c = 0xffffffffu;                       /* 白 */
                break;
            }

          fb[dy * rowpix + dx] = c;
        }
    }

  /* 与 show 一样，必须刷回 DDR，否则 VOP2 看不到。 */

  up_flush_dcache((uintptr_t)fb, (uintptr_t)fb + pinfo.fblen);
  close(fd);

  syslog(LOG_INFO, "摄像头: 已填图案 %d 到 %ux%u\n",
         pattern, vinfo.xres, vinfo.yres);
  return OK;
}

int kickpi_camera_show(int gamma)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  uint32_t *fb;
  uint16_t *src;
  uint32_t span;
  uint32_t outhist[16];
  int imgh;
  int yoff;
  int fd;
  int dx;
  int dy;

  if (g_cam_buf[g_cam_ready] == NULL)
    {
      syslog(LOG_ERR, "摄像头: 还没有取过图，先跑 cam cap\n");
      return -ENODATA;
    }

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_ERR, "摄像头: 打不开 /dev/fb0: %d\n", errno);
      return -errno;
    }

  /* ★ 这两个结构体必须先清零。
   *
   *   fb_planeinfo_s 的 display 字段是**输入**（要查哪一个显示器），
   *   不是输出。不清零就等于拿栈上的垃圾当显示器编号传进去，驱动照着
   *   索引，取回一个野指针 —— 崩溃发生在 ioctl 内部，调用方连一行日志
   *   都来不及打，看起来像是"一进函数就挂"。
   *
   *   教训是：传给内核的结构体，即使自己只关心输出字段，也要整体清零。
   */

  memset(&vinfo, 0, sizeof(vinfo));
  memset(&pinfo, 0, sizeof(pinfo));

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      close(fd);
      return -errno;
    }

  fb  = (uint32_t *)pinfo.fbmem;
  src = (uint16_t *)g_cam_buf[g_cam_ready];

  /* ★ 先把拿到的参数打出来再用。
   *
   *   上一版直接照着 pinfo 写，板子当场重启 —— 没有任何信息能说明是
   *   指针不对、跨距不对，还是越界。ioctl 返回成功只说明调用没出错，
   *   不保证填回来的值可用。
   */

  syslog(LOG_INFO,
         "摄像头: fb %ux%u bpp=%u fmt=%u mem=%p len=%zu stride=%u\n",
         vinfo.xres, vinfo.yres, pinfo.bpp, vinfo.fmt,
         pinfo.fbmem, (size_t)pinfo.fblen, pinfo.stride);

  if (fb == NULL || pinfo.fblen == 0 || pinfo.stride == 0 ||
      pinfo.bpp != 32)
    {
      syslog(LOG_ERR,
             "摄像头: 帧缓冲参数不可用，放弃送屏（只支持 32bpp）\n");
      close(fd);
      return -EINVAL;
    }

  /* 拉伸的分母。min==max 时（全黑或全白）退回直接取高 8 位，
   * 否则会除以 0。
   */

  span = (g_cam_max > g_cam_min) ? (uint32_t)(g_cam_max - g_cam_min) : 0;

  /* 最近邻缩放。摄像头是横向 1932x1096、屏是竖向 720x1280，
   * 这里按宽度等比缩放，画在屏幕上半部分，不做旋转 —— 旋转是取向
   * 问题，跟"链路通不通"无关，先不引入。
   */

  {
    uint32_t maxpix = (uint32_t)(pinfo.fblen / 4);
    uint32_t rowpix = pinfo.stride / 4;

    memset(outhist, 0, sizeof(outhist));

    /* ★ 把画面竖直居中，而不是贴在最上面。
     *
     *   摄像头是横的 1932x1096、屏是竖的 720x1280，按宽度等比缩放之后
     *   只有 720x408，其余三分之二是黑的。贴在顶端时，"上面一小条有东西、
     *   下面一大片黑"这个样子与"整屏全黑"在昏暗画面下很难分辨；居中之后
     *   上下各留一条黑边，一眼就能看出画面区域在哪，也就能判断是画面暗
     *   还是根本没画上去。
     */

    imgh = (int)IMX415_MODE_HEIGHT * (int)vinfo.xres / (int)IMX415_MODE_WIDTH;
    yoff = ((int)vinfo.yres - imgh) / 2;
    if (yoff < 0)
      {
        yoff = 0;
      }

  for (dy = 0; dy < (int)vinfo.yres; dy++)
    {
      /* ★ 纵向要用纵向比例。
       *
       *   上一版这里误用了 IMX415_MODE_WIDTH/xres —— 拿横向的比例去缩
       *   纵向，图像被拉伸成原来的近三倍高，绝大部分行落到源图之外，
       *   只有顶上一小条有内容。等比缩放要用同一个比例因子，这里按
       *   宽度定标，高度随之。
       */

      int sy = (dy - yoff) * (int)IMX415_MODE_WIDTH / (int)vinfo.xres;

      if (dy < yoff)
        {
          sy = IMX415_MODE_HEIGHT;      /* 上黑边，走下面的越界分支 */
        }

      if ((uint32_t)dy * rowpix + vinfo.xres > maxpix)
        {
          syslog(LOG_WARNING,
                 "摄像头: 第 %d 行会越过帧缓冲末尾，提前停止\n", dy);
          break;
        }

      for (dx = 0; dx < (int)vinfo.xres; dx++)
        {
          int sx = dx * (int)IMX415_MODE_WIDTH / (int)vinfo.xres;
          uint32_t g;
          uint32_t acc;

          /* 源图之外画黑，不要读越界 */

          if (sy + 1 >= IMX415_MODE_HEIGHT || sx + 1 >= IMX415_MODE_WIDTH)
            {
              fb[dy * rowpix + dx] = 0xff000000u;
              continue;
            }

          /* ★ 按 2x2 Bayer 单元取平均，而不是取单个像素。
           *
           *   传感器出的是 Bayer 阵列：相邻像素分别盖着 R/G/B 滤镜，
           *   同样光照下响应差很多。直接当灰度显示，再叠加下面的对比
           *   拉伸，棋盘格会被放大成满屏噪点 —— 看起来像"花屏"，很容易
           *   被误判成链路出了问题，其实数据是好的。
           *
           *   一个 2x2 单元恰好含 R、G、G、B 各一，取平均就消掉了滤镜
           *   差异，得到接近亮度的值。这不是去马赛克（分辨率减半、也不
           *   还原颜色），但足以让画面可辨。
           */

          acc = (uint32_t)src[(sy)     * IMX415_MODE_WIDTH + sx]     +
                (uint32_t)src[(sy)     * IMX415_MODE_WIDTH + sx + 1] +
                (uint32_t)src[(sy + 1) * IMX415_MODE_WIDTH + sx]     +
                (uint32_t)src[(sy + 1) * IMX415_MODE_WIDTH + sx + 1];
          acc >>= 2;

          if (span != 0)
            {
              g = (acc > g_cam_min) ?
                  ((acc - g_cam_min) * 255u) / span : 0;
            }
          else
            {
              g = acc >> 8;
            }

          if (g > 255)
            {
              g = 255;
            }

          /* ★ 伽马。原始线性数据直接送显示器本来就是错的。
           *
           *   显示器期望的是伽马编码的信号（约 2.2 次方），而传感器出的
           *   是线性光强。把线性值当成显示值送过去，中间调会被压得极暗：
           *   这次 p50 落在拉伸后的 44/255，屏上就是接近黑 —— 而数据其实
           *   是好的。这一步不是"美化"，是缺了会得出错误结论的一步。
           *
           *   用 sqrt 近似 gamma 0.5：sqrt(g/255)*255 = sqrt(g*255)。
           *   44 -> 106，中间调回到能看见的位置。整数开方够用，不值得
           *   为此引入浮点。
           */

          if (gamma)
            {
              uint32_t v = g * 255u;
              uint32_t r = 0;
              uint32_t bit = 1u << 16;

              while (bit > v)
                {
                  bit >>= 2;
                }

              while (bit != 0)
                {
                  if (v >= r + bit)
                    {
                      v -= r + bit;
                      r = (r >> 1) + bit;
                    }
                  else
                    {
                      r >>= 1;
                    }

                  bit >>= 2;
                }

              g = r;
            }

          outhist[g >> 4]++;

          fb[dy * rowpix + dx] =
            0xff000000u | (g << 16) | (g << 8) | g;
        }
    }
  }

  /* ★ 必须把缓存刷回 DDR。
   *
   *   CPU 写的是缓存，VOP2 是直接从 DDR 扫描的 —— 两者不经过同一条
   *   路径。不刷的话像素停在缓存里，屏上看到的还是初始化时那片 0，
   *   现象是"日志说送屏成功，屏幕却全黑"，而且完全不报错。
   *
   *   取图那侧我做了 invalidate（读 DMA 写入的数据前先失效），这侧是
   *   反方向：写完要 clean。两个方向都要管，漏一个就是单向失效。
   *
   *   不需要 FBIO_UPDATE —— VOP2 持续扫描这块内存，数据到了 DDR 就会
   *   被下一帧扫出去。那个 ioctl 是给需要显式刷新的面板用的。
   */

  up_flush_dcache((uintptr_t)fb, (uintptr_t)fb + pinfo.fblen);

  close(fd);

  syslog(LOG_INFO,
         "摄像头: 已送屏 %ux%u 画面区 %dx%d @y=%d（按 %u~%u 拉伸，伽马%s）\n",
         vinfo.xres, vinfo.yres, vinfo.xres, imgh, yoff,
         g_cam_min, g_cam_max, gamma ? "开" : "关");

  /* ★ 统计**写出去的**灰度，而不只是读进来的原始值。
   *
   *   屏幕全黑时，"写进去的像素本来就接近黑"和"像素没问题但显示不出来"
   *   是两件事，改法毫不相干。之前只统计了输入侧（传感器读数），这两种
   *   情况给出的输入统计完全一样，分不开。这里统计的是真正落进帧缓冲的
   *   那个 8 位灰度 —— 它若分布正常而屏幕仍黑，嫌疑就整个转到显示侧。
   */

  {
    int b;

    for (b = 0; b < 16; b++)
      {
        if (outhist[b] != 0)
          {
            syslog(LOG_INFO, "  输出灰度[%3d..%3d] %lu%%\n",
                   b * 16, b * 16 + 15,
                   (unsigned long)((uint64_t)outhist[b] * 100 /
                                   ((uint64_t)vinfo.xres * vinfo.yres)));
            up_mdelay(2);
          }
      }
  }

  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_ubtest
 *
 * Description:
 *   恢复 U-Boot 的窗口配置，然后直接往它的帧缓冲写图案。
 *
 *   ★ 这一步要回答的问题
 *
 *     我们自己配的窗口，VOP2 取出来的画面是错乱的：纯色填充正常，
 *     按行变化的图案正常，按列变化的图案却变成斜纹，100x100 的方块
 *     变成断续的横带。这几个现象在任何单一的线性寻址模型下都无法同时
 *     成立，说明问题不在"跨距算错"这一层。
 *
 *     U-Boot 那组参数是**唯一被实测证明可用的**（它把 logo 正常显示了
 *     出来）。恢复它、再往它的缓冲里写：
 *       画面正确 -> 显示通路没问题，问题出在我们改的某个寄存器上，
 *                   而且立刻就有了一条能用的出图通路
 *       仍然错乱 -> 问题在写入侧或缓存，与 VOP2 配置无关
 *
 *     无论哪种结果，嫌疑范围都被砍掉一半，这是当前信息量最大的一步。
 *
 ****************************************************************************/

int kickpi_camera_ubtest(int pattern)
{
  uintptr_t fbaddr;
  uint32_t  w;
  uint32_t  h;
  uint32_t  stride;
  uint32_t *fb;
  uint32_t  rowpix;
  int ret;
  int x;
  int y;

  ret = rk3576_vop2_restore_uboot(&fbaddr, &w, &h, &stride);
  if (ret < 0)
    {
      syslog(LOG_ERR, "摄像头: 没有 U-Boot 窗口备份: %d\n", ret);
      return ret;
    }

  fb     = (uint32_t *)fbaddr;
  rowpix = stride / 4;

  for (y = 0; y < (int)h; y++)
    {
      for (x = 0; x < (int)w; x++)
        {
          uint32_t c;

          switch (pattern)
            {
              case 1:
                c = 0xff000000u;                        /* 全黑 */
                break;

              case 2:
                /* 竖条纹 —— 检验列方向映射 */

                c = ((x / 32) & 1) ? 0xffffffffu : 0xff000000u;
                break;

              default:
                c = 0xffffffffu;                        /* 全白 */
                break;
            }

          fb[y * rowpix + x] = c;
        }
    }

  /* VOP2 直接从 DDR 扫描，CPU 写的是缓存，必须刷回。 */

  up_flush_dcache(fbaddr, fbaddr + (uintptr_t)stride * h);

  syslog(LOG_INFO,
         "摄像头: 已往 U-Boot 缓冲写图案 %d，%" PRIu32 "x%" PRIu32
         " 跨距 %" PRIu32 " @0x%08lx\n",
         pattern, w, h, stride, (unsigned long)fbaddr);
  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_morph
 *
 * Description:
 *   从 U-Boot 那组已知可用的窗口参数出发，一次只改一个变量，然后画
 *   竖条纹。第一个出斜纹的步骤就指名了元凶。
 *
 *   ★ 为什么要做成"一次烧写、七次实验"
 *
 *     `cam ub 2` 已经证明：用 U-Boot 的地址 + 654x270 + 跨距 2616，
 *     竖条纹是正的。也就是说显示通路、写入侧、缓存刷回全都没问题，
 *     错的一定是 fb_setup 相对 U-Boot 改动的那几项之一。
 *
 *     但 fb_setup 一次改了五样：地址、宽、高、跨距、显示起点。这五样
 *     里任何一样错了，屏上都是"斜纹 + 断续横带"，看现象分不出是哪一个。
 *     唯一的办法是每次只挪一个。
 *
 *     把它做成带参数的 nsh 子命令，而不是改代码重烧 —— 重烧一次几分钟，
 *     敲一条命令一秒钟，而这里要试的组合有七个。
 *
 *   ★ 步骤是有序的，不要跳着做
 *
 *     后面的步骤默认前面的结论成立（比如第 2 步之后才敢在放大的几何
 *     上继续加变量）。哪一步先出斜纹，就停在那一步。
 *
 *       0  U-Boot 地址 654x270 U-Boot 起点     基线，应当是正的竖条纹
 *       1  我们的地址 654x270 U-Boot 起点     只换内存
 *       2  U-Boot 地址 720x270 U-Boot 起点     只换宽（跨距随之 2880）
 *       3  U-Boot 地址 654x1280 U-Boot 起点    只换高
 *       4  U-Boot 地址 720x1280 U-Boot 起点    整个几何
 *       5  U-Boot 地址 720x1280 起点 0          再加显示起点
 *       6  我们的地址 720x1280 起点 0          等价于当前的 fb_setup
 *       7  我们的地址 720x1280 起点 hact/vact  ★ 故意复现故障
 *
 *   ★ 第 7 步是反过来用的：它把已经修掉的那个错误值写回去。
 *
 *     根因是从厂商 SDK 里读出来的（U-Boot 的 vop2_set_smart_win() 与
 *     内核的 vop2_win_atomic_update() 都只把目标矩形坐标写进 DSP_ST，
 *     不加消隐段），但"读代码得出的结论"和"在这块板上成立"是两件事。
 *     6 出正的竖条纹、7 出斜纹，两次对照才算证实 —— 否则只能说
 *     "改完好了"，说不清是不是别的什么被一起改掉了。
 *
 *   ★ 放大后的窗口一律写进 U-Boot 那块帧缓冲（0xfdf00000 起 4MB，
 *     已在 MMU 表里映射）。720x1280x4 = 3.69MB，装得下 —— 这样"换内存"
 *     和"换几何"才是两个独立的变量，不会一起动。
 *
 ****************************************************************************/

int kickpi_camera_morph(int step)
{
  /* 与上表一一对应。ourbuf 为 true 表示用 /dev/fb0 的缓冲，
   * 否则用 U-Boot 的；宽高为 0 表示沿用 U-Boot 的。
   */

  static const struct
  {
    bool     ourbuf;
    uint32_t width;
    uint32_t height;
    int      origin;
    const char *what;
  }
  steps[] =
  {
    { false,   0,    0, 0, "基线：U-Boot 原样"          },
    { true,    0,    0, 0, "只换帧缓冲地址"             },
    { false, 720,    0, 0, "只把宽改成 720"             },
    { false,   0, 1280, 0, "只把高改成 1280"            },
    { false, 720, 1280, 0, "几何改成全屏"               },
    { false, 720, 1280, 1, "再把起点移到有效区原点（0）" },
    { true,  720, 1280, 1, "全部改完，等价于 fb_setup"  },
    { true,  720, 1280, 2, "★ 用旧的错误起点复现斜纹"    },
  };

  struct fb_planeinfo_s pinfo;
  uintptr_t  fbaddr;
  uintptr_t  ourfb = 0;
  size_t     ourlen = 0;
  uint32_t   w;
  uint32_t   h;
  uint32_t   stride;
  uint32_t  *fb;
  uint32_t   rowpix;
  size_t     limit;
  int ret;
  int x;
  int y;

  if (step < 0 || step >= (int)(sizeof(steps) / sizeof(steps[0])))
    {
      syslog(LOG_ERR, "摄像头: 步骤号要在 0~%d 之间\n",
             (int)(sizeof(steps) / sizeof(steps[0])) - 1);
      return -EINVAL;
    }

  /* 只在需要时才去问 /dev/fb0 要地址 —— 不需要的步骤不该因为帧缓冲
   * 没初始化而失败。
   */

  if (steps[step].ourbuf)
    {
      int fd = open("/dev/fb0", O_RDWR);

      if (fd < 0)
        {
          syslog(LOG_ERR, "摄像头: 打不开 /dev/fb0: %d\n", errno);
          return -errno;
        }

      memset(&pinfo, 0, sizeof(pinfo));

      if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
        {
          close(fd);
          return -errno;
        }

      close(fd);

      ourfb  = (uintptr_t)pinfo.fbmem;
      ourlen = pinfo.fblen;

      if (ourfb == 0 || ourlen == 0)
        {
          syslog(LOG_ERR, "摄像头: /dev/fb0 没给出可用的缓冲\n");
          return -EINVAL;
        }
    }

  syslog(LOG_INFO, "摄像头: 第 %d 步 —— %s\n", step, steps[step].what);

  ret = rk3576_vop2_try_window(ourfb, steps[step].width, steps[step].height,
                               steps[step].origin,
                               &fbaddr, &w, &h, &stride);
  if (ret < 0)
    {
      syslog(LOG_ERR, "摄像头: 配窗口失败 %d（要先启动过帧缓冲）\n", ret);
      return ret;
    }

  /* ★ 越界保护要按实际那块内存的大小来算。
   *
   *   U-Boot 那块是 MMU 表里映射的 4MB；我们自己那块是 fblen。写过头
   *   在前者是踩别的外设、在后者是踩堆 —— 两种都是难查的随机故障，
   *   而这里只是个调试命令，不值得冒这个险。
   */

  limit = steps[step].ourbuf ? ourlen : (size_t)0x400000;

  fb     = (uint32_t *)fbaddr;
  rowpix = stride / 4;

  for (y = 0; y < (int)h; y++)
    {
      if (((size_t)y * rowpix + w) * 4 > limit)
        {
          syslog(LOG_WARNING, "摄像头: 第 %d 行越界，提前停止\n", y);
          break;
        }

      for (x = 0; x < (int)w; x++)
        {
          /* 32 像素宽的竖条纹。硬件每行取的像素数若与 stride 不符，
           * 条纹每行都会横移固定的距离，叠起来就是一条斜纹 —— 斜率
           * 直接给出差了多少像素，比"图像不对"这种描述可用得多。
           */

          fb[(size_t)y * rowpix + x] =
            ((x / 32) & 1) ? 0xffffffffu : 0xff000000u;
        }
    }

  up_flush_dcache(fbaddr, fbaddr + (uintptr_t)stride * h);

  syslog(LOG_INFO,
         "摄像头: 已写竖条纹 %" PRIu32 "x%" PRIu32 " 跨距 %" PRIu32
         " @0x%08lx\n", w, h, stride, (unsigned long)fbaddr);

  rk3576_vop2_dump_win();
  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_regs
 *
 * Description:
 *   只读打印 ESMART1 与 VP1 的寄存器，不写任何东西。
 *
 ****************************************************************************/

int kickpi_camera_regs(void)
{
  rk3576_vop2_dump_win();
  return OK;
}

/****************************************************************************
 * Name: kickpi_camera_gain
 *
 * Description:
 *   设置 IMX415 的模拟增益，并可选地改快门。
 *
 *   ★ 为什么要做成运行时可调
 *
 *     取图通路打通之后，直方图显示 68% 的像素挤在一个桶里、12 位值只有
 *     196~478 —— 亮度不够。但"不够"到底是曝光、增益、镜头，还是现场光线
 *     太暗，光看一组数字判断不了，而每换一个值就重编一次固件的话，
 *     一次扫描要花掉一个下午。
 *
 *     做成命令就能连续扫：增益从 0 一路加上去，看直方图往哪边走、什么
 *     时候开始饱和。这比任何一次性的"填个经验值"都更能说明问题。
 *
 *   ★ 增益寄存器不在模式表里
 *
 *     板级那份 IMX415 模式表里没有 0x3090/0x3091，所以模拟增益一直停在
 *     复位值 0 dB。这类"配置表里缺了一项、于是硬件用默认值"的问题不会
 *     报任何错，只会让画面暗得莫名其妙。
 *
 * Input Parameters:
 *   gain - GAIN_PCG_0 的原始值，0~240，每级 0.3dB（0 = 0dB，240 = 72dB）
 *   shr  - SHR0 快门值；传 -1 表示不动。曝光行数 = VMAX - SHR0，
 *          所以这个值**越小曝光越长**，最小 8。
 *
 ****************************************************************************/

int kickpi_camera_gain(int gain, int shr)
{
  uint8_t v;
  int ret;

  if (g_cam_i2c == NULL)
    {
      syslog(LOG_ERR, "摄像头: I2C 未初始化\n");
      return -ENODEV;
    }

  if (gain < 0 || gain > 240)
    {
      syslog(LOG_ERR, "摄像头: 增益要在 0~240 之间（每级 0.3dB）\n");
      return -EINVAL;
    }

  ret = imx415_write8(g_cam_i2c, 0x3090, (uint8_t)(gain & 0xff));
  if (ret >= 0)
    {
      ret = imx415_write8(g_cam_i2c, 0x3091, (uint8_t)((gain >> 8) & 0x07));
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "摄像头: 写增益失败 %d\n", ret);
      return ret;
    }

  if (shr >= 8)
    {
      ret = imx415_write8(g_cam_i2c, 0x3050, (uint8_t)(shr & 0xff));
      if (ret >= 0)
        {
          ret = imx415_write8(g_cam_i2c, 0x3051,
                              (uint8_t)((shr >> 8) & 0xff));
        }

      if (ret < 0)
        {
          syslog(LOG_ERR, "摄像头: 写快门失败 %d\n", ret);
          return ret;
        }
    }

  /* 回读核对 —— 与模式表那里同样的理由：写返回成功不代表寄存器接受了。
   * 传感器在出流状态下对某些寄存器是有写保护的，静默丢弃。
   */

  ret = imx415_read8(g_cam_i2c, 0x3090, &v);
  if (ret < 0)
    {
      return ret;
    }

  if (v != (uint8_t)(gain & 0xff))
    {
      syslog(LOG_ERR,
             "摄像头: 增益回读 0x%02x，期望 0x%02x —— 没写进去\n",
             v, (uint8_t)(gain & 0xff));
      return -EIO;
    }

  syslog(LOG_INFO, "摄像头: 增益 %d（%d.%d dB）%s\n",
         gain, gain * 3 / 10, (gain * 3) % 10,
         shr >= 8 ? "，快门已改" : "");
  return OK;
}
