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

This build sets **`LOWRAM_TARGET=1`**, which turns out to matter enormously
on this class of target - see "LOWRAM_TARGET is not optional here" below.

**Flash corpus: 24/24 files** decode and CRC-verify correctly
(`VERIFY_BLOCK_CRC=1`), AVHuff included.

**SD corpus: 112/128 files** - i.e. **every valid file in the set**:

| outcome | files | what it is |
|---|---|---|
| decoded + CRC-verified | 112 | all 112 non-broken CHDs on the card |
| `OPEN FAILED: invalid file` | 13 | **0-byte files** in the source ROM set |
| `OPEN FAILED: read error` | 3 | **truncated files** - header's `mapoffset` points past EOF |

Zero decompression errors, zero OOM, one reset in the whole run (the
initial power-on). The 16 failures are broken files, not a target
limitation: they fail identically on desktop x86-64, and libchdr rejects
them with the correct error. Verified directly rather than assumed -
`stat` shows 13 at size 0, and the other 3 have a `mapoffset` beyond their
own file length.

Representative throughput (400MHz):

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

Same board, same card, across the two changes that mattered:

| | flash corpus | SD corpus | decompression errors | OOM |
|---|---|---|---|---|
| baseline | 17/24, 4.78 MB out | 18/128 | 16 | 81 |
| + ROM-collision fix | 20/24, 10.63 MB out | 31/128 | **0** | 81 |
| + `LOWRAM_TARGET=1` | **24/24**, 14.16 MB out | **112/128** | **0** | **0** |

Read the SD rows carefully: the baseline run was uncapped, the later two
cap each file at `SD_MAX_HUNKS_PER_FILE` (600). The cap was added *because*
of the fix - files that used to bail out at hunk 0 now decode in full, and
an uncapped 128-file sweep of a 43GB set runs for hours. A cap can only
ever hide failures **past** hunk 600, and all 16 pre-fix failures occurred
at hunks 0-272, so it cannot be manufacturing the "0 decompression errors"
result. The flash corpus is uncapped in all three rows and is the
like-for-like comparison.

## `LOWRAM_TARGET` is not optional here - and it had a correctness bug

With `LOWRAM_TARGET=0` this board looks far more limited than it is: 76 of
128 SD files failed at `chd_open()` with `CHDERR_OUT_OF_MEMORY`, plus 3
CD-FLAC and 2 AVHuff failures further in. The boundary was sharp - the
largest file that opened had a 164KB rawmap, the smallest that failed
needed 176KB, against 582KB free / 524KB largest block. Sizes ran to a
3.1MB rawmap, which cannot fit in this part's SRAM at all under an eager
map.

`LOWRAM_TARGET=1` takes all of that to zero: lazy CHDv5 map decode,
`compressed` grown on demand, and codec `init()` deferred until a hunk
needs that slot. Measured heap cost, once the instrumentation was fixed:

| content | open cost | libchdr resident |
|---|---|---|
| CD (19584B hunks) | **5 KB** | ~6 KB |
| HD (4096B hunks) | **5 KB** | ~5 KB |
| AVHuff (223668B hunks) | 5 KB | ~7 KB |

**Opening a 271328-hunk CHD costs 5KB**, and libchdr's resident footprint
is 5-7KB regardless of file size. Hunk count no longer drives RAM at all;
the real constraint is the caller's own hunk buffer.

It is also close to free on throughput: across the 20 flash-corpus files
that pass in both configurations - uncapped, identical content - total
decode time is 3826ms versus 3822ms, or 1.00x.

**But it selects a second-level huffman lookup table that the default build
never uses, and that path had a bug** (fixed; see the commit resetting the
huffman subtable arena). `huffman_build_lookup_table()` rebuilt the lookup
from scratch on every call without resetting `subtable_count`, so the count
grew monotonically across hunks until it tripped a 2048 guard, after which
every subsequent huffman hunk failed. It also leaked up to 256KB - in the
configuration whose entire purpose is saving memory.

Two things about how that was nearly missed are worth recording:

- It is only reachable with `LOWRAM_TARGET=1` **and** on CHDs that use
  `CHD_CODEC_HUFFMAN`, which is rare - 0.2% of hunks in the file that
  exposed it.
- The sweep that reported "112/128 files, zero decompression errors" was
  **capped at 600 hunks per file**. These files fail ~20000 hunks in. The
  cap introduced to make the sweep tractable is exactly what hid it.

Treat `LOWRAM_TARGET=1` as the default for any target in this memory class -
with that fix applied.

## FLAC: the cost is the decode, not the per-hunk rebuild

`flac_decoder_reset()` calls `drflac_open_with_metadata()` once per hunk -
a full decoder teardown and rebuild, ~40KB allocation, STREAMINFO reparse -
where `cdlz` simply reuses its LZMA state. cdfl costs about 3.6x cdlz per
hunk at identical geometry, so the obvious conclusion was that the rebuild
explained it.

**It does not.** `CHDR_PROFILE_CDFL` splits the three stages, measured on
hardware:

| stage | share | per hunk |
|---|---|---|
| `flac_reset` (the rebuild) | 0.7 - 7.9% | ~0.03 ms |
| `flac_decode` | **58.6 - 95.6%** | 0.245 - 3.622 ms |
| `subcode` (zlib) | 3.7 - 33.5% | ~0.14 ms |

The rebuild is about 1% of cdfl's time. The cost is genuine FLAC decoding.
Reusing the drflac instance across hunks is therefore a **memory**
optimisation, not a throughput one - worth doing, because that 40KB
per-hunk allocation is what produced `CHDERR_OUT_OF_MEMORY` on the three
largest pcenginecd titles under the eager map, but it will not make cdfl
meaningfully faster.

Recorded because the wrong version of this conclusion is very easy to
reach: the rebuild is real, it is obviously wasteful, and it sits directly
in the hot path. Only measurement separates "wasteful" from "expensive".

For reference, per-codec cost at identical geometry (10 hunks of 19584
bytes, RAM-sourced, so no I/O in the number):

| codec | ms/hunk | minus `cd_none` baseline |
|---|---|---|
| `cd_none` | 1.218 | - |
| `cd_cdzs` | 1.901 | 0.683 |
| `cd_cdzl` | 1.945 | 0.727 |
| `cd_cdlz` | 2.303 | 1.085 |
| `cd_cdfl` | 5.087 | **3.869** |

## The storage stack costs 8.4x, and most of it is avoidable

Layering the same card at the same 40MHz clock three ways:

| path | 4 KB | 32 KB | 64 KB | 256 KB |
|---|---|---|---|---|
| `sdmmc_read_sectors()` (driver) | 11.28 | 16.49 | 17.80 | **18.96 MB/s** |
| `f_read()` (FatFs direct) | 9.71 | 10.59 | 10.58 | - |
| `fread()` (VFS + newlib stdio) | 2.30 | 2.30 | 2.30 | 2.30 MB/s |

The card sustains ~19 MB/s and scales with transfer size. FatFs costs 1.8x.
**The VFS and newlib stdio wrapper above it costs another 4.6x**, and delivers
a flat 2.30 MB/s no matter how large the request is.

libchdr never has to care. `core_file_callbacks` already lets the embedder
supply any reader, so a FatFs-backed backend is a drop-in replacement for the
stdio one and **needs no change to the library**. Measured over the 5-file
subset, identical read counts in both configurations - only the cost per byte
differs:

| file | stdio | FatFs | io% stdio -> FatFs |
|---|---|---|---|
| Castlevania X | 2.24 | **5.06** | 88.1 -> 72.8 |
| kinst2 | 1.70 | **2.54** | 61.9 -> 42.8 |
| Ikaruga | 8.34 | **11.58** | 42.5 -> 20.1 |
| Shadowrun | 1.58 | **2.04** | 35.1 -> 16.4 |
| Bonk III | 2.03 | **2.31** | 43.5 -> 35.3 |
| **aggregate** | **2.47** | **3.37 MB/s** | 60.5 s -> 44.4 s |

**Anyone running libchdr on ESP-IDF over FATFS should implement
`core_file_callbacks` over `f_read` rather than `fopen`/`fread`.** It is worth
more than every change in the library measured here put together. Select it in
this benchmark with `-DBENCH_FATFS_BACKEND=1`.

Halving the SD clock to 20MHz costs only 13% (2.05 vs 2.30 MB/s), so the bus
is not the constraint either.

With I/O no longer dominant the balance shifts: Shadowrun and Ikaruga become
84% and 80% CPU, so anything further has to come from decode or from avoiding
decode, not from storage.

### Read-ahead: measured, and it does not pay here

A caller-budgeted read-ahead window (`chd_set_cache_budget()`, 0 by default)
cuts `fread()` calls 6-16x on real files. It bought **1.2%** of throughput and
made p99 hunk latency up to **22x worse**, because a refill transfers the whole
window rather than one hunk. Castlevania X, the most I/O-bound file and the one
it should have helped most, regressed from 2.24 to 2.03 MB/s.

The premise was wrong: transaction count is not the cost. That is visible
directly in the table above - throughput is flat across a 32x range of request
sizes, so the path is bandwidth-bound in software, not transaction-bound. The
feature is kept default-off because the mechanism is sound where per-call cost
genuinely dominates, but it should not be enabled on this evidence.

## Storage, not decode, is the bottleneck

Per-file attribution (timing inside the storage callbacks, which are the
only path from the decoder to the card) puts I/O at 10-85% of wall time
depending on the file. Comparing the same files and the same build flags
against an x86-64 desktop (Ryzen 7 PRO 8840HS) separates the two cleanly:

| | ESP32-P4 | x86-64 | ratio |
|---|---|---|---|
| whole sample, wall clock | 2345.9 s | 145.3 s | **16.1x** |
| decode only (wall x (1 - io%)) | | | **6.0 - 8.6x**, mean ~7.2x |

The CPU-only ratio is remarkably tight across four codec families, five
hunk geometries and a 500x file-size range. A ~7x gap between a 400MHz
RISC-V core and a modern x86 core is about what clock and microarchitecture
predict on their own - libchdr's decode is not doing anything pathological
on RISC-V. Everything beyond that ~7x is storage, and it is the part worth
optimising.

Two caveats on the comparison: the x86 side reads through the page cache,
so its I/O is nearly free, and the CPU-only column is derived by
subtracting measured io%, not measured directly.

### Read amplification

libchdr reads whole hunks only - no sub-hunk API, no decoded-hunk cache for
ordinary reads - so a sub-hunk request costs a full hunk decode. Measured
at 8 units per hunk:

| units | aligned hunks/req | amp | unaligned hunks/req | amp |
|---|---|---|---|---|
| 1 | 1.00 | **8.00x** | 1.00 | 8.00x |
| 2 | 1.00 | 4.00x | 1.10 | 4.40x |
| 4 | 1.00 | 2.00x | 1.43 | 2.85x |
| 8 | 1.00 | **1.00x** | 2.00 | **2.00x** |
| 16 | 2.00 | 1.00x | 3.00 | 1.50x |

Latency is flat from 1 to 8 units - a one-sector read takes the same wall
time as a whole hunk. **Integrators should read whole, hunk-aligned
hunks**; an unaligned 8-unit read touches two hunks and doubles the work
for nothing.

Random access costs **1.8-2.6x** sequential, which is the first
measurement of what `v5_resume_cache`'s sequential fast path is worth.

### FATFS fast seek is essential on this storage

FatFs walks the FAT cluster chain on every `f_lseek`, and restarts from the
first cluster on any **backward** seek. Every `COMPRESSION_SELF` reference
is backward (measured: 100%, all 14 sample files), so a self-reference-heavy
CHD pays a full chain walk per hunk. On a 271328-hunk, 29.2%-self-ref file
this took reads from 2.7 to 85 ms/hunk and made a run look hung.

`CONFIG_FATFS_USE_FASTSEEK=y` makes `lseek` O(1) via a cluster link map
table. In the same 14 minutes the run then reached hunk 245000 instead of
25000 - about **10x**.

Caveat worth knowing: ESP-IDF allocates a fixed
`CONFIG_FATFS_FAST_SEEK_BUFFER_SIZE` (64 words, ~31 fragments) per open
file and, if a file needs more, **silently frees it and reverts to the slow
path** - no error, no log line. Every file on this card is a single
fragment (32KB clusters), so it engages here; a fragmented card would
quietly get nothing. FatFs reports the required size in `cltbl[0]` even
when it fails, so a two-phase allocation would size it exactly.

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

**All 4 AVHuff files pass under `LOWRAM_TARGET=1`.** The working-set
figures below still hold and still argue for the streaming redesign - what
changed is that deferring codec `init()` stops AVHuff's state from being
allocated alongside the three other codecs a CHDv5 header may list, which
is enough headroom on this part. Read this section as "why AVHuff is tight
even when it works", not as a blocker.

With `LOWRAM_TARGET=0`, all 4 AVHuff corpus files failed with
`malloc(219660)`/`malloc(223668) failed` despite ~587KB heap reported free
moments earlier.
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
