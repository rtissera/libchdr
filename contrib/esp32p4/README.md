# ESP32-P4 integration

`core_file_callbacks` bridge for Espressif's ESP32-P4 (dual-core RISC-V,
RV32IMAFC, up to ~360-400MHz, 768KB HP L2MEM + 32KB LP SRAM), targeting
ESP-IDF's VFS layer.

Unlike `contrib/tangcore-bl616` (a compile+link smoke test against a real,
pinned third-party firmware project), there is no open-source ESP32-P4
firmware that already consumes CHDs to build+link against here. What this
directory provides instead:

- `chd/chd_esp_vfs.{h,c}` - a `core_file_callbacks` implementation backed by
  plain stdio (`fopen`/`fread`/`fseek`/`ftell`/`fclose`). ESP-IDF's VFS layer
  transparently maps stdio calls onto whichever filesystem is mounted at a
  path's prefix (SD/MMC, SPI-flash FATFS, SPIFFS, LittleFS...), so a single
  stdio-backed implementation covers all of them - no filesystem-specific
  code needed, unlike BL616's FatFS-specific `chd_fatfs.c`. 100% original
  code, not derived from any third-party source; BSD-3-Clause, same as the
  rest of libchdr (see the file headers).
- `smoke_test.c` - compile+link smoke test (see
  `.github/workflows/esp32p4-build.yml`): opens a nonexistent path through
  `chd_esp_vfs_open()` so it always fails cleanly, forcing the linker to
  fully resolve `chd_esp_vfs.c` and libchdr against the toolchain's libc -
  same non-no-op-probe rationale as `tangcore-bl616`'s `chd_link_probe()`
  call.

## Using this in an ESP-IDF project

1. Vendor `include/`, `src/`, and `deps/` from this repo as an ESP-IDF
   component (or add libchdr as a git submodule and point a component
   `CMakeLists.txt` at it), building with `CHDR_LOWRAM_TARGET=ON` -
   `IDF_TARGET=esp32p4` builds already select ESP-IDF's own RISC-V toolchain,
   so no toolchain file from this repo is needed for a real ESP-IDF build.
2. Copy `chd/chd_esp_vfs.h` and `chd/chd_esp_vfs.c` into your component.
3. Mount whatever storage holds your CHDs through ESP-IDF's VFS (e.g.
   `esp_vfs_fat_sdmmc_mount()` for SD/MMC), then call
   `chd_esp_vfs_open("/sdcard/game.chd", &chd)`.

## CI

Two workflows:

- `.github/workflows/esp32p4-build.yml` builds `chdr-static` with
  `cmake/toolchain-rv32imafc.cmake` - ESP32-P4's RISC-V cores share BL616's
  RV32IMAFC/ilp32f base ISA/ABI (see that file's header comment) - then
  compiles and links `smoke_test.c` + `chd_esp_vfs.c` against it with the
  generic `riscv64-unknown-elf-gcc`, not Espressif's own `riscv32-esp-elf`
  toolchain or ESP-IDF. That means this job proves the integration code
  builds and links cleanly against libchdr under ESP32-P4's ABI, but *not*
  that it builds under the real ESP-IDF toolchain/build system, and (same
  as `tangcore-bl616`) there is no ESP32-P4 hardware in CI, so it cannot
  prove `chd_open()`/`chd_read()` work at runtime.
- Peak-heap-per-codec is measured by `.github/workflows/rv32-ram-budget.yml`,
  which covers this part too: the RISC-V cores here share BL616's
  RV32IMAFC/ilp32f base ISA/ABI, so the same `tests/rv32` probe under
  `qemu-system-riscv32` produces the same numbers. A separate copy of that
  job existed for a while purely for CI visibility and was removed - it was
  byte-identical work.