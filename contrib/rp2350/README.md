# RP2350 (Raspberry Pi Pico 2) integration

`cmake/toolchain-cortex-m33.cmake` cross-compiles libchdr for the RP2350's
Cortex-M33 core mode (dual Cortex-M33 @150MHz, 520KB SRAM - the default core
mode used by the Pico SDK and every current RP2350 SDK/toolchain; RP2350 can
alternatively run its two Hazard3 RISC-V cores instead, but that mode has no
released SDK/toolchain to target yet).

There is no dedicated `chd_*` bridge file in this directory: RP2350 boards
overwhelmingly access storage through FatFS (e.g.
[carlk3/no-OS-FatFS-SD-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico)
for SD/MMC over SPI), and `../tangcore-bl616/chd/chd_fatfs.{h,c}` is already
a generic FatFS `core_file_callbacks` bridge - it only calls `ff.h`'s
`f_open`/`f_read`/`f_lseek`/`f_close`/`f_size`, nothing BL616-specific - so
it's directly reusable here rather than duplicated. Copy those two files
into your Pico SDK project alongside libchdr.

## Using this with the Pico SDK

1. Vendor `include/`, `src/`, and `deps/` from this repo into your Pico SDK
   project (or add libchdr as a git submodule), building with
   `CHDR_LOWRAM_TARGET=ON`. The Pico SDK's own `pico_sdk_import.cmake`
   toolchain already targets Cortex-M33 hard-float for RP2350 boards
   (`PICO_PLATFORM=rp2350`), so `cmake/toolchain-cortex-m33.cmake` in this
   repo is for CI/local RAM-budget measurement only, not needed for a real
   Pico SDK build.
2. Copy `../tangcore-bl616/chd/chd_fatfs.h` and `chd_fatfs.c` into your
   project, alongside your FatFS integration (e.g.
   no-OS-FatFS-SD-SPI-RPi-Pico's `ff.h`).
3. Mount your SD card / flash filesystem, then call
   `chd_fatfs_open("game.chd", &fil, &chd)`.

## CI

`.github/workflows/rp2350-ram-budget.yml` cross-compiles `chdr-static` for
Cortex-M33 hard-float and runs a peak-heap-per-codec probe under
`qemu-system-arm`'s `mps2-an505` machine (an Armv8-M Mainline
Cortex-M33 reference board, not the real RP2350 SoC - only the core ISA/ABI
and heap numbers matter here). Measured baseline (2026-08-31,
`CHDR_LOWRAM_TARGET=ON`):

| codec   | peak heap | budget  |
|---------|-----------|---------|
| hd_zlib | 52,074 B  | 100,000 |
| hd_zstd | 105,817 B | 200,000 |
| hd_lzma | 30,642 B  | 80,000  |
| hd_huff | 21,839 B  | 60,000  |
| cd_cdzl | 136,341 B | 250,000 |
| cd_cdzs | 243,768 B | 400,000 |
| cd_cdlz | 135,427 B | 250,000 |

All well inside the RP2350's 520KB SRAM even before accounting for
`LOWRAM_TARGET`'s per-hunk-map savings on real (larger, more-hunks) discs -
see `tests/rp2350/check_budget.py` for how these are enforced and
`.github/workflows/rv32-ram-budget.yml`'s comment for why `LOWRAM_TARGET`
matters most on real full-size CHDs, not this workflow's tiny synthetic
corpus. No RP2350 hardware in CI, so this can't prove `chd_open()`/
`chd_read()` work at runtime against real flash/SD storage - only that the
Cortex-M33 build doesn't regress correctness or blow past budget on what CI
*can* see.
