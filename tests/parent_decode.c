/* Decode coverage for CHDs that have a parent.
 *
 * Nothing in tests/corpus/seeds has one - a child cannot be opened without its
 * parent, and the workflows that walk seeds/ open every file there standalone -
 * so COMPRESSION_PARENT is otherwise never exercised by any test in this tree.
 * That matters most for CHDR_CD_SCRATCH_BUFFER=0, where a hunk is decoded
 * straight into the caller's buffer and spread in place: a parent-referenced
 * hunk takes a different route to that same buffer, through a recursive
 * chd_read on the parent handle.
 *
 * tests/corpus/generate.sh builds two children off one parent:
 *   delta.chd  identical content, so every hunk is a parent reference
 *   mixed.chd  a few regions overwritten, so parent references and real
 *              compressed hunks share one map
 *
 * The corpus is generated on demand rather than committed, so a missing one is
 * a skip, not a failure.
 */

#include <libchdr/chd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a over bytes, folded with the hunk index so the result is sensitive to
 * a hunk landing in the wrong place, not just to the bytes present. */
static uint64_t hash_hunks(const char *path, chd_file *parent, uint32_t *hunks)
{
	chd_file *chd = NULL;
	const chd_header *header;
	uint8_t *buf;
	uint64_t acc = 0;
	uint32_t i;
	chd_error err;

	err = chd_open(path, CHD_OPEN_READ, parent, &chd);
	if (err != CHDERR_NONE) {
		printf("  FAIL  open %s: %s\n", path, chd_error_string(err));
		return 0;
	}
	header = chd_get_header(chd);
	buf = (uint8_t *)malloc(header->hunkbytes);
	if (buf == NULL) { chd_close(chd); return 0; }

	for (i = 0; i < header->totalhunks; i++) {
		size_t j;
		err = chd_read(chd, i, buf);
		if (err != CHDERR_NONE) {
			printf("  FAIL  %s hunk %u: %s\n", path, i, chd_error_string(err));
			free(buf);
			chd_close(chd);
			return 0;
		}
		for (j = 0; j < header->hunkbytes; j++) {
			acc ^= buf[j];
			acc *= 1099511628211ULL;
		}
		acc += (uint64_t)(i + 1);
	}
	*hunks = header->totalhunks;
	free(buf);
	chd_close(chd);   /* also closes the parent it was handed */
	return acc;
}

static int exists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (f == NULL) return 0;
	fclose(f);
	return 1;
}

int main(int argc, char **argv)
{
	char base[1024], delta[1024], mixed[1024];
	const char *dir = (argc > 1) ? argv[1] : "tests/corpus/parent";
	chd_file *p1 = NULL, *p2 = NULL;
	uint64_t h_base, h_delta, h_mixed;
	uint32_t n_base = 0, n_delta = 0, n_mixed = 0;
	int fail = 0;

	snprintf(base,  sizeof(base),  "%s/base.chd",  dir);
	snprintf(delta, sizeof(delta), "%s/delta.chd", dir);
	snprintf(mixed, sizeof(mixed), "%s/mixed.chd", dir);

	if (!exists(base) || !exists(delta)) {
		printf("parent corpus not generated (%s) - skipping\n", dir);
		printf("run tests/corpus/generate.sh to build it\n");
		return 0;
	}

	printf("parent/delta decode tests\n");

	h_base = hash_hunks(base, NULL, &n_base);
	if (h_base == 0) return 1;
	printf("  ok    parent                 %016llx  %u hunks\n",
	       (unsigned long long)h_base, n_base);

	/* every hunk of the delta is a parent reference, so it has to come out
	 * byte-identical to the parent */
	if (chd_open(base, CHD_OPEN_READ, NULL, &p1) != CHDERR_NONE)
		return 1;
	h_delta = hash_hunks(delta, p1, &n_delta);
	if (h_delta == h_base && n_delta == n_base) {
		printf("  ok    delta == parent        %016llx  %u hunks\n",
		       (unsigned long long)h_delta, n_delta);
	} else {
		printf("  FAIL  delta                  %016llx (%u hunks), want %016llx (%u)\n",
		       (unsigned long long)h_delta, n_delta,
		       (unsigned long long)h_base, n_base);
		fail = 1;
	}

	if (exists(mixed)) {
		if (chd_open(base, CHD_OPEN_READ, NULL, &p2) != CHDERR_NONE)
			return 1;
		h_mixed = hash_hunks(mixed, p2, &n_mixed);
		if (h_mixed == 0) {
			fail = 1;
		} else if (h_mixed == h_base) {
			printf("  FAIL  mixed decoded identical to the parent - the perturbed\n"
			       "        regions did not survive, so nothing was really tested\n");
			fail = 1;
		} else {
			printf("  ok    mixed != parent       %016llx  %u hunks\n",
			       (unsigned long long)h_mixed, n_mixed);
		}
	}

	if (fail) {
		printf("parent/delta decode FAILED\n");
		return 1;
	}
	printf("all parent/delta decode tests passed\n");
	return 0;
}
