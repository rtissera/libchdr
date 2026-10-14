/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * chd_cd_open() / chd_cd_read_sector() against the three-track CD that
 * tests/corpus/generate.sh builds (#155). Every LBA is read back and compared
 * with the bytes its cue sheet puts there, so the pregap that is stored, the
 * one that is not, and the per-track frame padding are all crossed.
 *
 * cuesheet.chd adds the cue sheet as CUES metadata (#164): its CATALOG, ISRC
 * and INDEX 02 must come through, and cuesheet_bad.chd's mismatched cue must
 * be ignored as a whole.
 *
 * Skips when the corpus is absent, as the other corpus tests do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#define BIN_FRAMES 776

static int failures;

static void check(int ok, const char *what, long value)
{
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s (%ld)\n", what, value);
		failures++;
	}
}

/* the .bin sector the cue sheet places at lba, -1 for an unstored pregap */
static long bin_sector(uint32_t lba)
{
	if (lba < 675)
		return lba;
	if (lba < 825)
		return -1;
	return lba - 150;
}

static chd_cd *open_cd(const char *dir, const char *name, chd_file **chd)
{
	char path[1024];
	chd_cd *cd = NULL;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (chd_open(path, CHD_OPEN_READ, NULL, chd) != CHDERR_NONE)
	{
		check(0, name, 0);
		return NULL;
	}
	check(chd_cd_open(*chd, &cd) == CHDERR_NONE, "chd_cd_open with a cue sheet", 0);
	return cd;
}

static void check_cuesheet(const char *dir)
{
	chd_file *chd = NULL;
	chd_cd *cd;
	uint32_t lba = 0;

	if ((cd = open_cd(dir, "cuesheet.chd", &chd)) != NULL)
	{
		const chd_cd_toc *toc = chd_cd_get_toc(cd);
		check(strcmp(toc->catalog, "0123456789012") == 0, "catalog", 0);
		check(toc->tracks[0].isrc[0] == 0 && strcmp(toc->tracks[1].isrc, "USRC17607839") == 0, "isrc", 0);
		check(chd_cd_get_index(cd, 2, 2, &lba) == CHDERR_NONE && lba == 525, "track 2 INDEX 02", (long)lba);
		check(chd_cd_get_index(cd, 2, 0, &lba) == CHDERR_NONE && lba == 300, "track 2 INDEX 00", (long)lba);
		check(chd_cd_get_index(cd, 3, 1, &lba) == CHDERR_NONE && lba == 825, "track 3 INDEX 01", (long)lba);
		check(chd_cd_get_index(cd, 3, 0, &lba) == CHDERR_NONE && lba == 675, "track 3 unstored INDEX 00", (long)lba);
		check(chd_cd_get_index(cd, 1, 2, &lba) == CHDERR_METADATA_NOT_FOUND, "track 1 INDEX 02", 0);
		check(chd_cd_get_index(cd, 4, 1, &lba) == CHDERR_INVALID_PARAMETER, "track past the last", 0);
		chd_cd_close(cd);
	}
	if (chd != NULL)
		chd_close(chd);

	chd = NULL;
	if ((cd = open_cd(dir, "cuesheet_bad.chd", &chd)) != NULL)
	{
		const chd_cd_toc *toc = chd_cd_get_toc(cd);
		check(toc->catalog[0] == 0 && toc->tracks[1].isrc[0] == 0, "mismatched cue ignored", 0);
		check(chd_cd_get_index(cd, 2, 2, &lba) == CHDERR_METADATA_NOT_FOUND, "mismatched cue INDEX 02", 0);
		check(chd_cd_get_index(cd, 2, 1, &lba) == CHDERR_NONE && lba == 450, "metadata INDEX 01 still known", (long)lba);
		chd_cd_close(cd);
	}
	if (chd != NULL)
		chd_close(chd);
}

int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "tests/corpus/cd";
	char path[1024];
	unsigned char *bin, sector[CD_MAX_SECTOR_DATA], zeros[CD_MAX_SECTOR_DATA];
	const chd_cd_toc *toc;
	chd_file *chd = NULL;
	chd_cd *cd = NULL;
	chd_error err;
	uint32_t lba;
	FILE *f;

	snprintf(path, sizeof(path), "%s/multitrack.bin", dir);
	f = fopen(path, "rb");
	if (f == NULL)
	{
		printf("multi-track CD not generated (%s) - skipping\n", path);
		printf("run tests/corpus/generate.sh to build it\n");
		return 0;
	}
	bin = (unsigned char *)malloc((size_t)BIN_FRAMES * CD_MAX_SECTOR_DATA);
	if (bin == NULL || fread(bin, CD_MAX_SECTOR_DATA, BIN_FRAMES, f) != BIN_FRAMES)
	{
		fprintf(stderr, "short read on %s\n", path);
		return 1;
	}
	fclose(f);

	snprintf(path, sizeof(path), "%s/multitrack.chd", dir);
	err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE)
	{
		fprintf(stderr, "open %s: %s\n", path, chd_error_string(err));
		return 1;
	}
	err = chd_cd_open(chd, &cd);
	if (err != CHDERR_NONE)
	{
		fprintf(stderr, "chd_cd_open: %s\n", chd_error_string(err));
		return 1;
	}

	toc = chd_cd_get_toc(cd);
	check(toc->numtracks == 3, "numtracks", toc->numtracks);
	check(toc->leadout == 926, "leadout", toc->leadout);
	if (toc->numtracks == 3)
	{
		check(toc->tracks[0].type == CD_TRACK_MODE1_RAW && toc->tracks[0].frames == 300, "track 1 type/frames", toc->tracks[0].frames);
		check(toc->tracks[0].logframeofs == 0 && toc->tracks[0].chdframeofs == 0, "track 1 offsets", toc->tracks[0].logframeofs);
		check(toc->tracks[1].type == CD_TRACK_AUDIO && toc->tracks[1].frames == 375, "track 2 type/frames", toc->tracks[1].frames);
		check(toc->tracks[1].pregap == 150 && toc->tracks[1].pgdatasize != 0, "track 2 stored pregap", toc->tracks[1].pregap);
		check(toc->tracks[1].logframeofs == 450 && toc->tracks[1].chdframeofs == 300, "track 2 offsets", toc->tracks[1].logframeofs);
		check(toc->tracks[2].frames == 101 && toc->tracks[2].pregap == 150 && toc->tracks[2].pgdatasize == 0, "track 3 unstored pregap", toc->tracks[2].frames);
		check(toc->tracks[2].logframeofs == 825 && toc->tracks[2].chdframeofs == 676, "track 3 offsets (padding)", toc->tracks[2].chdframeofs);
	}

	memset(zeros, 0, sizeof(zeros));
	for (lba = 0; lba < 926; lba++)
	{
		long s = bin_sector(lba);
		const unsigned char *want = (s < 0) ? zeros : bin + (size_t)s * CD_MAX_SECTOR_DATA;

		err = chd_cd_read_sector(cd, lba, sector, CHD_CD_READ_RAW);
		check(err == CHDERR_NONE && memcmp(sector, want, CD_MAX_SECTOR_DATA) == 0, "raw sector matches bin", (long)lba);

		err = chd_cd_read_sector(cd, lba, sector, CHD_CD_READ_DATA);
		if (lba < 300)
			check(err == CHDERR_NONE && memcmp(sector, want + 16, 2048) == 0, "mode 1 user data", (long)lba);
		else
			check(err == CHDERR_INVALID_PARAMETER, "user data refused on audio", (long)lba);
	}

	check(chd_cd_read_sector(cd, 926, sector, CHD_CD_READ_RAW) == CHDERR_HUNK_OUT_OF_RANGE, "read past leadout", 926);
	check(chd_cd_read_sector(cd, 0, sector, CHD_CD_READ_SUBCODE) == CHDERR_NONE && memcmp(sector, zeros, CD_MAX_SUBCODE_DATA) == 0, "no subcode reads as zeros", 0);

	check(toc->catalog[0] == 0, "no cue sheet, no catalog", 0);
	chd_cd_close(cd);
	chd_close(chd);
	free(bin);

	check_cuesheet(dir);
	if (failures == 0)
		printf("cd toc: ok\n");
	return failures ? 1 : 0;
}
