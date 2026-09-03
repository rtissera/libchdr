# libchdr on ESP32-P4: what costs what, and what does not help

Everything here is measured, on a Waveshare ESP32-P4-NANO at 400 MHz, against
real CHD files. Where a number came from a desktop proxy that is said
explicitly. The point of the document is to stop good-sounding ideas being
re-tried: most of the things in the "does not help" section look obviously
correct on paper and are losses in practice.

Companion to `contrib/esp32p4/idf-benchmark/README.md`, which covers the
storage stack. This file is about CPU.

## The board

    -march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f, 400 MHz
    single-issue, in-order, no Zbb

Measured with a dependent-chain probe on the board:

| op | cycles |
|---|---|
| add, xor | 1.25 |
| mul, mulh | 2.13 |
| `heap_caps_malloc` + `heap_caps_free` | 4.08 us, flat 4 KB..224 KB |
| `memset` 32 KB | 30.9 us |

`xesppie` is Espressif's 128-bit vector extension. The assembler accepts
`esp.vld.128.ip` / `esp.vadd.s8`, but **GCC exposes no intrinsics or builtins**,
and it needs CSR 0x7F2 enablement plus context-switch state. Any use means hand
written assembly.

**ESP32-S3 is Xtensa LX7, not RISC-V.** None of the RV32 work transfers. Its
128-bit SIMD is a TIE extension GCC does not auto-vectorise to.

## One model explains every codec

Profiling the seed CHDs the board actually runs
(`tests/corpus/seeds/cd_{none,cdzs,cdzl,cdlz,cdfl}.chd` - same content, one
codec each), x86-64 instruction counts predict P4 wall clock with a single
slope:

| codec | x86 Ir/hunk | predicted | measured | err |
|---|---|---|---|---|
| cd_none | 21,137 | 1.218 ms | 1.218 ms | fit |
| cd_cdzs | 180,877 | 1.736 | 1.901 | -8.7% |
| cd_cdzl | 209,849 | 1.830 | 1.945 | -5.9% |
| cd_cdlz | 341,315 | 2.257 | 2.303 | -2.0% |
| cd_cdfl | 1,213,474 | 5.087 | 5.087 | fit |

**1.30 cycles per instruction at 400 MHz**, covering a 57x spread in cost. There
is no codec-specific penalty on this core. FLAC is not slow because of RISC-V;
it executes ~127 instructions per audio sample and that is all.

## What shipped, and what it bought

Measured on-board, ms/hunk, identical content, cumulative:

| codec | before | + ECC SWAR | + CRC slice-4 | total |
|---|---|---|---|---|
| cd_cdzl | 1.945 | 1.926 | 1.656 | **1.175x** |
| cd_cdzs | 1.901 | 1.907 | 1.645 | **1.156x** |
| cd_cdlz | 2.303 | 2.304 | 2.038 | **1.130x** |
| cd_cdfl | 5.087 | 5.018 | 4.753 | **1.070x** |
| cd_none | 1.218 | 1.220 | 1.223 | 0.996x |

Real discs: Pyramid Plunder 468.58 -> 409.54 ms (1.144x), Hawiian Island Girls
916.64 -> 847.72 ms (1.081x).

`cd_none` does not move because an uncompressed CHD takes an early path that
reaches neither ECC nor the CRC.

### crc16 slice-by-4 (commit d0ff583) - the biggest single lever

`crc16_update` was a byte-at-a-time table walk: **12 instructions per byte on
RV32**, two of them purely truncating the accumulator back to `uint16_t`. It
runs over every decoded hunk under `VERIFY_BLOCK_CRC`, costing ~0.76 ms per
19584-byte hunk - *more than the zstd decode it was checking*.

Slice-by-4 folds four bytes per iteration through three extra tables
(`s_table1/2/3` = `s_table` advanced one, two and three byte positions), so four
lookups XOR into one result. **6.25 instructions per byte**, +1536 B rodata.

This beats the ECC work because it runs on **every compressed hunk of every
codec on every platform**, where ECC only fires on data sectors whose parity
chdman stripped.

Verified identical for every length 0..4096 **and 256 different starting CRCs** -
the latter matters because the CHD v5 map CRC chains a running value rather than
restarting from 0xffff.

### ECC P parity, SWAR (commit f1a0600)

Three exact algebraic identities, verified over their full domain:

    ecclow[x]      == xtime(x) in GF(2^8), poly 0x11d      (all 256 entries)
    poffsets[r][c] == r + 86c
    qoffsets[r][c] == (86*(r>>1) + (r&1) + 88c) mod 2236

The P rows are independent and, for a fixed component, read consecutive bytes.
So four rows pack into one 32-bit accumulator and the per-component step goes
word-wide, using arithmetic `xtime` because a table lookup cannot be packed
(no gather). This is SWAR, not SIMD: no vector ISA, no intrinsics, portable.

Measured `ecc_generate` over a 2352-byte sector: 47,776 -> 32,513 cycles,
**1.469x**.

Bytes are assembled from four loads rather than read as a word: the P stride is
86 and `86 % 4 == 2`, so consecutive components alternate 4-aligned and
2-aligned whatever the caller's buffer alignment. **This cannot be padded away.**
Measured, the byte-built word beats both an `lw`/`lhu` split by alignment parity
and a padded aligned copy.

**Q cannot be done this way** - its offsets are a diagonal, contiguous in
neither dimension, and it is the larger half (2236 of 4300 components/sector).
That caps any P-side work at ~1.76x on ECC.

### Other shipped work

- **dr_flac 0.13.3 -> 0.13.4** (commit 0765eec). Correctness, not speed. Fixes a
  heap buffer overflow in `drflac__realloc_from_callbacks` (copied `szOld` bytes
  into a smaller buffer when shrinking), and a discarded decode result that let
  a failed subframe report success. Both matter: CHDs are attacker-supplied.
- **CMake now refuses `CHDR_WANT_RAW_DATA_SECTOR=OFF` with
  `CHDR_VERIFY_BLOCK_CRC=ON`** (commit 86a4404). The stored CRC covers the
  reconstituted hunk, so skipping ECC regeneration can never match it - and the
  failure is *content-dependent* (a hunk holding only audio frames has no ECC to
  regenerate and still verifies), so it read as sporadic file corruption.

## Does not help - measured, do not re-try

### ECC: every other rewrite is slower

Cycles per 2352-byte sector on the board, all
producing identical parity:

| variant | cycles | vs shipped |
|---|---|---|
| A table offsets + table xtime (was shipped) | 47,776 | 1.000x |
| B table offsets + arithmetic xtime | 64,635 | 0.739x |
| C closed-form offsets + table xtime | 50,543 | 0.945x |
| D both | 73,453 | 0.650x |
| E P row-inner, auto-vectorisable | 50,084 | 0.954x |
| **F P SWAR 4x, byte-built word** | **32,513** | **1.469x** |
| G P SWAR 4x, aligned lw/lhu | 32,850 | 1.454x |

**One `lbu` from a hot 256-byte table beats six ALU ops** on a single-issue
in-order core. In-loop the table is 3 instructions (base hoisted); arithmetic
`xtime` is 6. x86 instruction counting predicts the opposite (+17.6% Ir for
arithmetic) and is the wrong metric for a load-latency question.

The 8856 bytes of tables *are* still buyable: variant C costs 5.7% of ECC for
all of `poffsets`+`qoffsets` (8600 B). A size option for flash-tight targets,
never a speed change.

**Variant E is target-divergent** and is the trap here. The same restructure
auto-vectorises to 9.7x on x86-64 SSE2, 17.7x with AVX2, 7.84x on aarch64 NEON -
but needs `-O3` (1.08x at `-O2`) and **loses** 4.6% on the P4 and 22% on aarch64
at `-O2`, because without a vector unit the accumulators spill from registers to
a stack array. SWAR wins everywhere and needs no gating.

### FLAC: six hypotheses, all dead

1. **Not clz / missing Zbb.** dr_flac sets `DRFLAC_NO_CPUID` off x86/ARM, so
   `gIsLZCNTSupported` stays false, GCC folds the branch, and the compiled RV32
   object contains **no `__clzsi2` reference at all**. It already uses the
   inline software clz.
2. **Not SIMD.** Default x86-64 builds compile no sse41 rice variant either, so
   desktop profiles were always the same `__scalar` path RISC-V takes. Forcing
   `-msse4.1` buys 1.20x on the function, 1.06x overall.
3. **Not I-cache**, despite `rice__scalar` being 23,206 B on RV32 against
   LzmaDec's 4,532. It is specialised by lpcOrder/riceParam so one call touches
   a few KB. Cachegrind at a simulated 16 KB L1I: cdlz 0.02% miss rate, cdfl
   0.05%.
4. **Not multiply latency.** mul/mulh 2.13 vs add 1.25 cycles, 12.6% static
   multiply density in the rice loop vs 2.9% in LzmaDec, predicting ~6.7%.
5. **Not FPU.** FLAC decode is pure integer; libchdr uses the s16 path.
6. **Not the allocator.** dr_flac allocates ~one 32-64 KB block per FLAC hunk
   (0.82 allocs + 34.7 KB per cdfl hunk, against 0.017 allocs and 714 bytes for
   cdlz - 39x the rate). At 4.08 us that is **0.15% of a cdfl hunk**, 0.28% of
   the whole cdfl-to-cdlz gap. This also settles the reverted drflac arena: a
   RAM fix, never a throughput one - a perfect arena returns under 8 us/hunk.
7. **Not the 64-bit LPC path.** I believed CD audio forced
   `drflac__calculate_prediction_64`. Instrumenting the real decision point over
   **4,729 subframes on three discs: 0.0%**. Max LPC order observed is 12;
   chdman's precision keeps `bps + precision + ilog2(order)` <= 32. dr_flac
   already uses the 32-bit path exclusively.

### PIE (ESP32-P4 vector extension): not worth it

Four independent reasons. The LPC recurrence is serial across samples; only the
8-12-tap dot product is parallel, and each sample needs an unaligned 16-byte
history load where `esp.vld.128` wants 16-byte alignment. Under mid-side coding
the side subframe is 17-bit while PIE multiplies are s16 lanes, so half the
subframes cannot use it. Rice extraction, the other ~40%, is bit-serial and
data-dependent. And it is hand assembly with no compiler support inside a
vendored file. Ceiling ~12-25% for a large, unportable, unmaintainable change.

### DRFLAC_64BIT: expected negative

`drflac_cache_t` is `uint32` on RV32 (`DRFLAC_64BIT` is only defined for LP64),
so the bit-reader cache refills twice as often as on x86-64. Forcing it grows
`rice__scalar` from 7,470 to 8,465 instructions, because every variable 64-bit
shift on RV32 becomes a branch (jump tables go 8 -> 29). The rice path does
three variable-width cache shifts per sample against 0.3 reloads per sample; the
arithmetic does not work. Untested on hardware, expected -5 to -15%.

## Still open

Both are in libchdr's own code, neither started.

### 1. `DR_FLAC_NO_CRC` (~8% of cdfl, and 13 KB of flash)

dr_flac CRCs every FLAC frame, on top of libchdr's own hunk CRC. Measured on
RV32:

| | text | data | bss |
|---|---|---|---|
| CRC on | 64,859 | 80 | 0 |
| `DR_FLAC_NO_CRC` | 51,765 | 80 | 0 |

**-13,094 B of flash (-20%)**; `rice__scalar` shrinks 23,206 -> 19,056; the
compiler drops all three CRC tables. Note `data`/`bss` are unchanged: this saves
**flash, not RAM**.

**Gate it on `VERIFY_BLOCK_CRC`, not on `LOWRAM_TARGET`.** What makes it safe is
that libchdr re-checks the whole decoded hunk against chdman's CRC; that is
`VERIFY_BLOCK_CRC`. Gating on `LOWRAM_TARGET` would permit
`LOWRAM_TARGET=1 + VERIFY_BLOCK_CRC=0`, which removes the *only* integrity check
on FLAC data - the same bug class the RAW/CRC CMake guard just closed. And
`LOWRAM_TARGET` is a RAM-vs-CPU trade for the hunk map, so the name would lie
about a flash saving.

Place it in `src/libchdr_flac.c` beside the existing `DR_FLAC_NO_STDIO`, which
keeps it out of the vendored header.

Seeking is not a concern: libchdr supplies a seek *callback* but never calls
`drflac_seek_to_pcm_frame`.

Open question for the reviewer: dr_flac's CRC catches corruption one level
earlier, before garbage is decoded. A CRC is an integrity check and not a
memory-safety barrier - dr_flac's bounds checks are independent - but the
trade is a judgement call.

### 2. The cdfl wrapper copies the audio three times (~5-6%)

Per hunk, 18,816 audio bytes are written three times:

    dr_flac  -> stack buffer[2352]           write #1   libchdr_flac.c
             -> cdfl->buffer, byteswapped    read+write #2  write_callback
             -> dest, memcpy 2352 per sector read+write #3  cdfl:158

The stack buffer exists *only* so `flac_decoder_write_callback` has somewhere to
read from while byteswapping. The final memcpy exists because `dest` has a
2448-byte stride (2352 audio + 96 subcode) while decode output is contiguous -
but `flac_decoder_decode_interleaved` already chunks at exactly 588 frames =
one sector = 2352 bytes, so each chunk could be decoded straight into
`dest + framenum*CD_FRAME_SIZE`.

The byteswap itself is also poor: a nested per-channel loop with `sampch` and
`shift` as runtime values, ~8-10 instructions per sample. Word-wise in place is
2: `w = ((w & 0x00FF00FF) << 8) | ((w >> 8) & 0x00FF00FF)`.

Risks: `libchdr_codec_flac.c` and `libchdr_codec_avhuff.c` share this wrapper.
AVHuff uses the **non-interleaved** multi-stream branch and has the thinnest
test coverage (4 files). Subcode still needs placing at offset 2352 of each 2448
frame.

## Methodology notes, learned the hard way

- **Never anchor a fit on the point under test.** The first version of the
  cycles-per-instruction table anchored the slope on `cd_none` *and* `cd_cdfl`,
  so cdfl's 0% error was true by construction and the fit could not have
  detected a FLAC-specific cost. Refit on non-FLAC codecs only, cdfl
  extrapolates to 5.332 ms against 5.087 measured.
- **Compare the same file.** A "2.4x unexplained" residual turned out to be an
  artifact of comparing a self-made corpus against on-target numbers from a
  *different* disc. Codec cost ratios are content dependent.
- **x86 instruction counts are the wrong metric for load-latency questions** on
  an in-order core, and they cannot see the target's allocator, its byteswap
  lowering, or its cache width. They predicted the opposite result for ECC.
- **Static instruction counts include dead specialisations.** The 553 `mul` +
  390 `mulh` that made the 64-bit LPC path look hot were in a branch that never
  executes.
- **A flat callgrind profile can be an annotator artifact.** "No hot line" in
  `rice__scalar` was because everything hot is inlined from elsewhere and
  attributed to *those* source lines, outside the function's own range.
- **Synthetic seeds are not real content.** `tests/corpus/seeds/cd_*.chd` do
  **0.0%** ECC work, so they cannot measure an ECC change; ECC only runs on
  frames whose parity chdman stripped. Of 14 real discs on the test card, three
  do 0% ECC (a PSP ISO and two hard-disk CHDs) and the rest range 2-35%.
- **Cap size hides defects.** A 600-hunk cap hid a huffman bug; a 3000-hunk cap
  hid a missing FatFs cluster map.

## Reproducing

    # decode-output equivalence over a CHD corpus
    # (strided sampling - capping from hunk 0 never reaches the audio tracks
    #  that follow the data track, so a naive cap validates nothing about FLAC)

Correctness bar used throughout: decoded output byte-identical over **287 CHDs**,
with `VERIFY_BLOCK_CRC` checking every hunk against chdman's own CRC, plus the
AVHuff regression suite (4/4).

## End-to-end result on the full corpus

Same 14 SD files, same FatFs + cluster-map + 64 KB read-ahead configuration,
both uncapped over 7.32 GB of decoded output:

| | time | throughput |
|---|---|---|
| before the CPU work on this branch | 2307.0 s | 3.17 MB/s |
| after ECC SWAR + crc16 slice-by-4 | **1924.7 s** | **3.80 MB/s** |

**1.20x end to end**, 14/14 files OK. Per file, uncapped:

| file | hunks | MB/s | CPU | IO |
|---|---|---|---|---|
| Ikaruga | 68,645 | 12.71 | 97.1% | 2.9% |
| naomi vathlete | 68,645 | 9.55 | - | - |
| kinst2 | 111,737 | 6.49 | 68.1% | 31.9% |
| Sensible Soccer | 16,400 | 6.33 | - | - |
| Insanity | 11,825 | 4.74 | 79.5% | 20.5% |
| Shadowrun | 13,291 | 3.69 | - | - |
| Castlevania X | 271,328 | 3.07 | 30.7% | 69.3% |
| Surgical Strike | 25,707 | 1.87 | - | - |
| SS-parodius | 17,393 | 1.69 | 84.9% | 15.1% |

Every CD image decodes faster than the drive it shipped on: Ikaruga 69x CD
(5.8x a Dreamcast GD-ROM), Insanity 25.8x (a 1x PC Engine CD drive),
SS-parodius 9.2x (4.6x a 2x Saturn drive), Castlevania X 1.7x a PSP UMD.

## Robustness: three latent bugs, all pre-existing on master

Found by fuzzing under ASan+UBSan. None affect well-formed files; all are
reachable from a malformed one, which matters because CHDs are attacker-supplied.

1. **Signed overflow in the header parser** (149dc57). `get_bigendian_uint32_t`
   did `base[0] << 24` without casting - uint8_t promotes to int, so any byte
   >= 0x80 is UB. Real files never hit it because every field read through it
   is a small count or an ASCII four-char codec tag. The uint48 and uint64
   readers already cast; this one was inconsistent.
2. **Unvalidated bit widths from the file** (56273ca). The v5 map header's
   `lengthbits`/`selfbits`/`parentbits` are raw bytes that become the width
   argument to `bitstream_read()`. Above 32 they make `bitstream_peek()` shift
   by a negative amount. Now rejected as `CHDERR_INVALID_FILE` at parse time.
3. **Shift by 32 refilling an over-consumed bitstream** (598424f). `bits` goes
   negative after over-consumption, so `24 - bits` reaches 32. Only reachable
   with `LOWRAM_TARGET=OFF` - the lazy checkpointed map reaches the huffman
   decoder differently - so fuzzing that covered only the low-RAM path missed
   it. **Fuzz both map implementations.**

Bar now established, all clean:

- **3412 malformed inputs** (mutations of all 17 seed codecs across header,
  map-region, whole-file and truncation strategies, plus 131 structure-aware
  header-field cases) against **both** `LOWRAM_TARGET=ON` and `OFF`, on
  **both 64-bit and 32-bit** builds.
- **546 metadata-targeted inputs** including explicit self-referential cycles
  in the metadata chain, through `chd_get_metadata()`, on both map paths.
- **API misuse**: NULL handles, out-of-range hunks, cache budget 0/1/1TB,
  budget toggled mid-stream (output identical), OOM path. No leaks.
- **Access-order equivalence**: HEAD vs master byte-identical over sequential,
  reverse, random and scattered-sample orders.
- **Config matrix**: LOWRAM on/off, system zlib, system zstd and LTO all
  produce byte-identical output; `WANT_RAW_DATA_SECTOR=OFF` differs as designed.
- **32-bit vs 64-bit** decode byte-identical.
- Compiles clean for **Cortex-M33** (68,816 B .text) and **Cortex-M0+**
  (77,976 B), i.e. both RP2350 cores.

Incidental: no CD image sampled from the corpus has any nonzero subcode, which
is why `CHDR_WANT_SUBCODE=OFF` is output-identical on them - 96 of every 2448
bytes is zeros being decompressed and copied.

## Self-reference locality, and why a bigger decoded-hunk cache is not worth it

Castlevania X is 29.2% self-referential over its 271,328 hunks - but only
**10.0% over the first 2500**, which is why a capped run reads 4.61 MB/s and the
full disc 3.07. Not a regression; the capped number was unrepresentative.

LRU simulation over the real 79,305-self-reference stream:

| entries | RAM at 4 KB hunks | hit % of self-refs |
|---|---|---|
| **1** | **4 KB** | **21.1%** |
| 16 | 64 KB | 25.3% |
| 64 | 256 KB | 27.0% |
| 256 | 1 MB | 31.6% |
| 4096 | 16 MB | 44.4% |
| 65536 | 256 MB | 99.7% |

One entry already captures most of the benefit because **24.0% of
self-references point at the same target as the previous one**. 99.2% are more
than 16 hunks back, with the mode at 2^14-2^15 - 16k to 64k hunks, 64-256 MB
into the file - so no small cache can reach them. The single-entry cache that
ships under LOWRAM is at the knee. 16 MB of PSRAM would avoid 9.5% of reads for
perhaps 6-7% of wall time.

## Portability of the two perf changes

Measured, not extrapolated (aarch64 under qemu, so ratios not absolute times):

| | x86-64 -O2 | x86-64 -O3 | aarch64 -O2 | aarch64 -O3 | RV32 |
|---|---|---|---|---|---|
| crc16 slice-4 (shipped) | 3.55x | 3.52x | 1.97x | 2.42x | 1.9x |
| crc16 **slice-8** (not done) | **6.57x** | **6.42x** | 2.35x | 2.70x | n/a |
| ECC P SWAR32 (shipped) | 2.54x | 2.70x | 4.41x | 4.54x | 1.47x |
| ECC P row-inner (rejected) | 0.79x | 8.36x | 0.84x | 8.19x | 0.95x |

SWAR32 wins on every target at both optimisation levels, which is why it
shipped; row-inner is far better at -O3 with a vector unit but **loses** at -O2
and on RV32, and distro packages are frequently -O2.

**Slice-by-8 is a free 1.85x over slice-4 on 64-bit** for +2 KB of rodata. Not
implemented - it only pays on 64-bit and would add a second code path.
