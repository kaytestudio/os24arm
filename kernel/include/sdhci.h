#ifndef SDHCI_H
#define SDHCI_H

/* SD card over an SDHCI-compatible host controller (kernel/src/sdhci.c):
 *   - BeagleBone Black: TI AM335x MMCHS0 (SD_HOST_AM335X_MMCHS)
 *   - BeagleV-Ahead:    T-Head TH1520 SDIO0, Synopsys DWC MSHC
 * SD_BASE / SD_BASE_CLK_HZ come from the platform's PLATFORM_DEFINES in
 * the top-level CMakeLists.txt. */

#include "types.h"
#include "blkdev.h"

/**
 * @brief Bring up the host controller, identify the card and register
 * it as block device "mmc0" (whole card; partitions are probed
 * separately with mbr_probe()/gpt_probe()).
 *
 * @return the registered device, or NULL if no card answered
 */
blkdev_t *sdhci_sd_init(void);

#endif /* SDHCI_H */
