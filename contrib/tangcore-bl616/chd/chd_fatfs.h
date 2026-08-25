/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_fatfs.h
 *
 * Bridges libchdr's core_file_callbacks onto FatFS, for firmware targets
 * (e.g. nand2mario/firmware-bl616) that access CHD files through a FatFS
 * volume instead of a hosted libc filesystem. Original code, not derived
 * from any third-party source.
 */

#pragma once

#include "ff.h"
#include "libchdr/chd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bridges libchdr's core_file_callbacks onto FatFS. One FIL per open CHD -
 * caller owns the FIL's storage (e.g. embed in the same struct as the chd_file*)
 * since libchdr only ever sees it through the opaque argp pointer. */

extern const core_file_callbacks chd_fatfs_callbacks;

/* Opens path via f_open(FA_READ) into *fil, then hands it to libchdr as the
 * argp for chd_fatfs_callbacks. On any failure, *fil is closed if it was
 * opened and *chd is left untouched. */
chd_error chd_fatfs_open(const char *path, FIL *fil, chd_file **chd);

#ifdef __cplusplus
}
#endif
