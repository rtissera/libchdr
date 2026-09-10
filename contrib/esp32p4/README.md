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

## Options worth setting, and what they measured

`chd_esp_vfs_open()` already calls `chd_set_cache_budget()` with 32KB.
Compressed hunks are a few KB and laid out sequentially, so one larger read
serves many and the per-read fixed cost is paid far less often: **1.11x on an
ESP32-S3** over SPI, **1.05-1.12x on an RP2350** with 32KB the knee there. The
P4's own number has not been taken. Set `CHD_ESP_VFS_CACHE_BUDGET` to 0 to turn
it off.

**micro-flac is the default backend here**, unlike libchdr itself. The
component fetches it at a pinned commit when `CHDR_MICROFLAC_SOURCE_DIR` is not
given - point that at your own checkout, or at a managed component under
`managed_components/esphome__micro-flac`, if you would rather not fetch.
`CHDR_FLAC_BACKEND=drflac` goes back to dr_flac.

Output is byte-identical either way. On an ESP32-S3 with I/O excluded:
**1.198x** on a CD-FLAC hunk, **1.233x** on raw FLAC. Across eleven real discs:
**1.072x** overall, 1.21x where the image is FLAC-heavy, **0.988x** on one
profile where FLAC barely appears. Peak heap is lower than dr_flac's on most
images.

libchdr's own default stays dr_flac: a desktop consumer vendoring `src/` must
not have to fetch anything, and micro-flac is C++ and Apache-2.0. An MCU
integrator is already cloning an SDK and a toolchain, so one more pinned
checkout costs nothing - hence the different default on this side.

An earlier revision of this file quoted 1.41x for the P4. That predates the
STREAMINFO block-size fix, which removed an oversized decoded-sample buffer
from dr_flac and took most of micro-flac's lead with it. **The P4 has not been
re-measured since**; treat the S3 numbers above as the estimate until it is.

Two things not to try, both with their numbers in
`../../docs/perf-esp32p4-findings.md`: **`-Os`** is 1.12x *slower* than the
`PERF` (-O2) default on an S3, and **`Z7_LZMA_PROB32`** costs 15,980 bytes per
LZMA instance for a speedup the LZMA SDK only claims for "some CPUs" and that
was never measured on any target.

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