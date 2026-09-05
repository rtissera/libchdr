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

## The test corpus

Fourteen real images, referred to throughout by letter so the numbers stay
comparable across sections. What matters about each is its geometry and codec
mix, which is what the labels record:

| | kind | hunks | hunk size | dominant codecs |
|---|---|---|---|---|
| disc A | GD-ROM | 68,645 | 19,584 | cdzs 92.9% |
| disc B | CD | 11,825 | 19,584 | cdfl 79.0% |
| disc C | CD | 16,400 | 19,584 | cdfl 64.6%, 96 tracks |
| disc D | CD | 125 | 19,584 | cdlz 97.6% (smallest) |
| disc E | CD | 147 | 19,584 | cdlz 100% |
| disc F | CD | 170 | 19,584 | mixed |
| disc G | CD | 17,393 | 19,584 | cdlz 93.6% |
| disc H | CD | 34,709 | 19,584 | cdlz 100% |
| disc I | CD | 25,707 | 19,584 | cdlz 90.6% |
| image J | disc image | 271,328 | 4,096 | zstd 35.6%, uncompressed 34.8% |
| disc K | CD | 13,291 | 19,584 | cdzl 58.7% |
| disc L | CD | 268,676 | 2,448 | cdfl 93.6%, one frame per hunk |
| disc M | GD-ROM | 68,645 | 19,584 | cdlz 88.6% (largest) |
| image N | hard disk | 6,135 | 9,792 | cdlz 55.6%, cdzl 36.8% |
| image O | hard disk | 111,737 | 4,096 | lzma + zlib + huff + flac |

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

Real discs: disc D (CD, 125 hunks, cdlz 97.6%) 468.58 -> 409.54 ms (1.144x), disc E (CD, 147 hunks, cdlz only)
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
| disc A | 68,645 | 12.71 | 97.1% | 2.9% |
| disc M (GD-ROM, cdlz 88.6%) | 68,645 | 9.55 | - | - |
| image O | 111,737 | 6.49 | 68.1% | 31.9% |
| disc C | 16,400 | 6.33 | - | - |
| disc B | 11,825 | 4.74 | 79.5% | 20.5% |
| disc K | 13,291 | 3.69 | - | - |
| image J | 271,328 | 3.07 | 30.7% | 69.3% |
| disc I | 25,707 | 1.87 | - | - |
| disc G | 17,393 | 1.69 | 84.9% | 15.1% |

Every CD image decodes faster than the drive it shipped on: disc A (GD-ROM, cdzs 92.9%) 69x CD
(5.8x a Dreamcast GD-ROM), disc B (CD, cdfl 79%) 25.8x (a 1x PC Engine CD drive),
disc G (CD, cdlz 93.6%) 9.2x (4.6x a 2x Saturn drive), image J (4096-byte hunks, 271328 hunks, zstd + uncompressed) 1.7x a PSP UMD.

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

image J (4096-byte hunks, 271328 hunks, zstd + uncompressed) is 29.2% self-referential over its 271,328 hunks - but only
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

## Decoding into the caller's hunk: a RAM lever with a copy hidden in it

Each CD codec kept a hunk-sized scratch buffer (19584 B), decoded the sector
data into it, and copied every byte across to the caller's hunk at the end.
`CHDR_CD_SCRATCH_BUFFER=0` removes that buffer: the codec decodes straight into
the caller's hunk and the sectors are spread out to the 2448-byte frame stride
in place, needing only a frame of bounce plus the subcode staging - 3120 B.

The same idea applies to LZMA independently of the CD wrappers. `LzmaDec` keeps
a private dictionary and copies out of it at the end of every call, but each
hunk is an independent stream and the output size is known up front, so the
dictionary can simply *be* the destination - which is what the SDK's own
one-call `LzmaDecode()` does.

### What it buys

Peak heap, ESP32-P4/S3 benchmark, real discs from the embedded corpus:

| | before | after |
|---|---|---|
| disc D | 142 KB | **80 KB** |
| disc E | 104 KB | **61 KB** |
| disc F | 106 KB | **63 KB** |
| Saturn CD, desktop | 224624 B | **150576 B** |

The same thing measured where CI can see it - `tests/rv32/fw.c` under
qemu-system-riscv32, `-DCHDR_LOWRAM_TARGET=ON`, which is the BL616-class
configuration and reports peak heap per codec exactly:

| codec | before | after | |
|---|---|---|---|
| cd_cdlz | 99014 B | **57906 B** | -41108 (-41.5%) |
| cd_cdzl | 66249 B | **49785 B** | -16464 (-24.9%) |
| cd_cdzs | 145257 B | **128793 B** | -16464 (-11.3%) |
| hd_lzma | 32022 B | **27858 B** | -4164 (-13.0%) |
| hd_zlib / hd_huff / hd_zstd | unchanged | | |

cd_cdlz gets both changes, which is why it moves twice as far as its neighbours.
The budgets in `tests/rv32/check_budget.py` are ceilings with headroom and were
left alone; they could be tightened to lock this in.

(The three discs above are decoded from flash, so those figures are libchdr's
own working set with no filesystem stack in them. On the SD path disc A (GD-ROM, cdzs 92.9%)'s peak goes
205 -> 188 KB, the rest being FatFs, the VFS and the SPI driver.)

The number that matters more than the peak is the **smallest largest-free-block**
over a run: on the disc A (GD-ROM, cdzs 92.9%) SD sweep, 31 KB before and 39 KB after. What stops
being allocated is a hunk-sized block *per codec*, and a hunk-sized hole is
exactly what the next hunk-sized allocation needs. That is the failure mode
behind the ESP32-S3 OOMs, not the total. Treat this metric as indicative rather
than precise - it moved between 39 and 55 KB across builds that all drop the
scratch, so it carries run-to-run variance the peak does not.

### The trap: memmove on overlapping regions

Frames move up 96 bytes each, so source and destination overlap and the obvious
in-place spread is a `memmove`. **Do not.** Measured on the ESP32-S3:

| disc A (68645 hunks) | wall | CPU/hunk | I/O |
|---|---|---|---|
| private scratch (master) | 270561 ms | 3.131 ms | 51693 ms read |
| in-place, memmove | 310193 ms | 3.722 ms (+18.9%) | 51049 ms read |
| in-place, memmove, packing reversed | 327386 ms | 3.958 ms (+26.4%) | 52150 ms read |
| **in-place, frame bounce (ships)** | **273232 ms** | **3.175 ms (+1.4%)** | 51717 ms read |

I/O time is the same in all four; the whole difference is CPU. Bouncing each
sector through a 2352-byte buffer - two non-overlapping `memcpy`s - recovers
almost all of it, and on the flash corpus, which decodes the same content with
no card in the loop, it is not measurable at all:

| flash corpus | master | in-place, memmove | in-place, bounce | scratch + LZMA in-place |
|---|---|---|---|---|
| cd_cdlz | 26.52 ms | 27.23 | **26.51** | 26.46 |
| cd_cdfl | 64.81 ms | 74.31 | **65.28** | 65.05 |
| cd_cdzl | 19.90 ms | 20.59 | **19.95** | 19.91 |
| cd_cdzs | 19.25 ms | 19.98 | **19.29** | 19.24 |
| cd_default | 63.92 ms | 73.33 | **64.28** | 64.05 |
| hd_lzma | 2.44 ms | 2.46 | **2.39** | 2.41 |

The last column is the LZMA change alone, with the CD scratch restored: it is
free everywhere, so it is on unconditionally.

### End to end on real discs from the card

Five files, every hunk, ESP32-S3 with LOWRAM_TARGET=1, private scratch against
in-place spread. "largest-free" is the smallest largest-free-block seen during
the run - the headroom a later allocation has to fit into.

| file | scratch | in-place | ratio | peak heap | largest-free |
|---|---|---|---|---|---|
| disc A (Dreamcast, cdzs) | 270561 ms | 273232 ms | 1.010x | 205 -> 188 KB | 31 -> 39 KB |
| disc M (Naomi GD, cdlz) | 375306 ms | 370995 ms | 0.989x | 243 -> 169 KB | 31 -> 74 KB |
| disc B (PCE-CD, cdfl) | 191780 ms | 192567 ms | 1.004x | 239 -> 165 KB | 16 -> 78 KB |
| disc C (Sega CD, cdfl) | 164469 ms | 165002 ms | 1.003x | 238 -> 165 KB | 24 -> 80 KB |
| image N (MAME HD, cdlz+cdzl) | 62852 ms | 62805 ms | 0.999x | 171 -> 138 KB | 64 -> 104 KB |
| **total** | **1064968 ms** | **1064600 ms** | **1.000x** | | |

Confirmed over the full fourteen-file sweep afterwards: 7699129 ms against
7711565 ms, **1.002x** over 7.7 GB and 128 minutes of decoding, with peak heap
down on every CD image and the largest free block up on every one of them
(disc G (CD, cdlz 93.6%) 15 -> 72 KB, disc B (CD, cdfl 79%) 16 -> 58 KB, disc K (CD, cdzl 58.7%) 31 -> 82 KB).

Neutral on time over eighteen minutes of decoding, 33-74 KB off the peak, and
the free-block headroom roughly doubled on every CD image. disc B (CD, cdfl 79%)'s 16 -> 78 KB
is the one to look at: 16 KB is not enough to open another codec, and that is
what an OOM on this board looks like.

### What the direction of the move is not

The natural theory is newlib's `memmove`, which falls back to a byte-at-a-time
loop when the destination is above the source. Packing the sectors at the end of
the hunk so every move runs *downwards* should then take the word-copy path.
It was measured, and it made things **worse** - 3.958 ms/hunk against 3.722.

The flash corpus contradicts it a second way: `cd_cdlz` there moves the same
18816 bytes through the same `memmove` for +3%, where disc A (GD-ROM, cdzs 92.9%) pays +19%. Those
two numbers cannot both be the memmove. The cause was never isolated; the bounce
sidesteps it rather than explaining it. If you come back to this, do not start
from the newlib story - it is measured wrong.

### Default

`CHDR_CD_SCRATCH_BUFFER` follows `LOWRAM_TARGET`: private scratch on where RAM
is not the binding constraint, so hosts with memory to spare keep the old
behaviour and the old numbers exactly, and off on small-RAM targets. Setting it
explicitly pins it either way; `1` is also what a caller wants whose hunk buffer
is uncached, write-combining or a FIFO, since the buffer is then written once,
linearly, and never read back.

With the scratch off, `chd_read`'s buffer must also be 2-byte aligned, because
dr_flac writes `int16_t` into it directly. Anything from `malloc` or an array
already is; an odd pointer returns `CHDERR_INVALID_PARAMETER` rather than
trapping on a target without unaligned stores.

## Optimisation level: both ends of the range lose

Desktop x86-64, full-disc decode, best of three, and libchdr's own `.text`:

| | decode | libchdr .text |
|---|---|---|
| -O0 | - | 155752 B |
| -Os | 2.34 s | 60571 B |
| -O2 | 2.20 s | 75831 B |
| -O3 | 2.20 s | 130061 B |

**-O3 is disqualified**: identical speed to -O2 for 72% more code. All four
levels decode byte-identically against master on every disc tested.

On the ESP32-S3 the same switch is worth a lot of flash:

| | .flash.text | .iram0.text | libchdr.a .text |
|---|---|---|---|
| -O2 (`CONFIG_COMPILER_OPTIMIZATION_PERF`) | 313092 B | 65751 B | 144788 B |
| -Os (`..._SIZE`) | 276880 B | 61555 B | 119968 B |

-40408 B of total text, -24820 B in libchdr alone. The tempting theory is that
this wins on the S3 even though it lost on x86, because code runs from flash
through a cache there. **It does not.** Measured on the card, every hunk:

| file | -O2 | -Os | |
|---|---|---|---|
| disc A | 273232 ms | 310273 ms | 1.136x |
| disc M | 370995 ms | 418968 ms | 1.129x |
| disc B | 192567 ms | 210148 ms | 1.091x |
| disc C | 165002 ms | 184127 ms | 1.116x |
| image N | 62805 ms | 67340 ms | 1.072x |
| **total** | **1064600 ms** | **1190856 ms** | **1.119x** |

12% slower for 40 KB of flash. Keep `CONFIG_COMPILER_OPTIMIZATION_PERF`. Neither
end of the range pays: -O3 costs code for nothing, -Os costs time for space this
board has.

## SD clock on the ESP32-S3: what the parts actually allow

The board (Waveshare ESP32-S3-LCD-1.47) routes only CLK, CMD, DAT0 and DAT3, and
driving them as 1-bit SDMMC times out at `send_op_cond` - already tried, still
dead. So it is SD-over-SPI on SPI3, and `sdmmc_card_print_info` confirms the bus
actually runs at **20.00 MHz (limit 20.00 MHz)**, ESP-IDF's `SDMMC_FREQ_DEFAULT`.

What bounds it, from the sources rather than folklore:

- **The divider.** SPI3 clocks off the 80 MHz APB with an integer divider, so the
  reachable rates are 80, 40, 26.67, 20, 16... **25 MHz is not one of them** -
  asking for 25000 lands back on 20, a no-op. The two real steps up are 26.67
  (80/3) and 40 (80/2).
- **The driver.** `spi_hal_get_freq_limit()` is `APB_CLK_FREQ / (delay_apb_n+1)`
  with `delay_apb_n` derived from `input_delay_ns` plus `GPIO_LL_MATRIX_DELAY_NS`.
  The S3 defines no matrix delay and `sdspi_host` leaves `input_delay_ns` at 0,
  so the driver imposes no limit below 80 MHz. The GPIO matrix is not the
  constraint people assume it is here.
- **The card.** The SD Physical Layer spec caps SPI mode at 25 MHz in default
  speed and 50 MHz in high speed. ESP-IDF runs `sdmmc_init_card_hs_mode`
  unconditionally, SPI included (`sdmmc_init.c:146`, `SDMMC_INIT_STEP(always, ...)`),
  so a card that answers CMD6 is switched to high speed and 26.67 or 40 MHz is
  in spec, not an overclock. `SDMMC_FREQ_HIGHSPEED` is 40000 in ESP-IDF's own
  header.

So the ladder to test was 20 -> 26.67 -> 40. **26.67 MHz does not work on this
board**: the card does not come up at all -

```
E (6512) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).
SD: mount failed (ESP_ERR_INVALID_RESPONSE) - sclk=14 mosi=15 miso=16 cs=21
```

which is the safe way to fail - it is refused during card identification, not
accepted and then silently corrupting. 40 MHz was not attempted after that. So
20 MHz is the ceiling on the Waveshare ESP32-S3-LCD-1.47, and the SD clock is
not a lever here whatever the parts allow on paper. (It was worth 1.219x on the
RP2350, whose SD is wired differently - see the rp2350 notes.)

## The read-ahead budget is the largest lever on this board

`chd_set_cache_budget()` has shipped since the read-ahead/self-reference-cache
work, and the RP2350 harness turns it on by default; the ESP benchmark never
did. At 32 KB, on the in-place build, every hunk of every file:

| file | no budget | 32 KB budget | |
|---|---|---|---|
| disc A | 273232 ms | 231615 ms | 0.848x |
| disc M | 370995 ms | 334669 ms | 0.902x |
| disc B | 192567 ms | 183123 ms | 0.951x |
| disc C | 165002 ms | 150498 ms | 0.912x |
| image N | 62805 ms | 59760 ms | 0.952x |
| **total** | **1064600 ms** | **959664 ms** | **0.901x** |

**1.11x, and it wins on every file** - the only lever measured here that does.
disc A (GD-ROM, cdzs 92.9%)'s I/O share falls from 20.24% of wall to 14.65%. Against master
(scratch buffer, no budget) the same file goes 270561 -> 231615 ms, 1.168x.

It costs exactly its 32 KB: peak heap +32 KB, largest free block -31 KB on every
file. Which is the point of the in-place spread, and this was then measured
rather than argued - four configurations over every hunk of the same five discs:

| config | min free heap | min largest block | result |
|---|---|---|---|
| in-place, no budget | 111 KB | 55 KB | 5/5 files |
| **in-place + 32 KB budget** | 79 KB | 31 KB | **5/5 files** |
| private scratch + 32 KB budget | 45 KB | 23 KB | **4/5 - read error** |
| in-place + 64 KB budget | 44 KB | 15 KB | **4/5 - read error** |

So the budget does not fit on top of a hunk-sized scratch per codec: that
configuration runs out of headroom and dies partway through disc B (CD, cdfl 79%). And 64 KB
does not fit either, dying 1820 hunks from the end of disc A (GD-ROM, cdzs 92.9%), for about 1% more
speed than 32 KB where it does complete. 32 KB on the in-place build is the only
one of the four that finishes, which is why it is the default.

**Running out of heap on this board looks like a read error, not an
out-of-memory error.** Both failures above are `CHDERR_READ_ERROR` from
`hunk_read_compressed`, several thousand hunks in - not `CHDERR_OUT_OF_MEMORY`
and not an allocation failure anywhere in libchdr. What actually happens is that
the SD/FatFs layer underneath cannot get a transfer buffer and returns a short
read. Worth knowing before someone spends a day suspecting the card: check the
`smallest largest-free-block` figure first. Below about 25 KB on this board,
reads start failing.

The in-place spread is not free, and this is where its cost is visible: on the
files that completed, the private-scratch build is ~1.5% faster (disc A (GD-ROM, cdzs 92.9%) 227757
against 231615 ms, disc M (GD-ROM, cdlz 88.6%) 329771 against 334669). That is the frame-bounce
memcpy. The trade is 1.5% of CPU for the headroom that makes an 11% lever
usable - and without it, that lever cannot be switched on at all.
