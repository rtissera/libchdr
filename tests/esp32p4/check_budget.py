#!/usr/bin/env python3
"""Enforce a per-codec RAM budget against tests/esp32p4/fw.c's qemu output.

Usage: check_budget.py <log-file>

Fails (exit 1) if any codec's decode failed (OPEN/READ FAILED - a
correctness regression, e.g. the kind the LZMA dict-size bug caused) or if
any codec's peak heap exceeds its budget (a memory regression). Budgets
leave headroom over the measured rv32imafc/ilp32f baseline; see the libchdr
issue/PR that added this check for the measured numbers.

Same ABI/toolchain/qemu machine as tests/rv32's identical check (ESP32-P4's
RISC-V cores share BL616's RV32IMAFC/ilp32f base ISA/ABI), so these numbers
are expected to match tests/rv32/check_budget.py's exactly - kept as a
separate file only so ESP32-P4 gets its own named CI job/budget, not because
a different measurement was taken.
"""
import re
import sys

# bytes; measured baseline (2026-08-31, rv32imafc/ilp32f, qemu-system-riscv32
# virt) rounded up with headroom for the ESP32-P4's 768KB HP L2MEM budget
# (more generous than BL616's 480KB SRAM allows, but left at the same
# absolute numbers as tests/rv32 - they're not close to either budget).
BUDGETS = {
	"hd_zlib": 100_000,
	"hd_zstd": 150_000,
	"hd_lzma": 80_000,
	"hd_huff": 200_000,
	"cd_cdzl": 200_000,
	"cd_cdzs": 320_000,
	"cd_cdlz": 200_000,
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
			budget = BUDGETS.get(name)
			if budget is None:
				print(f"WARN {name}: no budget defined, skipping (peak={peak})")
				continue
			status = "ok" if peak <= budget else "OVER BUDGET"
			print(f"{status:12s} {name:10s} peak={peak:>7d} budget={budget:>7d}")
			if peak > budget:
				failed = True

	missing = set(BUDGETS) - set(seen)
	if missing:
		print(f"FAIL: expected codecs missing from output: {sorted(missing)}")
		failed = True

	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
