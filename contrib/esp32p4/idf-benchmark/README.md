# ESP32-P4 real-hardware throughput benchmark

A real ESP-IDF application (not the compile+link smoke test in
`contrib/esp32p4/smoke_test.c`) that actually runs on a physical ESP32-P4
board, opens and fully decodes a CHD corpus embedded in flash, and reports
per-file and aggregate decode throughput over the USB serial console.

Built and run against a real **Waveshare ESP32-P4-NANO** (chip revision
v3.1, 16MB flash, 32MB PSRAM - PSRAM unused here, everything runs from
internal SRAM).

## Corpus

`gen_embed.sh` generates `main/embed/*.h` (gitignored, regenerated on
demand) from:

- `tests/corpus/seeds/*.chd` - the full 17-file synthetic corpus (one file
  per codec/order combination) also used by `lowram-correctness.yml`.
- `tests/avhuff_corpus/{regtest,synth}/*.chd` - the 4-file AVHuff corpus.
- Up to 3 small real MAME-derived CHDs pulled from an actual game library
  (`REAL_CHD_DIR`, default `/userdata/roms/pcenginecd`) - optional, skipped
  if not present, kept small (<2MB each) so they fit in flash without a
  custom partition layout beyond `partitions.csv`'s 6MB app slot.

24 files, ~2.4MB of embedded CHD data total.

## Building and running

```sh
git clone --recursive -b v5.5.5 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
source ~/esp/esp-idf/export.sh

cd contrib/esp32p4/idf-benchmark
./gen_embed.sh
idf.py set-target esp32p4
idf.py -p /dev/ttyACM0 build flash monitor
```

**Must be ESP-IDF v5.5.3 or later** (or v6.0+) if your board's ESP32-P4 is
chip revision v3.x - earlier ESP-IDF versions' bootloader rejects it
outright (`bootloader/bootloader.bin requires chip revision in range
[v0.1 - v1.99]`). v3.x silicon changed over 50 registers vs earlier
revisions; see
[Espressif's chip revision v3.x guide](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.html).

## Results (2026-09-02, real hardware)

```
=== libchdr ESP32-P4 real-hardware throughput benchmark ===
free heap: 587264 bytes (largest block: 524288)
```

Two corpora are run back to back: the flash-embedded synthetic one (below)
and a real SD card holding 128 CHDs from a 43GB retro ROM set, read through
the real FATFS/SDMMC stack - the actual ESP32-P4-NANO deployment path.

**Flash corpus: 20/24 files** decode and CRC-verify correctly
(`VERIFY_BLOCK_CRC=1`). The 4 failures are all AVHuff, and all are RAM
headroom, not decode (see below).

**SD corpus: 31/128 files**, with **zero decompression errors**:

| outcome | files | what it is |
|---|---|---|
| decoded + CRC-verified | 31 | |
| `OPEN FAILED: out of memory` | 76 | RAM wall - large CHDs' rawmap + codec state vs ~560KB SRAM |
| `OPEN FAILED: invalid file` / `read error` | 16 | naomi GD-ROM parent/child sets - **fail identically on desktop x86-64**, corpus artifacts, not a target issue |
| `READ FAILED: out of memory` | 3 | drflac's per-hunk allocation, see below |
| harness dest-buffer `malloc` failed | 2 | AVHuff, see below |

Every remaining failure is a RAM-capacity limit or a corpus artifact; none
is a decode defect. One reset in the whole run (the initial power-on).

Representative throughput (400MHz, `CHDR_LOWRAM_TARGET=OFF` - full per-hunk
map materialized at open, no checkpointed re-decode):

| file | codec | hunkbytes | out MB/s |
|---|---|---|---|
| cd_none | uncompressed | 19584 | 16.4 |
| cd_cdzl / cd_cdzs | CD zlib/zstd | 19584 | ~9.2 |
| cd_cdlz | CD LZMA | 19584 | 7.8 |
| cd_cdfl / cd_default | CD FLAC(+subcode) | 19584 | ~3.4 |
| hd_lzma | LZMA | 4096 | 9.9 |
| hd_zstd | zstd | 4096 | 18.3 |
| hd_huff | huffman | 4096 | 3.9 |
| real_Hawiian_Island_Girls (147 real CD hunks) | mixed | 19584 | 2.3 |

Before/after the ROM-collision fix described below, same board, same card:

| | flash corpus | SD corpus | decompression errors |
|---|---|---|---|
| before | 17/24, 4.78 MB out | 18/128 | 16 |
| after | 20/24, 10.63 MB out | 31/128 | **0** |

Read the two SD rows carefully: the "before" run was uncapped, the "after"
run caps each file at `SD_MAX_HUNKS_PER_FILE` (600). The cap was added
*because* of the fix - files that used to bail out at hunk 0 now decode in
full, and an uncapped 128-file sweep of a 43GB set runs for hours. A cap
can only ever hide failures **past** hunk 600, and all 16 pre-fix failures
occurred at hunks 0-272, so it cannot be manufacturing the "0 decompression
errors" result. The flash corpus is uncapped in both rows and is the
like-for-like comparison; it also independently went 17/24 -> 20/24 with
2.2x the bytes decoded.

## FLAC: `drflac` is re-allocated once per hunk

Three pcenginecd titles fail with `CHDERR_OUT_OF_MEMORY` at the first
CD-FLAC hunk. They are exactly the three largest files in the corpus by
hunk count (11055, 11271, 11825); the largest passing file is 7312 - a
clean monotone boundary, which is the signature of a headroom wall rather
than a data-dependent decode bug. All three decode fully on desktop, in
both LP64 and ILP32 (`gcc -m32`, so `DRFLAC_64BIT` isn't the difference).

The mechanism: `flac_decoder_reset()` calls `drflac_open_with_metadata()`
*per hunk*, which allocates a fresh decoder plus a decoded-sample buffer
sized from STREAMINFO - about 40KB for a CD-FLAC hunk - and frees it
again on the next hunk. Once a title's rawmap is large enough
(12 bytes/hunk, so ~142KB at 11825 hunks), that 40KB no longer fits.

This used to surface as `CHDERR_DECOMPRESSION_ERROR`, which is what made
it look like a second instance of the miniz bug. `flac_decoder_reset()`
now routes drflac through allocation callbacks that record failure, so
`libchdr_codec_cdfl.c`/`libchdr_codec_flac.c` can return
`CHDERR_OUT_OF_MEMORY` instead - confirmed on hardware
(`stage=flac_reset ... alloc_failed=1`). Reusing one drflac instance
across hunks instead of rebuilding it every time would both remove this
failure and save the per-hunk malloc/free churn; not attempted here.

## Resolved: ESP32 ROM `miniz` symbol collision

**Symptom** (what this section used to describe as an unsolved
silicon/toolchain mystery): `hd_zlib` plus 15 real CHDs failed with
`CHDERR_DECOMPRESSION_ERROR` from `zlib_codec_decompress()`, only on real
hardware, bit-identically across power cycles.

**Root cause.** Espressif's ROMs bake in an *older* miniz and export its
entry points from the target's ROM linker script as **absolute** symbols
(`esp_rom/esp32p4/ld/esp32p4.rom.ld`, "Group miniz":
`tinfl_decompress = 0x4fc000f8;`). A linker-script assignment outranks an
ordinary object definition, so an ESP-IDF build silently binds that name
to ROM and drops the copy compiled from `deps/miniz-3.1.2/miniz.c` - even
though both are present in the archive. The result was a *split decoder*:
`mz_inflateInit2()`/`mz_inflate()` from miniz 3.1.2 set up and interpret a
3.1.2-layout `tinfl_decompressor`, then handed it to a ROM
`tinfl_decompress()` that lays that struct out differently (miniz 3.0
reworked the Huffman tables from `tinfl_huff_table m_tables[3]` to the
flattened `m_look_up`/`m_tree_N` form, changing both field offsets and
total size).

Confirmed directly, not inferred:

```
$ riscv32-esp-elf-nm build/...elf | grep -w tinfl_decompress
4fc000f8 A tinfl_decompress          <-- absolute, = the ROM address
$ riscv32-esp-elf-objdump -d ...     <-- inside our own mz_inflate:
40026f14: jalr 484(ra)  # 4fc000f8 <tinfl_decompress>
```

while `nm build/esp-idf/libchdr/liblibchdr.a` shows libchdr's own
`00000000 T tinfl_decompress` compiled but never linked.

**Why the failure looked like bad data.** Every one of the 16 failures
consumed *exactly 2 bytes* with `total_out=0` and `zerr=-3`:

```
zlib_codec_decompress: FAILED complen=20 destlen=4096 zerr=-3 total_out=0 avail_in=18 ...
zlib_codec_decompress: FAILED complen=14653 destlen=18816 zerr=-3 total_out=0 avail_in=14651 ...
```

That is not a decode failure - it is `mz_inflate()` taking the
`TINFL_FLAG_PARSE_ZLIB_HEADER` branch (2-byte CMF/FLG check) on a
*raw-deflate* stream opened with `inflateInit2(..., -MAX_WBITS)`. The
flag is set from `pState->m_window_bits > 0`, and `m_window_bits` lives
*inside* the `inflate_state` allocation, just before `m_dict[32768]`: the
larger ROM decoder writing its tables past the end of the smaller
3.1.2-layout `m_decomp` corrupts it. Hence the signature "first hunk of a
stream decodes, every later one fails".

`CONFIG_HEAP_POISONING_COMPREHENSIVE=y` was structurally blind to this -
the corruption is *intra-block*, so it never reaches a canary, and
`zlib_fast_alloc()`'s 1KB size rounding leaves enough slack to absorb the
overrun.

**Fix.** Namespace the colliding symbols on ESP-IDF builds. Six names
collide: `tinfl_decompress`, `tinfl_decompress_mem_to_{heap,mem,callback}`,
`mz_adler32` and `mz_free` (the last one matters independently - binding it
to ROM hands ESP-IDF-heap pointers to the ROM allocator).

The renames live in **`cmake/EspRomMinizWorkaround.cmake`** at the repo
root, applied as compile definitions to whichever target compiles
`miniz.c` - the `miniz` target from libchdr's own CMake, and
`${COMPONENT_LIB}` from this benchmark's `components/libchdr`. It is a
no-op off ESP-IDF. Deliberately *not* patched into
`deps/miniz-3.1.2/miniz.h`: that tree is vendored verbatim so it can be
re-synced from upstream, and an edit there would be silently dropped by
the next miniz bump - which would resurrect this bug with no diff to
point at. Only `miniz.c` references these names, so scoping the defines
to that target is sufficient.

Cheap regression check, no flashing required - this must print nothing:

```sh
grep -hoE '^[A-Za-z_][A-Za-z0-9_]* = 0x' \
    "$IDF_PATH"/components/esp_rom/esp32p4/ld/esp32p4.rom*.ld \
  | sed 's/ = 0x//' | sort -u > /tmp/rom_syms.txt
riscv32-esp-elf-nm build/esp-idf/libchdr/liblibchdr.a \
  | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u > /tmp/chdr_syms.txt
comm -12 /tmp/rom_syms.txt /tmp/chdr_syms.txt
```

**Why this took so long to find.** Every desktop and QEMU null result
below was *correct* - and none of them could have caught this, because
none was linked against ESP-IDF's ROM linker scripts, so all of them used
real miniz 3.1.2. They are now confirming evidence rather than confusing
evidence. Kept for the record:

- `heap_caps_check_integrity_all()` (with `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`)
  never flags corruption, and free/largest-block heap numbers are flat
  through the run - **not** heap corruption or fragmentation.
- Per-hunk tracing shows `inflate()` itself returns `Z_DATA_ERROR` (-3) a
  couple of bytes into a short/degenerate compressed stream (e.g.
  `complen=20` for a 4096-byte all-zero hunk) - the destination buffer is
  never written (still shows the pre-fill poison pattern).
- The exact same 20 compressed bytes, run through the exact same
  `miniz.c`/`mz_inflate` with the exact same `MINIZ_NO_*` defines, decode
  **correctly** on: x86-64 desktop, x86-32 desktop (`gcc -m32`, so ILP32
  vs LP64 isn't it either), and vanilla `riscv64-unknown-elf-gcc` 13.2.0
  targeting `rv32imafc/ilp32f` under `qemu-system-riscv32` (so it isn't a
  general RV32/ILP32 portability bug in miniz).
- Only Espressif's own GCC 14.2.0 `riscv32-esp-elf` toolchain, compiling
  for the real chip, reproduces it. Ruled out as the specific cause:
  their `xesppie` ISA extension (`-march=rv32imafc_zicsr_zifencei`,
  dropping it, made no difference) and optimization level (`-Og`, `-O2`,
  and `-O0` all fail identically).
- zstd/LZMA/huffman/FLAC-coded hunks are all unaffected - this is specific
  to the zlib/miniz `inflate()` path.

A *faithful* repro - real `zlib_codec_init()`/`zlib_codec_decompress()`,
called twice on the same codec instance with the real hunk 0 (42 bytes,
succeeds) then hunk 1 (20 bytes, the one that failed on hardware)
compressed bytes pulled straight from a `CHDR_DEBUG_ZLIB` dump - also
passed on x86-64, x86-32, and (freestanding, no ESP-IDF, no libc)
Espressif's own GCC 14.2.0 under `qemu-system-riscv32`. Freestanding is
exactly the point: no ROM linker script, so no collision.

`src/libchdr_codec_zlib.c` keeps a `CHDR_DEBUG_ZLIB`-gated,
failure-path-only diagnostic printf (`zerr`/`avail_in`/`avail_out`/full
compressed-input hex), off by default. `libchdr_cdrom.c` and
`libchdr_codec_cdfl.c` add stage tags (`stage=base`/`subcode`,
`stage=flac_reset`/`flac_decode`/`subcode`) under the same flag, which is
what separated the two independent bugs here. Note the earlier
unconditional per-call version of this dump flooded UART badly enough to
destabilize a 128-file run on its own - keep it on the failure path.

**Generalizable lesson.** Espressif ROMs export more than miniz. Any
third-party dep vendored into an ESP-IDF build should be checked against
the target's `rom.ld` symbol list before its behaviour is blamed on
silicon. The check is three shell lines (above) and would have saved this
investigation several days.

## AVHuff RAM headroom

All 4 AVHuff corpus files fail with `malloc(219660)`/`malloc(223668)
failed` despite ~587KB heap reported free moments earlier.
`heap_caps_print_heap_info()` at the failure point shows the real number:
only ~78-82KB actually free, because `chd_open()` on an AVHuff CHD
allocates ~500-530KB of internal codec working state (previous-frame
reference buffer + working buffer + Huffman tables - AVHuff's current,
non-streaming decode design keeps at least one full video frame
resident). That's on top of the ~220KB destination buffer this benchmark
also allocates. This matches, almost exactly, the ~633KB whole-hunk
working-set figure already predicted against BL616's RAM budget earlier in
this project (see `project_avhuff_wip` memory) - this board's usable
internal SRAM (~560-590KB total across `RETENT_RAM`/`RAM`/two smaller
pools) hits the same wall. The board has 32MB PSRAM, unused by this build
(`CONFIG_SPIRAM` not enabled) - wiring PSRAM into the heap would likely
fix this specific failure, at the cost of PSRAM's slower access time
skewing AVHuff's throughput numbers relative to internal-SRAM-only
figures above. Confirms the streaming-decode redesign scoped earlier in
this project (avhuff-only additional entry point, ~257KB projected working
set) is worth doing for real memory-constrained targets, not just a
theoretical concern.
