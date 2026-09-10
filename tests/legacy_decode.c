/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * v1-v4 CHDs carry a CRC32 of each decoded hunk in their map entry, with a
 * per-entry flag to opt out. This walks the fixtures tests/corpus/mklegacy.py
 * builds: the intact pair must read and match the source data, the two whose
 * stored hunk no longer matches the CRC the map advertises must be refused,
 * and the one that sets the opt-out flag on that same hunk must read again.
 *
 * With VERIFY_BLOCK_CRC off nothing is checked, so the broken files are
 * expected to read - the point there is only that they still decode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

static unsigned char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (unsigned char *)malloc((size_t)n);
	if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* Reads every hunk. Returns the first error, or CHDERR_NONE, and compares
 * against expect when the whole file decoded. */
static chd_error decode_all(const char *path, const unsigned char *expect,
	size_t expectlen, int *mismatch)
{
	chd_file *chd = NULL;
	const chd_header *header;
	unsigned char *buf;
	chd_error err;
	uint32_t i;

	*mismatch = 0;
	err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE)
		return err;

	header = chd_get_header(chd);
	buf = (unsigned char *)malloc(header->hunkbytes);
	if (buf == NULL)
	{
		chd_close(chd);
		return CHDERR_OUT_OF_MEMORY;
	}

	for (i = 0; i < header->totalhunks; i++)
	{
		err = chd_read(chd, i, buf);
		if (err != CHDERR_NONE)
			break;
		if (expect != NULL && (size_t)(i + 1) * header->hunkbytes <= expectlen &&
			memcmp(buf, expect + (size_t)i * header->hunkbytes, header->hunkbytes) != 0)
			*mismatch = 1;
	}

	free(buf);
	chd_close(chd);
	return err;
}

int main(int argc, char **argv)
{
	static const char *const versions[] = { "v3", "v4" };
	unsigned char *raw;
	size_t rawlen;
	char path[1024];
	int failures = 0;
	size_t v;

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <corpus dir>\n", argv[0]);
		return 2;
	}

	snprintf(path, sizeof(path), "%s/plain.raw", argv[1]);
	raw = slurp(path, &rawlen);
	if (raw == NULL)
	{
		printf("legacy corpus not generated (%s) - skipping\n", argv[1]);
		printf("run tests/corpus/mklegacy.py to build it\n");
		return 0;
	}

	for (v = 0; v < sizeof(versions) / sizeof(versions[0]); v++)
	{
		struct { const char *name; chd_error want; int check; } cases[] = {
			{ "plain",   CHDERR_NONE, 1 },
#if VERIFY_BLOCK_CRC
			{ "badcomp", CHDERR_DECOMPRESSION_ERROR, 0 },
			{ "badraw",  CHDERR_DECOMPRESSION_ERROR, 0 },
#else
			{ "badcomp", CHDERR_NONE, 0 },
			{ "badraw",  CHDERR_NONE, 0 },
#endif
			{ "nocrc",   CHDERR_NONE, 0 },
		};
		size_t c;

		for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		{
			chd_error err;
			int mismatch;

			snprintf(path, sizeof(path), "%s/%s_%s.chd", argv[1],
				versions[v], cases[c].name);
			err = decode_all(path, cases[c].check ? raw : NULL, rawlen, &mismatch);
			if (err != cases[c].want)
			{
				fprintf(stderr, "%s_%s: got \"%s\", want \"%s\"\n",
					versions[v], cases[c].name, chd_error_string(err),
					chd_error_string(cases[c].want));
				failures++;
			}
			else if (mismatch)
			{
				fprintf(stderr, "%s_%s: decoded data differs from the source\n",
					versions[v], cases[c].name);
				failures++;
			}
		}
	}

	free(raw);
	if (failures != 0)
		fprintf(stderr, "%d failure(s)\n", failures);
	else
		printf("legacy CRC32 checks ok\n");
	return failures != 0;
}
