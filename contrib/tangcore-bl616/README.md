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
  open a CHD from an SD card or USB drive under this firmware.
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

## Status (2026-08-25)

Compiles and links clean, `LOWRAM_TARGET=1`. Real flash cost: +142.5KB (whole
codec suite) out of a 4MB budget. Static SRAM cost of linking libchdr in is
negligible (~80B) - the real dynamic heap cost (~250KB-class, per
`project_avhuff_wip` memory) only shows up once `chd_open()` actually
succeeds, which needs a mounted filesystem this smoke test doesn't have.
Not yet wired into an actual TangCore core loader - no CD-capable core
exists in nand2mario's ecosystem yet (`mdtang`/Genesis and `pctang`/PC-XT
have no CD-ROM support), that's being built separately.
