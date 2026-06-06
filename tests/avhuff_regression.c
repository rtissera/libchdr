/*
 * AVHuff regression harness for libchdr.
 *
 * Decode every hunk of a CHD via libchdr. Built-in CRC16 verification
 * (VERIFY_BLOCK_CRC=1) catches any byte-level decode error: a mismatched
 * decoded hunk fails CRC and chd_read returns CHDERR_DECOMPRESSION_ERROR.
 *
 * Usage: avhuff_regression <chd-file> [<chd-file> ...]
 * Exit status: 0 = all hunks decode + CRC clean; 1 = any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libchdr/chd.h"

static int run_one(const char *path)
{
	chd_file *chd = NULL;
	const chd_header *hdr;
	uint8_t *buf;
	uint32_t i;
	chd_error err;

	err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE) {
		fprintf(stderr, "[FAIL] open %s: %s\n", path, chd_error_string(err));
		return 1;
	}

	hdr = chd_get_header(chd);
	if (hdr == NULL || hdr->hunkbytes == 0) {
		fprintf(stderr, "[FAIL] %s: no header\n", path);
		chd_close(chd);
		return 1;
	}

	printf("[INFO] %s: v%u, %u hunks of %u bytes, codecs=[0x%08x 0x%08x 0x%08x 0x%08x]\n",
	       path, hdr->version, hdr->totalhunks, hdr->hunkbytes,
	       hdr->compression[0], hdr->compression[1],
	       hdr->compression[2], hdr->compression[3]);

	buf = (uint8_t *)malloc(hdr->hunkbytes);
	if (buf == NULL) {
		fprintf(stderr, "[FAIL] OOM\n");
		chd_close(chd);
		return 1;
	}

	for (i = 0; i < hdr->totalhunks; i++) {
		err = chd_read(chd, i, buf);
		if (err != CHDERR_NONE) {
			fprintf(stderr, "[FAIL] %s: hunk %u: %s\n",
			        path, i, chd_error_string(err));
			free(buf);
			chd_close(chd);
			return 1;
		}
	}

	printf("[PASS] %s: %u/%u hunks decoded + CRC verified\n",
	       path, hdr->totalhunks, hdr->totalhunks);
	free(buf);
	chd_close(chd);
	return 0;
}

int main(int argc, char **argv)
{
	int rc = 0;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <chd> [<chd> ...]\n", argv[0]);
		return 2;
	}

	for (i = 1; i < argc; i++)
		rc |= run_one(argv[i]);

	return rc;
}
