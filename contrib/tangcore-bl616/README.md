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

## FLAC backend: micro-flac by default

The integration patch decodes FLAC through
[micro-flac](https://github.com/esphome-libs/micro-flac) rather than dr_flac,
with byte-identical output. Clone it before applying the patch:

    git -C thirdparty clone https://github.com/esphome-libs/micro-flac.git
    git -C thirdparty/micro-flac checkout ffbe8a9ba5e78c16e535a4695a8b2418b0c091ee

Pinned, because the backend compiles against micro-flac's internals. Fetched,
never vendored: it is Apache-2.0 and libchdr stays BSD-3 for its other
consumers. TangCore's firmware is already Apache-2.0 end to end, so it adds no
obligation here - but if it does not suit you, apply
`patches/firmware-bl616-libchdr-drflac.patch` on top and you are back on
dr_flac. CI builds both, so neither path rots.

**What is actually known.** Nothing has been measured on BL616 - there is no
hardware in CI, and this board has never run a decode. On an ESP32-S3 with I/O
excluded it is **1.198x** on a CD-FLAC hunk and **1.233x** on raw FLAC; across
eleven real discs **1.072x** overall, 1.21x where the image is FLAC-heavy, and
**0.988x** on one profile where FLAC barely appears. An RP2350 Cortex-M33 gives
1.032x overall. Peak heap is lower than dr_flac's on most images, and the
worst-case largest-free-block is better.

Earlier revisions of this file quoted 1.46x and 1.41x. Those predate the
STREAMINFO block-size fix, which removed an oversized decoded-sample buffer
from dr_flac and took most of micro-flac's lead with it. Do not use them.

The gain is on the FLAC part of the decode only. On a board reading over SPI,
storage is usually the larger share of wall time - see the read-ahead budget
above, which attacks that side.

## Status (2026-08-25)

Compiles and links clean, `LOWRAM_TARGET=1`. Real flash cost: +142.5KB (whole
codec suite) out of a 4MB budget. Static SRAM cost of linking libchdr in is
negligible (~80B) - the real dynamic heap cost (~250KB-class, per
`project_avhuff_wip` memory) only shows up once `chd_open()` actually
succeeds, which needs a mounted filesystem this smoke test doesn't have.
Not yet wired into an actual TangCore core loader - no CD-capable core
exists in nand2mario's ecosystem yet (`mdtang`/Genesis and `pctang`/PC-XT
have no CD-ROM support), that's being built separately.
