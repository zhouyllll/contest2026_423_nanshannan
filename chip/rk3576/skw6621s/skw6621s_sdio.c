/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S function-1 probe for the RK3576 SDIO host.
 *
 * The standard SDIO card-information tuples identify the device. Private
 * register behavior is inferred from the supplied Rockchip SKW6621S SDK;
 * this implementation is independently structured for NuttX.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/sdio.h>

#include "skw6621s_sdio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SKW_FUNCTION                 1
#define SKW_VENDOR_ID                0x1ffe
#define SKW_DEVICE_ID                0x6621
#define SKW_CCCR_REVISION            0x000
#define SKW_FBR1_CIS_POINTER         0x109
#define SKW_CIS_END                  0x1ffff
#define SKW_CIS_MAX_WALK             1024
#define SKW_CIS_MANFID               0x20
#define SKW_ADDR_LATCH               0x15c
#define SKW_DIRECT_WINDOW            0x00f
#define SKW_CHIP_ID_ADDRESS          0x40000000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int skw_read_f0(FAR struct sdio_dev_s *sdio, uint32_t address,
                       FAR uint8_t *value)
{
  return sdio_io_rw_direct(sdio, false, 0, address, 0, value);
}

static int skw_write_f0(FAR struct sdio_dev_s *sdio, uint32_t address,
                        uint8_t value)
{
  return sdio_io_rw_direct(sdio, true, 0, address, value, NULL);
}

static int skw_read_cis_pointer(FAR struct sdio_dev_s *sdio,
                                FAR uint32_t *pointer)
{
  uint8_t byte;
  uint32_t address = 0;
  int i;
  int ret;

  for (i = 0; i < 3; i++)
    {
      ret = skw_read_f0(sdio, SKW_FBR1_CIS_POINTER + i, &byte);
      if (ret < 0)
        {
          return ret;
        }

      address |= (uint32_t)byte << (8 * i);
    }

  if (address == 0 || address > SKW_CIS_END)
    {
      return -EINVAL;
    }

  *pointer = address;
  return OK;
}

static int skw_read_manfid(FAR struct sdio_dev_s *sdio,
                           FAR uint16_t *vendor, FAR uint16_t *device)
{
  uint32_t pointer;
  uint32_t walked = 0;
  uint8_t code;
  uint8_t length;
  uint8_t id[4];
  int ret;
  int i;

  ret = skw_read_cis_pointer(sdio, &pointer);
  if (ret < 0)
    {
      return ret;
    }

  while (walked < SKW_CIS_MAX_WALK && pointer <= SKW_CIS_END)
    {
      ret = skw_read_f0(sdio, pointer, &code);
      if (ret < 0)
        {
          return ret;
        }

      pointer++;
      walked++;

      if (code == 0xff)
        {
          break;
        }

      if (code == 0x00)
        {
          continue;
        }

      if (pointer > SKW_CIS_END)
        {
          return -EOVERFLOW;
        }

      ret = skw_read_f0(sdio, pointer, &length);
      if (ret < 0)
        {
          return ret;
        }

      pointer++;
      walked++;

      if (pointer > SKW_CIS_END ||
          length > SKW_CIS_END - pointer + 1 ||
          length > SKW_CIS_MAX_WALK - walked)
        {
          return -EOVERFLOW;
        }

      if (code == SKW_CIS_MANFID && length >= sizeof(id))
        {
          for (i = 0; i < 4; i++)
            {
              ret = skw_read_f0(sdio, pointer + i, &id[i]);
              if (ret < 0)
                {
                  return ret;
                }
            }

          *vendor = (uint16_t)id[0] | (uint16_t)id[1] << 8;
          *device = (uint16_t)id[2] | (uint16_t)id[3] << 8;
          return OK;
        }

      pointer += length;
      walked += length;
    }

  return -ENODEV;
}

static int skw_read_identity(FAR struct sdio_dev_s *sdio,
                             FAR uint8_t identity[16])
{
  int ret;
  int i;

  /* The four CMD52 writes form one address-latch operation. Probe has only
   * one caller; the runtime driver must protect this sequence with its bus
   * mutex before concurrent TX/RX is enabled.
   */

  for (i = 0; i < 4; i++)
    {
      ret = skw_write_f0(sdio, SKW_ADDR_LATCH + i,
                         (SKW_CHIP_ID_ADDRESS >> (8 * i)) & 0xff);
      if (ret < 0)
        {
          return ret;
        }
    }

  return sdio_io_rw_extended(sdio, false, SKW_FUNCTION,
                             SKW_DIRECT_WINDOW, false, identity, 16, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int skw6621s_sdio_probe(FAR struct sdio_dev_s *sdio,
                        FAR struct skw6621s_probe_s *result)
{
  struct skw6621s_probe_s probe;
  int ret;

  if (sdio == NULL || result == NULL)
    {
      return -EINVAL;
    }

  memset(&probe, 0, sizeof(probe));

  ret = sdio_probe(sdio);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW6621S: standard SDIO probe failed: %d\n", ret);
      return ret;
    }

  ret = skw_read_f0(sdio, SKW_CCCR_REVISION, &probe.cccr_revision);
  if (ret < 0 || probe.cccr_revision == 0)
    {
      return ret < 0 ? ret : -ENODEV;
    }

  ret = skw_read_manfid(sdio, &probe.vendor, &probe.device);
  if (ret < 0)
    {
      return ret;
    }

  if (probe.vendor != SKW_VENDOR_ID || probe.device != SKW_DEVICE_ID)
    {
      syslog(LOG_ERR,
             "SKW6621S: unexpected SDIO ID %04x:%04x\n",
             probe.vendor, probe.device);
      return -ENODEV;
    }

  ret = sdio_enable_function(sdio, SKW_FUNCTION);
  if (ret < 0)
    {
      return ret;
    }

  ret = skw_read_identity(sdio, probe.identity);
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(probe.identity, "SV6160LITE", 10) == 0)
    {
      probe.protocol = SKW6621S_SDIO_V2;
      probe.blocksize = 512;
    }
  else if (memcmp(probe.identity, "SV6160", 6) == 0)
    {
      probe.protocol = SKW6621S_SDIO_V1;
      probe.blocksize = 256;
    }
  else
    {
      syslog(LOG_ERR, "SKW6621S: unsupported chip identity\n");
      return -ENODEV;
    }

  ret = sdio_set_blocksize(sdio, SKW_FUNCTION, probe.blocksize);
  if (ret < 0)
    {
      return ret;
    }

  *result = probe;
  syslog(LOG_INFO,
         "SKW6621S: ID %04x:%04x, chip %.16s, SDIO v%d, block %u\n",
         probe.vendor, probe.device, probe.identity, probe.protocol,
         probe.blocksize);
  return OK;
}
