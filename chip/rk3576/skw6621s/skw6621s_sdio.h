/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S SDIO probe interface.
 ****************************************************************************/

#ifndef __CONTEST_SKW6621S_SDIO_H
#define __CONTEST_SKW6621S_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sdio.h>

#include <stdint.h>

enum skw6621s_protocol_e
{
  SKW6621S_SDIO_V1 = 1,
  SKW6621S_SDIO_V2 = 2
};

struct skw6621s_probe_s
{
  uint8_t identity[16];
  uint8_t cccr_revision;
  uint16_t vendor;
  uint16_t device;
  uint16_t blocksize;
  enum skw6621s_protocol_e protocol;
};

int skw6621s_sdio_probe(FAR struct sdio_dev_s *sdio,
                        FAR struct skw6621s_probe_s *result);

#endif /* __CONTEST_SKW6621S_SDIO_H */
