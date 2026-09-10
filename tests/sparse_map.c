/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * A CHDv5 map is run-length and Huffman coded, so a run of identical hunks
 * costs far less than one bit each: 64 MiB of zeros compresses to a couple of
 * hundred bytes carrying 16384 hunks. Header sanity checks that bound
 * totalhunks by the file's own size in bits therefore reject perfectly valid
 * sparse images - issue #190, where a 50 GB image of ~23 KB was refused.
 *
 * Skips when the corpus is absent, as the other corpus tests do.
 */
#include <stdio.h>
#include <stdlib.h>

#include <libchdr/chd.h>

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "tests/corpus/seeds/rle_sparse.chd";
	chd_file *chd = NULL;
	const chd_header *header;
	unsigned char *buf;
	chd_error err;
	uint32_t i;
	FILE *probe;

	probe = fopen(path, "rb");
	if (probe == NULL)
	{
		printf("sparse seed not generated (%s) - skipping\n", path);
		printf("run tests/corpus/generate.sh to build it\n");
		return 0;
	}
	fclose(probe);

	err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE)
	{
		fprintf(stderr, "open failed: %s\n", chd_error_string(err));
		fprintf(stderr, "a sparse v5 map holds more hunks than the file has bits;"
			" a header check must not assume otherwise\n");
		return 1;
	}

	header = chd_get_header(chd);
	printf("sparse map: %u hunks, %u hunkbytes\n",
		(unsigned)header->totalhunks, (unsigned)header->hunkbytes);

	buf = (unsigned char *)malloc(header->hunkbytes);
	if (buf == NULL)
	{
		chd_close(chd);
		return 1;
	}

	/* every hunk has to decode, not just the header parse */
	for (i = 0; i < header->totalhunks; i++)
	{
		err = chd_read(chd, i, buf);
		if (err != CHDERR_NONE)
		{
			fprintf(stderr, "hunk %u: %s\n", i, chd_error_string(err));
			free(buf);
			chd_close(chd);
			return 1;
		}
	}

	printf("all %u hunks decoded\n", (unsigned)header->totalhunks);
	free(buf);
	chd_close(chd);
	return 0;
}
