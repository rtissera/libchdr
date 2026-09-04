/* Reads a CHD's hunks in a given order (sequential, reverse, or a random
 * permutation seeded from argv) and writes them concatenated to stdout, so
 * two builds (e.g. LOWRAM_MAP=0 vs =1) can be diffed byte-for-byte - not
 * just relying on the internal CRC check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <chd> <sequential|reverse|random:SEED> [max_hunks]\n", argv[0]);
		return 2;
	}

	chd_file *chd = NULL;
	chd_error err = chd_open(argv[1], CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE) {
		fprintf(stderr, "chd_open: %s\n", chd_error_string(err));
		return 1;
	}

	const chd_header *h = chd_get_header(chd);
	uint32_t n = h->totalhunks;
	uint32_t *order;

	if (strncmp(argv[2], "sample:", 7) == 0) {
		/* scattered sample across the FULL [0,totalhunks) range - for large
		 * real-world files, exercises many checkpoints without decoding the
		 * whole (possibly multi-GB) file. format: sample:COUNT:SEED */
		char *rest = argv[2] + 7;
		uint32_t count = (uint32_t)strtoul(rest, &rest, 10);
		unsigned seed = (rest[0] == ':') ? (unsigned)strtoul(rest + 1, NULL, 10) : 0;
		if (count > n) count = n;
		srand(seed);
		order = malloc(sizeof(uint32_t) * count);
		for (uint32_t i = 0; i < count; i++)
			order[i] = (uint32_t)((double)rand() / ((double)RAND_MAX + 1) * n);
		n = count;
	} else {
		if (argc >= 4) {
			uint32_t cap = (uint32_t)strtoul(argv[3], NULL, 10);
			if (cap < n) n = cap;
		}
		order = malloc(sizeof(uint32_t) * n);
		for (uint32_t i = 0; i < n; i++) order[i] = i;

		if (strncmp(argv[2], "reverse", 7) == 0) {
			for (uint32_t i = 0; i < n; i++) order[i] = n - 1 - i;
		} else if (strncmp(argv[2], "random:", 7) == 0) {
			unsigned seed = (unsigned)strtoul(argv[2] + 7, NULL, 10);
			srand(seed);
			for (uint32_t i = n; i > 1; i--) {
				uint32_t j = rand() % i;
				uint32_t tmp = order[i - 1];
				order[i - 1] = order[j];
				order[j] = tmp;
			}
		}
		/* else: sequential, already set up */
	}

	uint8_t *buf = malloc(h->hunkbytes);
	for (uint32_t i = 0; i < n; i++) {
		err = chd_read(chd, order[i], buf);
		if (err != CHDERR_NONE) {
			fprintf(stderr, "chd_read(hunk %u) failed: %s\n", order[i], chd_error_string(err));
			return 1;
		}
		/* prefix each hunk with its logical index so the diff pinpoints which
		 * hunk mismatched, regardless of read order */
		{
			/* Write the index as four explicit bytes rather than a raw
			 * uint32_t: a host-endian write makes this dump differ between
			 * big- and little-endian builds even when the decoded data is
			 * identical, which would defeat cross-endian comparison. */
			const uint8_t idx[4] = {
				(uint8_t)(order[i] >> 24), (uint8_t)(order[i] >> 16),
				(uint8_t)(order[i] >> 8),  (uint8_t)order[i]
			};
			fwrite(idx, 1, sizeof(idx), stdout);
		}
		fwrite(buf, 1, h->hunkbytes, stdout);
	}

	free(buf);
	free(order);
	chd_close(chd);
	return 0;
}
