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

## Contents

- `chd/chd_fatfs.{h,c}` - a `core_file_callbacks` implementation backed by
  FatFS (`f_open`/`f_read`/`f_lseek`/`f_close`), the bridge libchdr needs to
  open a CHD from an SD card or USB drive under this firmware.
- `firmware-overlay/{CMakeLists.txt,main.cpp}` - full copies of
  `firmware-bl616`'s own files, modified to vendor libchdr in and add a
  `chd_link_probe()` call. The probe is deliberately not a no-op: an
  unreferenced library builds and links "clean" by silently getting
  dead-stripped, which proved nothing the first time this was tried. The
  probe calls `chd_fatfs_open()` on a path that doesn't exist (safe - no SD
  is mounted yet at that point in `main()`), forcing the linker to fully
  resolve libchdr against this toolchain's libc.

## Pinned versions

CI clones fixed commits, not branch heads - `firmware-bl616` and
`bouffalo_sdk` are nand2mario's own repos we don't control, so pinning keeps
libchdr's CI from going red over changes we didn't make. Bump these
manually when picking up upstream changes is actually wanted.

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

mkdir -p firmware-bl616/thirdparty/libchdr
cp -r /path/to/libchdr/include /path/to/libchdr/src firmware-bl616/thirdparty/libchdr/
mkdir -p firmware-bl616/thirdparty/libchdr/deps
cp -r /path/to/libchdr/deps/lzma-26.02 /path/to/libchdr/deps/miniz-3.1.2 /path/to/libchdr/deps/zstd-1.5.7 \
    firmware-bl616/thirdparty/libchdr/deps/

cp /path/to/libchdr/contrib/tangcore-bl616/chd/*.{h,c} firmware-bl616/chd/
cp /path/to/libchdr/contrib/tangcore-bl616/firmware-overlay/CMakeLists.txt firmware-bl616/CMakeLists.txt
cp /path/to/libchdr/contrib/tangcore-bl616/firmware-overlay/main.cpp firmware-bl616/main.cpp

cd firmware-bl616
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
