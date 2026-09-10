/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * Who closes what when an open fails.
 *
 * The file and the parent handed to chd_open_core_file_callbacks() are
 * libchdr's from the call on, success or failure: a failed open has already
 * closed both. Callers are built on that - they retry a child that reported
 * CHDERR_REQUIRES_PARENT with a fresh file, and never close the parent they
 * passed in. A caller that closed the file again after a failure would close
 * it twice; one that kept using it would use a closed handle.
 *
 * The images are built in memory - a one-hunk uncompressed CHDv5, with or
 * without a parent SHA1 - so this runs without the generated corpus.
 *
 * chd_open() opens its FILE itself, so the test cannot count its closes.
 * On POSIX it checks instead that the descriptor came back: open() always
 * returns the lowest free one, so a FILE the failed open kept would push the
 * next open to a higher number.
 */
#if defined(__unix__) || defined(__APPLE__)
#define _POSIX_C_SOURCE 200112L   /* fileno() under a strict -std= */
#define HAVE_FILENO 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

#define HUNK 512
#define IMAGE (2 * HUNK)
#define SCRATCH_NAME "open_ownership_child.chd"

typedef struct {
	uint8_t data[IMAGE];
	size_t size;
	size_t pos;
	int closes;
} memfile;

static uint64_t mem_fsize(void *argp)
{
	return ((memfile *)argp)->size;
}

static size_t mem_fread(void *ptr, size_t size, size_t count, void *argp)
{
	memfile *f = (memfile *)argp;
	size_t want = size * count;
	if (f->closes != 0)
		return 0;   /* reading a closed file: the test below will notice */
	if (f->pos >= f->size)
		return 0;
	if (want > f->size - f->pos)
		want = f->size - f->pos;
	memcpy(ptr, f->data + f->pos, want);
	f->pos += want;
	return size ? want / size : 0;
}

static int mem_fclose(void *argp)
{
	((memfile *)argp)->closes++;
	return 0;
}

static int mem_fseek(void *argp, int64_t offset, int whence)
{
	memfile *f = (memfile *)argp;
	int64_t origin = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? (int64_t)f->pos : (int64_t)f->size;
	if (origin + offset < 0)
		return -1;
	f->pos = (size_t)(origin + offset);
	return 0;
}

static const core_file_callbacks mem_callbacks = {
	mem_fsize, mem_fread, mem_fclose, mem_fseek
};

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* One uncompressed hunk: 124-byte v5 header, a 4-byte map entry right after
 * it pointing at hunk slot 1, the data in that slot. sha1 names the image;
 * a nonzero parentsha1 makes it a child of the image with that sha1. */
static void make_image(memfile *f, uint8_t sha1, uint8_t parentsha1)
{
	uint8_t *h = f->data;
	memset(f, 0, sizeof(*f));
	memcpy(h, "MComprHD", 8);
	put_be32(h + 8, 124);       /* header length */
	put_be32(h + 12, 5);        /* version; compressors at 16-31 stay 0 */
	put_be32(h + 36, HUNK);     /* logicalbytes, low half */
	put_be32(h + 44, 124);      /* mapoffset, low half */
	put_be32(h + 56, HUNK);     /* hunkbytes */
	put_be32(h + 60, HUNK);     /* unitbytes */
	memset(h + 84, sha1, 20);
	memset(h + 104, parentsha1, 20);
	put_be32(h + 124, 1);       /* hunk 0 lives at 1 * HUNK */
	memset(h + HUNK, 0x5a, HUNK);
	f->size = IMAGE;
}

static int failures;

static void expect(int cond, const char *what)
{
	printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		failures++;
}

int main(void)
{
	static memfile base, other, child, garbage;
	uint32_t not_a_chd[64];
	chd_file *parent = NULL, *chd = NULL;
	FILE *scratch;
	uint8_t buf[HUNK];
	chd_error err;

	printf("success: the file stays open until chd_close()\n");
	make_image(&base, 0x11, 0);
	err = chd_open_core_file_callbacks(&mem_callbacks, &base, CHD_OPEN_READ, NULL, &chd);
	expect(err == CHDERR_NONE, "opens");
	if (err == CHDERR_NONE) {
		expect(base.closes == 0, "not closed while open");
		expect(chd_read(chd, 0, buf) == CHDERR_NONE && buf[0] == 0x5a && buf[HUNK - 1] == 0x5a,
			"hunk 0 reads back");
		chd_close(chd);
		expect(base.closes == 1, "closed once by chd_close()");
	}

	printf("bad header: closed by the failed open\n");
	make_image(&garbage, 0x11, 0);
	garbage.data[0] = 'X';
	err = chd_open_core_file_callbacks(&mem_callbacks, &garbage, CHD_OPEN_READ, NULL, &chd);
	expect(err != CHDERR_NONE, "fails");
	expect(garbage.closes == 1, "closed exactly once");

	printf("truncated file: closed by the failed open\n");
	make_image(&garbage, 0x11, 0);
	garbage.size = 60;
	err = chd_open_core_file_callbacks(&mem_callbacks, &garbage, CHD_OPEN_READ, NULL, &chd);
	expect(err != CHDERR_NONE, "fails");
	expect(garbage.closes == 1, "closed exactly once");

	printf("parent that is not a chd_file: the file is closed, the parent untouched\n");
	memset(not_a_chd, 0, sizeof(not_a_chd));
	make_image(&child, 0x22, 0x11);
	err = chd_open_core_file_callbacks(&mem_callbacks, &child, CHD_OPEN_READ,
		(chd_file *)(void *)not_a_chd, &chd);
	expect(err == CHDERR_INVALID_PARAMETER, "fails with CHDERR_INVALID_PARAMETER");
	expect(child.closes == 1, "file closed exactly once");

	printf("child without its parent: closed, retried with a fresh file\n");
	make_image(&child, 0x22, 0x11);
	err = chd_open_core_file_callbacks(&mem_callbacks, &child, CHD_OPEN_READ, NULL, &chd);
	expect(err == CHDERR_REQUIRES_PARENT, "fails with CHDERR_REQUIRES_PARENT");
	expect(child.closes == 1, "closed exactly once");

	make_image(&base, 0x11, 0);
	make_image(&child, 0x22, 0x11);
	err = chd_open_core_file_callbacks(&mem_callbacks, &base, CHD_OPEN_READ, NULL, &parent);
	expect(err == CHDERR_NONE, "parent opens");
	if (err == CHDERR_NONE) {
		err = chd_open_core_file_callbacks(&mem_callbacks, &child, CHD_OPEN_READ, parent, &chd);
		expect(err == CHDERR_NONE, "child opens with its parent");
		if (err == CHDERR_NONE) {
			expect(child.closes == 0 && base.closes == 0, "neither closed while open");
			chd_close(chd);
			expect(child.closes == 1, "child closed once by chd_close()");
			expect(base.closes == 1, "parent closed once along with it");
		}
	}

	printf("child with the wrong parent: both closed by the failed open\n");
	make_image(&other, 0x33, 0);
	make_image(&child, 0x22, 0x11);
	err = chd_open_core_file_callbacks(&mem_callbacks, &other, CHD_OPEN_READ, NULL, &parent);
	expect(err == CHDERR_NONE, "unrelated parent opens");
	if (err == CHDERR_NONE) {
		err = chd_open_core_file_callbacks(&mem_callbacks, &child, CHD_OPEN_READ, parent, &chd);
		expect(err == CHDERR_INVALID_PARENT, "fails with CHDERR_INVALID_PARENT");
		expect(child.closes == 1, "child closed exactly once");
		expect(other.closes == 1, "parent closed exactly once");
	}

	printf("chd_open() with a parent that is not a chd_file: its own FILE closed\n");
	make_image(&child, 0x22, 0x11);
	scratch = fopen(SCRATCH_NAME, "wb");
	if (scratch == NULL) {
		printf("  skip  cannot write %s in the working directory\n", SCRATCH_NAME);
	} else {
		int written = fwrite(child.data, 1, child.size, scratch) == child.size;
#if HAVE_FILENO
		int fd_before = fileno(scratch);
#endif
		written &= fclose(scratch) == 0;
		expect(written, "scratch image written");
		err = chd_open(SCRATCH_NAME, CHD_OPEN_READ, (chd_file *)(void *)not_a_chd, &chd);
		expect(err == CHDERR_INVALID_PARAMETER, "fails with CHDERR_INVALID_PARAMETER");
#if HAVE_FILENO
		scratch = fopen(SCRATCH_NAME, "rb");
		expect(scratch != NULL && fileno(scratch) == fd_before, "its descriptor was released");
		if (scratch != NULL)
			fclose(scratch);
#endif
		remove(SCRATCH_NAME);
	}

	printf("no callbacks: rejected, nothing to close with\n");
	err = chd_open_core_file_callbacks(NULL, &base, CHD_OPEN_READ, NULL, &chd);
	expect(err == CHDERR_INVALID_PARAMETER, "fails with CHDERR_INVALID_PARAMETER");

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
