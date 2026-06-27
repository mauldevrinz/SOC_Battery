/******************************************************************************
 *  File        : sd_diskio_spi.h
 *  Author      : ControllersTech
 *  Website     : https://controllerstech.com
 *  Date        : June 26, 2025
 *
 *  Description :
 *    This file is part of a custom STM32/Embedded tutorial series.
 *    For documentation, updates, and more examples, visit the website above.
 *
 *  Note :
 *    This code is written and maintained by ControllersTech.
 *    You are free to use and modify it for learning and development.
 *
 *  --------------------------------------------------------------------------
 *  Project patch (SOC_PROTO):
 *    - SD_disk_write's buff parameter is now `const BYTE *`, matching the
 *      disk_write function pointer type FatFs itself expects in
 *      Diskio_drvTypeDef (diskio.h). Fixes a -Wincompatible-pointer-types
 *      warning when SD_disk_write is assigned into the SD_Driver struct.
 *    - SD_Driver is no longer declared `const`, matching the non-const
 *      `Diskio_drvTypeDef *` parameter FATFS_LinkDriver expects. Fixes a
 *      -Wdiscarded-qualifiers warning at the call site in sd_functions.c.
 ******************************************************************************/

#ifndef __SD_DISKIO_SPI_H__
#define __SD_DISKIO_SPI_H__

#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

extern Diskio_drvTypeDef SD_Driver;
DSTATUS SD_disk_status(BYTE drv);
DSTATUS SD_disk_initialize(BYTE drv);
DRESULT SD_disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
DRESULT SD_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

#ifdef __cplusplus
}
#endif

#endif /* __SD_DISKIO_SPI_H__ */
