#!/usr/bin/env bash
# Generates C byte-array headers (main/embed/*.h) from the FULL CHD corpus
# (tests/corpus/seeds/*.chd + tests/avhuff_corpus/{regtest,synth}), for
# embedding into the real-hardware ESP32-P4 throughput benchmark
# (main/benchmark_main.c). Not committed - regenerated on demand, same
# convention as tests/esp32p4/gen_embed.sh (which only embeds a curated
# 7-file subset for the RAM-budget probe; this one embeds everything, since
# the point here is measuring real decode throughput across the whole
# codec/order corpus, not just peak heap per codec).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUT_DIR="$SCRIPT_DIR/main/embed"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.h

emit() {
	local src="$1" name="$2"
	xxd -i "$src" | sed \
		-e "s/unsigned char [A-Za-z0-9_]*\[\]/const unsigned char ${name}_chd[]/" \
		-e "s/unsigned int [A-Za-z0-9_]*_len/const unsigned int ${name}_chd_len/" \
		> "$OUT_DIR/${name}.h"
}

count=0
list_file="$OUT_DIR/../embed_list.inc"
: > "$list_file"

for src in "$REPO_ROOT"/tests/corpus/seeds/*.chd; do
	name="$(basename "$src" .chd)"
	emit "$src" "$name"
	echo "{ \"$name\", ${name}_chd, ${name}_chd_len }," >> "$list_file"
	count=$((count + 1))
done

for src in "$REPO_ROOT"/tests/avhuff_corpus/regtest/*/out.chd; do
	name="avhu_$(basename "$(dirname "$src")")"
	emit "$src" "$name"
	echo "{ \"$name\", ${name}_chd, ${name}_chd_len }," >> "$list_file"
	count=$((count + 1))
done

for src in "$REPO_ROOT"/tests/avhuff_corpus/synth/*.chd; do
	name="avhu_$(basename "$src" .chd)"
	emit "$src" "$name"
	echo "{ \"$name\", ${name}_chd, ${name}_chd_len }," >> "$list_file"
	count=$((count + 1))
done

# A handful of real, small MAME-CD-derived CHDs from an actual game library
# (/userdata/roms), not just the synthetic test corpus - flash-sized (<2MB
# each) so they fit without a custom partition table. Optional: skipped if
# REAL_CHD_DIR isn't set or the files aren't there, so this script still
# works standalone for anyone without that ROM directory.
REAL_CHD_DIR="${REAL_CHD_DIR:-/userdata/roms/pcenginecd}"
REAL_CHDS=(
	"Pyramid Plunder (USA) (Unl).cue.chd"
	"Hawiian Island Girls (USA) (Unl).cue.chd"
	"Local Girls of Hawaii, The (USA) (Unl).cue.chd"
)
for fname in "${REAL_CHDS[@]}"; do
	src="$REAL_CHD_DIR/$fname"
	if [ ! -f "$src" ]; then
		echo "gen_embed.sh: skipping missing real CHD: $src" >&2
		continue
	fi
	name="real_$(basename "$fname" .chd)"
	name="$(echo "$name" | tr -c 'A-Za-z0-9_' '_')"
	emit "$src" "$name"
	echo "{ \"$name\", ${name}_chd, ${name}_chd_len }," >> "$list_file"
	count=$((count + 1))
done

# one #include line per header, generated alongside the entry list so
# benchmark_main.c doesn't need to be edited when the corpus changes
inc_file="$OUT_DIR/../embed_includes.inc"
: > "$inc_file"
for h in "$OUT_DIR"/*.h; do
	echo "#include \"embed/$(basename "$h")\"" >> "$inc_file"
done

echo "Generated $count embedded CHD headers in $OUT_DIR"
