#!/usr/bin/env python3
# Generate CHDv3/v4 fixtures. chdman only writes v5, so these are built here
# from the format itself; every file is accepted by MAME's own reader, and the
# deliberately-broken ones are rejected by it.
#
# Layouts follow MAME's chd.cpp (parse_v3_header / parse_v4_header) and its
# 16-byte V34 map entry: offset, CRC32 of the decoded hunk, length, flags.
# libchdr additionally requires the end-of-list cookie MAME writes after the
# last entry.
#
# The pairs generated here differ only in that a hunk's stored data no longer
# matches the CRC32 the map advertises: it still decompresses cleanly, so the
# map CRC is the only thing that can tell. One file sets the per-entry
# "no CRC" flag on that same hunk, which must make it readable again.

import hashlib
import os
import struct
import sys
import zlib

V3_HEADER_SIZE = 120
V4_HEADER_SIZE = 108
MAP_ENTRY_SIZE = 16

TYPE_COMPRESSED = 1
TYPE_UNCOMPRESSED = 2
FLAG_NO_CRC = 0x10

HD_META_TAG = b'GDDD'
META_FLAG_CHECKSUM = 0x01


def build(path, version, hunkbytes, hunks, payload, crc_payload=None,
          nocrc_hunks=(), sector=512):
	"""Write one CHD. crc_payload, when given, is what the map claims."""
	assert version in (3, 4)
	assert len(payload) == hunkbytes * hunks
	if crc_payload is None:
		crc_payload = payload

	hdr_size = V3_HEADER_SIZE if version == 3 else V4_HEADER_SIZE
	map_off = hdr_size
	data_off = map_off + MAP_ENTRY_SIZE * (hunks + 1)   # + end-of-list cookie

	cylinders = max(1, (hunkbytes * hunks) // (sector * 16 * 32))
	meta = b'CYLS:%d,HEADS:16,SECS:32,BPS:%d\x00' % (cylinders, sector)

	entries, blob, off = [], bytearray(), data_off
	for i in range(hunks):
		raw = payload[i * hunkbytes:(i + 1) * hunkbytes]
		comp = zlib.compress(raw, 9)[2:-4]              # raw deflate
		if len(comp) < hunkbytes:
			etype, data = TYPE_COMPRESSED, comp
		else:
			etype, data = TYPE_UNCOMPRESSED, raw
		flags = etype | (FLAG_NO_CRC if i in nocrc_hunks else 0)
		claimed = crc_payload[i * hunkbytes:(i + 1) * hunkbytes]
		crc = 0 if i in nocrc_hunks else zlib.crc32(claimed) & 0xffffffff
		entries.append((off, crc, len(data), flags))
		blob += data
		off += len(data)

	rawmap = bytearray()
	for offset, crc, length, flags in entries:
		rawmap += struct.pack('>QIHBB', offset, crc, length & 0xffff,
		                      (length >> 16) & 0xff, flags)

	rawsha1 = hashlib.sha1(payload).digest()
	# compute_overall_sha1(): sha1(rawsha1 || tag || sha1(metadata))
	overall = hashlib.sha1(rawsha1 + HD_META_TAG
	                       + hashlib.sha1(meta).digest()).digest()

	h = bytearray(hdr_size)
	h[0:8] = b'MComprHD'
	h[8:12] = struct.pack('>I', hdr_size)
	h[12:16] = struct.pack('>I', version)
	h[16:20] = struct.pack('>I', 0)          # no parent, writable
	h[20:24] = struct.pack('>I', 1)          # zlib
	h[24:28] = struct.pack('>I', hunks)
	h[28:36] = struct.pack('>Q', hunkbytes * hunks)
	h[36:44] = struct.pack('>Q', off)        # metadata follows the hunks
	if version == 4:
		h[44:48] = struct.pack('>I', hunkbytes)
		h[48:68] = overall
		h[88:108] = rawsha1
	else:
		h[76:80] = struct.pack('>I', hunkbytes)
		h[80:100] = rawsha1                  # v3 keeps only the raw hash

	meta_blk = struct.pack('>4sIQ', HD_META_TAG,
	                       (META_FLAG_CHECKSUM << 24) | len(meta), 0) + meta

	with open(path, 'wb') as f:
		f.write(h)
		f.write(rawmap)
		f.write(b'EndOfListCookie\0')
		f.write(blob)
		f.write(meta_blk)


def main(outdir):
	hunkbytes, hunks = 4096, 12
	os.makedirs(outdir, exist_ok=True)

	# Every fourth hunk is incompressible, so both COMPRESSED and
	# UNCOMPRESSED entries are exercised.
	rnd = os.urandom(hunkbytes * hunks)
	payload = bytearray()
	for i in range(hunks):
		if i % 4 == 3:
			payload += rnd[i * hunkbytes:(i + 1) * hunkbytes]
		else:
			payload += bytes(((i * 7 + j // 13) & 0xff) for j in range(hunkbytes))
	payload = bytes(payload)

	def flip(buf, hunk, off=17):
		b = bytearray(buf)
		b[hunk * hunkbytes + off] ^= 0x40
		return bytes(b)

	bad_comp = flip(payload, 5)     # a COMPRESSED hunk
	bad_raw = flip(payload, 7)      # an UNCOMPRESSED one

	with open(os.path.join(outdir, 'plain.raw'), 'wb') as f:
		f.write(payload)
	for v in (3, 4):
		p = lambda n: os.path.join(outdir, 'v%d_%s.chd' % (v, n))
		build(p('plain'), v, hunkbytes, hunks, payload)
		build(p('badcomp'), v, hunkbytes, hunks, bad_comp, crc_payload=payload)
		build(p('badraw'), v, hunkbytes, hunks, bad_raw, crc_payload=payload)
		build(p('nocrc'), v, hunkbytes, hunks, bad_comp, crc_payload=payload,
		      nocrc_hunks=(5,))


if __name__ == '__main__':
	main(sys.argv[1] if len(sys.argv) > 1 else '.')
