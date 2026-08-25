#!/usr/bin/env bash
# Generate a tiny CHDv5 fuzz corpus spanning all codec combos.
# Requires: chdman (from MAME), dd, mktemp.
set -euo pipefail

CORPUS="$(cd "$(dirname "$0")" && pwd)/seeds"
mkdir -p "$CORPUS"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! command -v chdman >/dev/null; then
    echo "chdman not found in PATH" >&2
    exit 1
fi

# Tiny raw payload: 64 KiB of zeros + a few deterministic bytes.
# Small enough to fuzz fast, large enough to exercise multi-hunk paths.
RAW_HD="$TMP/tiny.img"
dd if=/dev/zero of="$RAW_HD" bs=512 count=128 status=none
printf 'LIBCHDR-FUZZ-CORPUS' | dd of="$RAW_HD" bs=1 seek=0 conv=notrunc status=none

# Minimal CUE+BIN (single audio track, 1 second = 75 frames of 2352 B).
CUE="$TMP/tiny.cue"
BIN="$TMP/tiny.bin"
dd if=/dev/urandom of="$BIN" bs=2352 count=75 status=none
cat > "$CUE" <<EOF
FILE "tiny.bin" BINARY
  TRACK 01 AUDIO
    INDEX 01 00:00:00
EOF

# Raw for createraw (power-of-two, small).
RAW="$TMP/tiny.raw"
dd if=/dev/urandom of="$RAW" bs=4096 count=16 status=none

# Raw for a small-hunk, many-hunk file - 2,200 hunks of 64 B (140,800 B
# total) specifically to cross LOWRAM_TARGET_CHECKPOINT_STRIDE's default
# 2048-hunk checkpoint boundary. LOWRAM_TARGET's sequential resume-cache
# only exercises its bucket-boundary handling when a real checkpoint
# crossing happens mid-file - every other seed here is too small to ever
# hit one. See lowram-correctness.yml.
RAW_BOUNDARY="$TMP/boundary.raw"
dd if=/dev/urandom of="$RAW_BOUNDARY" bs=64 count=2200 status=none

create_hd () {
    local name="$1"; shift
    chdman createhd -f -o "$CORPUS/$name" -i "$RAW_HD" --chs 4,16,2 -ss 512 "$@" >/dev/null 2>&1 || true
}

create_cd () {
    local name="$1"; shift
    chdman createcd -f -o "$CORPUS/$name" -i "$CUE" "$@" >/dev/null 2>&1 || true
}

create_raw () {
    local name="$1"; shift
    chdman createraw -f -o "$CORPUS/$name" -i "$RAW" -hs 4096 -us 512 "$@" >/dev/null 2>&1 || true
}

create_raw_boundary () {
    local name="$1"; shift
    chdman createraw -f -o "$CORPUS/$name" -i "$RAW_BOUNDARY" -hs 64 -us 64 "$@" >/dev/null 2>&1 || true
}

# Hard disk: default codecs + flavor variants.
create_hd hd_default.chd
create_hd hd_none.chd       -c none
create_hd hd_zlib.chd       -c zlib
create_hd hd_lzma.chd       -c lzma
create_hd hd_huff.chd       -c huff
create_hd hd_zstd.chd       -c zstd
create_hd hd_multi.chd      -c zlib,lzma,huff,zstd

# CD-ROM: default + per-codec.
create_cd cd_default.chd
create_cd cd_none.chd       -c none
create_cd cd_cdzl.chd       -c cdzl
create_cd cd_cdlz.chd       -c cdlz
create_cd cd_cdfl.chd       -c cdfl
create_cd cd_cdzs.chd       -c cdzs

# Raw.
create_raw raw_default.chd
create_raw raw_none.chd     -c none
create_raw raw_zstd.chd     -c zstd

# Raw, many small hunks - crosses a LOWRAM_TARGET checkpoint boundary.
create_raw_boundary raw_boundary.chd -c zstd

# Summary.
echo "generated $(ls -1 "$CORPUS"/*.chd 2>/dev/null | wc -l) CHD samples:"
ls -lhS "$CORPUS"/*.chd 2>/dev/null | awk '{print "  " $5 "  " $NF}' | sed "s|$CORPUS/||"
