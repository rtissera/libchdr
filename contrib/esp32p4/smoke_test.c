/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * Compile+link smoke test for chd_esp_vfs.{h,c} - not a functional test
 * (there is no ESP32-P4 hardware or ESP-IDF VFS in CI to open a real file
 * against), only proof that this integration file builds and fully links
 * against libchdr under the RV32IMAFC/ilp32f ABI that ESP32-P4's RISC-V
 * cores share (see cmake/toolchain-rv32imafc.cmake). chd_esp_vfs_open() is
 * called on a path that can't exist, so it always fails cleanly - the point
 * is forcing the linker to resolve every symbol chd_esp_vfs.c and libchdr
 * reference, the same non-no-op probe rationale as
 * contrib/tangcore-bl616's chd_link_probe(). See
 * .github/workflows/esp32p4-build.yml.
 */

#include "chd/chd_esp_vfs.h"

int main(void)
{
	chd_file *chd = NULL;
	chd_error err = chd_esp_vfs_open("/does/not/exist.chd", &chd);
	return (err == CHDERR_NONE) ? 1 : 0;
}
