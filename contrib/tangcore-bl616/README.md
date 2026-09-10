# TangCore / BL616 integration (nand2mario)

Reference integration proving libchdr builds and links against the real
firmware running on Sipeed Tang Console 60K / Primer 25K's onboard BL616
companion MCU ([nand2mario/tangcore](https://github.com/nand2mario/tangcore),
[nand2mario/firmware-bl616](https://github.com/nand2mario/firmware-bl616)).

This is a **compile+link smoke test only** - there is no BL616 hardware in
CI, so it cannot prove `chd_open()`/`chd_read()` work at runtime. What it
does prove: libchdr keeps building cleanly against the real Xuantie/T-Head
toolchain and nand2mario's real bouffalo_sdk fork, on every libchdr change.
See `.github/workflows/bl616-tangcore-build.yml`.

## Licensing - what's ours, what's theirs

libchdr is BSD-3-Clause (`LICENSE.txt`). `firmware-bl616` and `bouffalo_sdk`
are nand2mario's own repos, Apache License 2.0. Nothing here changes
libchdr's own license - same model as `deps/` (each vendored third party
keeps its own license, separate from libchdr's top-level one) - but it's
worth being explicit since this directory touches someone else's project
directly:

- **`chd/chd_fatfs.{h,c}`** - 100% original code, not derived from any
  third-party source. BSD-3-Clause, same as the rest of libchdr (see the
  file headers).
- **`patches/firmware-bl616-libchdr-integration.patch`** - a small (92-line)
  unified diff against nand2mario's actual `CMakeLists.txt` and `main.cpp`
  from `firmware-bl616` (Apache-2.0, © nand2mario). **This directory does
  NOT contain copies of his files** - only the diff, applied at build time
  in CI (`git apply`) against a freshly-cloned, pinned commit. This is
  deliberate: a patch is the standard, minimal way to distribute a
  modification to someone else's code without redistributing the whole
  file, and it keeps this repo from carrying content that isn't ours.
  Applying the patch reproduces two files that remain Apache-2.0/©
  nand2mario, modified - not BSD-3-Clause libchdr content. Nothing under
  `contrib/` is compiled into libchdr itself, so none of this reaches
  libchdr's own build artifacts (`libchdr.so`/`chdr-static.a`).

## Contents

- `chd/chd_fatfs.{h,c}` - a `core_file_callbacks` implementation backed by
  FatFS (`f_open`/`f_read`/`f_lseek`/`f_close`), the bridge libchdr needs to
  open a CHD from an SD card or USB drive under this firmware. Generic (only
  calls `ff.h`, nothing BL616-specific) - also reused as-is by
  `../rp2350/README.md`.
- `patches/firmware-bl616-libchdr-integration.patch` - modifies
  `firmware-bl616`'s `CMakeLists.txt` (vendors libchdr in, sets
  `LOWRAM_TARGET=1`) and `main.cpp` (adds a `chd_link_probe()` call). The
  probe is deliberately not a no-op: an unreferenced library builds and
  links "clean" by silently getting dead-stripped, which proved nothing the
  first time this was tried locally. The probe calls `chd_fatfs_open()` on
  a path that doesn't exist (safe - no SD is mounted yet at that point in
  `main()`), forcing the linker to fully resolve libchdr against this
  toolchain's libc.

## Pinned versions

CI clones fixed commits, not branch heads - `firmware-bl616` and
`bouffalo_sdk` are nand2mario's own repos we don't control, so pinning keeps
libchdr's CI from going red over changes we didn't make. Bump these
manually when picking up upstream changes is actually wanted (the patch
above may need regenerating if `firmware-bl616`'s `CMakeLists.txt`/
`main.cpp` have since diverged).

| repo | commit |
|---|---|
| [bouffalolab/toolchain_gcc_t-head_linux](https://github.com/bouffalolab/toolchain_gcc_t-head_linux) | `c4afe91cbd01bf7dce525e0d23b4219c8691e8f0` |
| [nand2mario/bouffalo_sdk](https://github.com/nand2mario/bouffalo_sdk) | `7f44f9ea6b4ccf96db8c5236c8024b68e2a76df7` |
| [nand2mario/firmware-bl616](https://github.com/nand2mario/firmware-bl616) | `a5a6ea1cf7c81f32c1c3f0f91ea9d913be5ba078` |

## Reproducing locally

```sh
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git
git clone --recurse-submodules https://github.com/nand2mario/bouffalo_sdk.git
git clone https://github.com/nand2mario/firmware-bl616.git
# check out the pinned commits above in each, then:

export PATH="$PWD/toolchain_gcc_t-head_linux/bin:$PATH"

cd firmware-bl616
git apply /path/to/libchdr/contrib/tangcore-bl616/patches/firmware-bl616-libchdr-integration.patch

mkdir -p thirdparty/libchdr
cp -r /path/to/libchdr/include /path/to/libchdr/src thirdparty/libchdr/
mkdir -p thirdparty/libchdr/deps
cp -r /path/to/libchdr/deps/lzma-26.02 /path/to/libchdr/deps/miniz-3.1.2 /path/to/libchdr/deps/zstd-1.5.7 \
    thirdparty/libchdr/deps/

mkdir -p chd
cp /path/to/libchdr/contrib/tangcore-bl616/chd/*.{h,c} chd/

make  # BL_SDK_BASE defaults to ../bouffalo_sdk, TANG_BOARD defaults to console60k
```

## Read-ahead budget

`chd_fatfs_open()` calls `chd_set_cache_budget()` with 32KB on every open.
Compressed hunks are small - a few KB - and laid out strictly sequentially, so
one larger read serves many of them and the fixed cost of each `f_read` (FatFs
bookkeeping, SPI command setup, DMA, interrupt) is paid far less often.

Measured **1.11x on an ESP32-S3** and **1.05-1.12x on an RP2350**, both reading
over SPI, with 32KB the knee on the RP2350: 64KB doubled the cost for under
0.7% more. Not measured on BL616.

The budget is a ceiling, not an allocation request: an image whose hunks exceed
it leaves caching off rather than over-allocating. Set `CHD_FATFS_CACHE_BUDGET`
to 0 to turn it off, or to another size to trade RAM for fewer reads.

## Two things not to try

Both are recorded with their numbers in `docs/perf-esp32p4-findings.md`:

- **`-Os`** is 1.12x *slower* than -O2 on an ESP32-S3. The decoders are
  loop-heavy and lose more to reduced unrolling than the smaller text wins back.
- **`Z7_LZMA_PROB32`** costs 15,980 bytes per LZMA instance for a speedup the
  LZMA SDK only claims for "some CPUs", and was never measured on any target.
  The option has been removed rather than left as a trap.

## Optional: the micro-flac backend

libchdr can decode FLAC through [micro-flac](https://github.com/esphome-libs/micro-flac)
instead of dr_flac (`CHDR_FLAC_BACKEND=microflac`), with byte-identical output.

Apply `patches/firmware-bl616-libchdr-microflac.patch` on top of the
integration patch, with a micro-flac checkout at `thirdparty/micro-flac`. CI
builds both variants, so the C++ backend is known to compile and link against
this vendor GCC 10.2 and its bare-metal libc.

**What to expect, and what is actually known.** Nothing has ever been measured
on BL616 - there is no hardware in CI. On an ESP32-S3, with I/O excluded, it is
**1.198x** on a CD-FLAC hunk and **1.233x** on raw FLAC; across eleven real
discs it is **1.072x** overall, 1.21x where the image is FLAC-heavy, and
**0.988x** on one profile where FLAC barely appears. An RP2350 Cortex-M33 gives
1.032x overall.

Earlier revisions of this file quoted 1.46x and 1.41x. Those predate the
STREAMINFO block-size fix, which removed an oversized decoded-sample buffer
from dr_flac and took most of micro-flac's lead with it. Do not use them.

The gain is on the FLAC part of the decode only. On a board reading over SPI,
storage is usually the larger share of wall time, so measure end to end before
concluding anything - and see the read-ahead budget below, which attacks that
side and is wired in by default.

**Licensing.** micro-flac is Apache-2.0, including its `.S` files. That is
permissive and does not relicense libchdr, and TangCore's firmware is already
Apache-2.0 end to end (Bouffalo SDK and firmware-bl616 both), so it adds no
obligation here. Do not vendor it into libchdr's own tree - fetch it, so
libchdr stays BSD-3 for its other consumers.

**1. Source selection.** `thirdparty/libchdr/src/*.c` globs `libchdr_flac.c`,
which is the dr_flac backend; compiling both is a duplicate-symbol error. List
the sources explicitly, or exclude that one file, and add:

    thirdparty/libchdr/src/libchdr_flac_microflac.cpp
    thirdparty/micro-flac/src/{flac_decoder,decorrelation,frame_header,pcm_packing,crc,lpc}.cpp

with `CHDR_FLAC_BACKEND_MICROFLAC` and `MICRO_FLAC_DISABLE_OGG` defined, and
`-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` on the
C++ sources (the last one is what ESP-IDF does; without it the objects also
reference `__cxa_atexit`, which newlib does provide, so it links either way). Put
only micro-flac's `include/` on the include path - its `src/` holds a `crc.h`
that will shadow another component's own header of that name.

**2. Do not link with `g++`.** A plain C++ link pulls ~82 KB of libstdc++ even
with exceptions off: `std::__throw_length_error` drags in `cow-stdexcept`,
`tinfo`, `cp-demangle` and around fifteen `eh_*.o` objects. ESP-IDF hides this
by wrapping `__cxa_throw`; the BL616 toolchain does not. Link with the **`gcc`
driver** (the T-Head GCC is 10.2, which predates `-nostdlib++`) and supply the
handful of symbols micro-flac actually needs:

```c
/* everything micro-flac wants from the C++ runtime */
#include <stdlib.h>
void *operator new(unsigned sz) { return malloc(sz); }
void *operator new[](unsigned sz) { return malloc(sz); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, unsigned) noexcept { free(p); }
void operator delete[](void *p, unsigned) noexcept { free(p); }
namespace std { void __throw_length_error(const char *) { abort(); } }
```

With those flags the only C++ runtime symbols left undefined are the three the
shim defines - verified by `nm -u` on the compiled objects at the real ABI.
Measured effect on a test image: 154,514 B of text down to 54,569 B, with zero
`libstdc++.a` members linked.

**The shim exists because the toolchain is GCC 10.2, not because of anything
about this chip.** `-nostdlib++` does the same job in one flag and has been
available since GCC 11, so if this ever moves to a newer toolchain, delete the
shim rather than carrying it forward. Do not copy it into a project that is
already on a modern compiler.

**3. Use the vendor toolchain.** The T-Head GCC at
`toolchain_gcc_t-head_linux` and Debian's `gcc-riscv64-unknown-elf` share the
`riscv64-unknown-elf-` prefix, so PATH order decides which one you get, and
`CROSS_COMPILE ?= riscv64-unknown-elf-` does not disambiguate. Debian's ships
the `g++` driver but **no libstdc++ at all** - no `cstddef`, no `libstdc++.a` -
so micro-flac will not compile against it, for reasons that say nothing about
the BL616. Check with `riscv64-unknown-elf-gcc --version`: the vendor one
reports "Xuantie-900".

Upstream GCC is not an option here yet, and it is worth writing down why so
nobody re-derives it. The T-Head vendor extensions themselves are not the
obstacle - GCC has had the XThead* collection since GCC 13. Three other things
are, and all three were still missing when checked against the GCC 15.2 and
16.1 manuals (2026-09):

- `-mtune=e907`, which `bouffalo_sdk` sets, is rejected as an unknown cpu.
  GCC 16 did grow the Xuantie application cores - `xt-c908`, `xt-c910`,
  `xt-c920` and their variants - but not the small embedded E907.
- the `p` (packed SIMD) extension in the ABI string below is not in GCC's
  `-march` table at all; it is still unratified, and the implementations that
  exist live in vendor forks.
- `zpsfoperand` and `xtheade` likewise have no upstream spelling.

`-mtune=size` is the documented substitute for the first, at the cost of the
core-specific tuning. The other two have no substitute. The vendor toolchain is
also frozen: its last commit is from October 2022. So this is a real constraint
rather than an upgrade nobody got round to.

Clang does not unblock it either, checked at the same time against LLVM main.
It carries the same XThead* extensions, knows no E907 either (its only Xuantie
processors are `xt-c910v2` and `xt-c920v2`), and rejects `xtheade` and
`zpsfoperand` outright. It does have a `p` extension where GCC has none - but
as `experimental-p` behind `-menable-experimental-extensions`, implementing
draft 0.21, whereas `zpsfoperand` belongs to the older 0.9.x drafts this core
was built to. So they are not the same instruction set, and P being ratified
some day would not by itself make an upstream compiler target this chip.

The vendor fork is the only route, and it has moved since the pin above.
[XUANTIE-RV/gcc](https://github.com/XUANTIE-RV/gcc) carries three branches
(checked 2026-09):

| branch | last commit | declares `e907` |
|---|---|---|
| `xuantie-gcc-10.2.0` | 2024-07 | yes - c906, c908, c910, c920, e902, e906, e907 |
| `xuantie-gcc-10.4.0` | 2024-12 | yes, plus the c907 family |
| `xuantie-gcc-14.1.1` | 2025-03 | **no** - `riscv-cores.def` is upstream's, no Xuantie cores at all |

So the GCC 14 branch cannot build this chip yet; it looks like a rebase in
progress rather than a finished port. `xuantie-gcc-10.4.0` can, and is two
years of GCC fixes newer than the GCC 10.2 blob pinned in
`bl616-tangcore-build.yml` - but it is still below GCC 11, so it does not
retire the shim above. Moving to it is `firmware-bl616`'s call, not ours.

The community forks are not an alternative: `openbouffalo/xuantie-gnu-toolchain`
was last pushed in 2023 and `revyos/xuantie-gnu-toolchain` in 2024, both behind
the upstream they forked.

micro-flac itself compiles clean at the real BL616 ABI
(`-march=rv32imafcpzpsfoperand_xtheade -mabi=ilp32f`, zero warnings) and its
object code is smaller than dr_flac's there: 36,304 B against 45,129 B.

## Status (2026-08-25)

Compiles and links clean, `LOWRAM_TARGET=1`. Real flash cost: +142.5KB (whole
codec suite) out of a 4MB budget. Static SRAM cost of linking libchdr in is
negligible (~80B) - the real dynamic heap cost (~250KB-class, per
`project_avhuff_wip` memory) only shows up once `chd_open()` actually
succeeds, which needs a mounted filesystem this smoke test doesn't have.
Not yet wired into an actual TangCore core loader - no CD-capable core
exists in nand2mario's ecosystem yet (`mdtang`/Genesis and `pctang`/PC-XT
have no CD-ROM support), that's being built separately.
