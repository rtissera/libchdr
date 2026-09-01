# RP2350 (Raspberry Pi Pico 2) integration

RP2350 boots either two Arm Cortex-M33 cores or two Hazard3 RISC-V cores -
picked as a pair at boot (via OTP/bootloader configuration), never both at
once. libchdr builds for either; both are cross-compiled and RAM-budgeted in
CI:

| | Arm (Cortex-M33) | RISC-V (Hazard3) |
|---|---|---|
| toolchain file | `cmake/toolchain-cortex-m33.cmake` | `cmake/toolchain-hazard3.cmake` |
| ISA/ABI | Armv8-M Mainline, hard-float | RV32IMAC_Zicsr_Zifencei, **soft-float** (no FPU) |
| CI workflow | `.github/workflows/rp2350-arm-ram-budget.yml` | `.github/workflows/rp2350-riscv-ram-budget.yml` |
| test dir | `tests/rp2350-arm/` | `tests/rp2350-riscv/` |

## Which one to actually use

**Arm (Cortex-M33) is the better default.** It's what the Pico SDK targets
by default, has a hardware FPU, and has the toolchain/debugger/SDK support
every current RP2350 board and example assumes. Hazard3 is real and Pico-SDK
supported, but newer, has no hardware FPU (everything soft-float, so more
CPU-bound), and has a smaller ecosystem of examples/libraries to build on -
pick it only if you have a specific reason to (e.g. wanting the open-hardware
RISC-V core, or needing its extra PIO/interrupt features some Pico SDK
release notes call out).

For libchdr's own memory footprint, the choice barely matters: peak heap
per codec is driven by allocation sizes inside the codecs, not the core
ISA/ABI, so the two CI workflows above measure almost identical numbers
(see each `check_budget.py` for the current baseline) - both comfortably fit
the RP2350's 520KB SRAM. Soft-float Hazard3 code is somewhat larger/slower
than hard-float Cortex-M33 code for the little floating-point libchdr uses,
but that's a CPU-time/code-size tradeoff, not a RAM one.

## Using this with the Pico SDK

1. Vendor `include/`, `src/`, and `deps/` from this repo into your Pico SDK
   project (or add libchdr as a git submodule), building with
   `CHDR_LOWRAM_TARGET=ON`. The Pico SDK's own `pico_sdk_import.cmake`
   toolchain already targets the right core/ABI for whichever
   `PICO_PLATFORM` you pick (`rp2350-arm-s` or `rp2350-riscv`), so the
   toolchain files in this repo are for CI/local RAM-budget measurement
   only, not needed for a real Pico SDK build.
2. Access storage through FatFS (e.g.
   [carlk3/no-OS-FatFS-SD-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico)
   for SD/MMC over SPI) and reuse
   `../tangcore-bl616/chd/chd_fatfs.{h,c}` as-is - it's a generic FatFS
   `core_file_callbacks` bridge (only calls `ff.h`'s
   `f_open`/`f_read`/`f_lseek`/`f_close`/`f_size`, nothing BL616-specific),
   not duplicated here.
3. Mount your SD card / flash filesystem, then call
   `chd_fatfs_open("game.chd", &fil, &chd)`.

## CI

Both workflows cross-compile `chdr-static` and run a peak-heap-per-codec
probe under QEMU (`qemu-system-arm`'s `mps2-an505` for Cortex-M33,
`qemu-system-riscv32`'s `virt` for Hazard3/rv32imac) - neither is the real
RP2350 SoC, only the core ISA/ABI and heap high-water numbers matter here.
Measured baseline (2026-08-31, `CHDR_LOWRAM_TARGET=ON`, both cores):

| codec   | Cortex-M33 peak | Hazard3 peak | budget  |
|---------|-----------------|--------------|---------|
| hd_zlib | 52,074 B        | 52,074 B     | 100,000 |
| hd_zstd | 105,817 B       | 105,825 B    | 200,000 |
| hd_lzma | 30,642 B        | 30,642 B     | 80,000  |
| hd_huff | 21,839 B        | 21,839 B     | 60,000  |
| cd_cdzl | 136,341 B       | 136,341 B    | 250,000 |
| cd_cdzs | 243,768 B       | 243,784 B    | 400,000 |
| cd_cdlz | 135,427 B       | 135,427 B    | 250,000 |

All well inside the RP2350's 520KB SRAM even before accounting for
`LOWRAM_TARGET`'s per-hunk-map savings on real (larger, more-hunks) discs -
see each `check_budget.py` for how these are enforced and
`.github/workflows/rv32-ram-budget.yml`'s comment for why `LOWRAM_TARGET`
matters most on real full-size CHDs, not this workflow's tiny synthetic
corpus. No RP2350 hardware in CI, so this can't prove `chd_open()`/
`chd_read()` work at runtime against real flash/SD storage - only that
neither build regresses correctness or blows past budget on what CI *can*
see.
