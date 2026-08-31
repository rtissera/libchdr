/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_esp_vfs.h
 *
 * Bridges libchdr's core_file_callbacks onto ESP-IDF's VFS layer (plain
 * stdio FILE*), for ESP32-P4 firmware that accesses CHD files through any
 * VFS-mounted volume (SD/MMC, SPI flash FATFS, SPIFFS/LittleFS...) - ESP-IDF
 * transparently maps fopen()/fread()/fseek()/fclose() onto whichever
 * filesystem is mounted at the path's prefix, so a plain stdio backend
 * covers all of them without a filesystem-specific implementation (unlike
 * contrib/tangcore-bl616's chd_fatfs.c, which talks to FatFS directly).
 * Original code, not derived from any third-party source.
 */

#pragma once

#include <stdio.h>

#include "libchdr/chd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bridges libchdr's core_file_callbacks onto a stdio FILE*. One FILE* per
 * open CHD - the FILE* is owned by chd_esp_vfs_open()/closed by libchdr's
 * chd_close() (which calls back into chd_esp_vfs_fclose()). */

extern const core_file_callbacks chd_esp_vfs_callbacks;

/* Opens path via fopen("rb"), then hands it to libchdr as the argp for
 * chd_esp_vfs_callbacks. On any failure, the FILE* is closed if it was
 * opened and *chd is left untouched. */
chd_error chd_esp_vfs_open(const char *path, chd_file **chd);

#ifdef __cplusplus
}
#endif
