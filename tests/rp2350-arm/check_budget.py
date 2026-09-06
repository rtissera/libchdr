#!/usr/bin/env python3
"""Enforce a per-codec RAM budget against tests/rp2350-arm/fw.c's qemu output.

Usage: check_budget.py <log-file>

Fails (exit 1) if any codec's decode failed (OPEN/READ FAILED - a
correctness regression, e.g. the kind the LZMA dict-size bug caused) or if
any codec's peak heap crosses either of two thresholds.

REGRESSION is about 10% over what the code uses today: it fails when a memory
win is given back, which a platform ceiling sized for headroom cannot see.
PLATFORM is what RP2350 can afford at all. They answer different questions -
"did we just lose ground" and "does it still fit" - and both have to hold.
"""
import re
import sys

# bytes; measured baseline (2026-08-31, cortex-m33 hard-float,
# qemu-system-arm mps2-an505) rounded up with headroom for the RP2350's
# 520KB SRAM budget.
# Regression thresholds: about 10% over the measured baseline, which is what
# fails the job when a memory win is silently given back. Measured 2026-09-06
# on all three targets - rv32imafc, cortex-m33 and hazard3 agree to within
# 8 bytes on every codec, so these are not architecture-specific and the
# headroom is for allocator and toolchain drift, not for design changes.
#
# Raise one deliberately, in the commit that spends the memory, and say why.
REGRESSION = {
	"hd_zlib":  22_000,   # measured 19_766
	"hd_zstd": 118_000,   # measured 107_205
	"hd_lzma":  31_000,   # measured 27_858
	"hd_huff":  26_000,   # measured 23_219
	"cd_cdzl":  55_000,   # measured 49_786
	"cd_cdzs": 142_000,   # measured 128_795
	"cd_cdlz":  64_000,   # measured 57_905
}

# Platform ceilings: what this target can actually afford, independent of what
# the code happens to use today. These answer "does it still fit", the
# thresholds above answer "did we just lose ground". Both have to hold.
PLATFORM = {
	"hd_zlib": 100_000,
	"hd_zstd": 200_000,
	"hd_lzma": 80_000,
	"hd_huff": 60_000,
	"cd_cdzl": 250_000,
	"cd_cdzs": 400_000,
	"cd_cdlz": 250_000,
}

FAIL_RE = re.compile(r"^(\S+)\s+(?:OPEN|READ) FAILED: (.*)$")
OK_RE = re.compile(r"^(\S+)\s+hunkbytes=(\d+)\s+hunks=(\d+)\s+peak_heap=(\d+) bytes$")


def main():
	if len(sys.argv) != 2:
		print(f"usage: {sys.argv[0]} <log-file>", file=sys.stderr)
		return 2

	with open(sys.argv[1]) as f:
		lines = f.readlines()

	seen = {}
	failed = False

	for line in lines:
		line = line.rstrip("\n")
		m = FAIL_RE.match(line)
		if m:
			name, why = m.groups()
			print(f"FAIL {name}: decode failed - {why}")
			failed = True
			continue
		m = OK_RE.match(line)
		if m:
			name, hunkbytes, hunks, peak = m.groups()
			peak = int(peak)
			seen[name] = peak
			plat = PLATFORM.get(name)
			reg = REGRESSION.get(name)
			if plat is None and reg is None:
				print(f"WARN {name}: no budget defined, skipping (peak={peak})")
				continue
			over_reg = reg is not None and peak > reg
			over_plat = plat is not None and peak > plat
			if over_plat:
				status = "OVER PLATFORM"
			elif over_reg:
				status = "REGRESSION"
			else:
				status = "ok"
			print(f"{status:14s} {name:10s} peak={peak:>7d} "
			      f"regression={reg if reg is not None else '-':>7} "
			      f"platform={plat if plat is not None else '-':>7}")
			if over_reg or over_plat:
				failed = True

	missing = (set(PLATFORM) | set(REGRESSION)) - set(seen)
	if missing:
		print(f"FAIL: expected codecs missing from output: {sorted(missing)}")
		failed = True

	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
