/* license:BSD-3-Clause
 * copyright-holders:Romain Tisserand
 *
 * The CD track metadata formats carry %s fields, and the metadata string is
 * whatever the CHD says it is. Scanned with the unbounded *_FORMAT macros an
 * over-long TYPE writes past the caller's buffer (#165); the *_SCANF variants
 * stop at CD_METADATA_STRING_SIZE - 1 characters. Needs no corpus.
 */
#include <stdio.h>
#include <string.h>

#include <libchdr/chd.h>

struct guarded
{
	char str[CD_METADATA_STRING_SIZE];
	char canary[16];
};

static int failures;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void fill(struct guarded *g)
{
	memset(g->str, 0, sizeof(g->str));
	memset(g->canary, 0x5a, sizeof(g->canary));
}

static int intact(const struct guarded *g)
{
	size_t i;
	for (i = 0; i < sizeof(g->canary); i++)
		if ((unsigned char)g->canary[i] != 0x5a)
			return 0;
	return 1;
}

int main(void)
{
	struct guarded type, subtype, pgtype, pgsub;
	int track = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;

	/* a well-formed CHT2 entry parses in full */
	fill(&type); fill(&subtype); fill(&pgtype); fill(&pgsub);
	check(sscanf("TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:1500 PREGAP:150 PGTYPE:VAUDIO PGSUB:RW POSTGAP:0",
	             CDROM_TRACK_METADATA2_SCANF, &track, type.str, subtype.str, &frames,
	             &pregap, pgtype.str, pgsub.str, &postgap) == 8, "CHT2 field count");
	check(track == 2 && frames == 1500 && pregap == 150 && postgap == 0, "CHT2 numbers");
	check(strcmp(type.str, "AUDIO") == 0 && strcmp(pgtype.str, "VAUDIO") == 0, "CHT2 strings");

	/* the longest real value, MODE2_FORM_MIX, still fits */
	fill(&type); fill(&subtype);
	check(sscanf("TRACK:1 TYPE:MODE2_FORM_MIX SUBTYPE:RW_RAW FRAMES:10",
	             CDROM_TRACK_METADATA_SCANF, &track, type.str, subtype.str, &frames) == 4, "CHTR field count");
	check(strcmp(type.str, "MODE2_FORM_MIX") == 0 && frames == 10, "CHTR longest type");

	/* an over-long TYPE stops at the buffer, and the rest of the line no
	 * longer matches - so the field count tells the caller to reject it */
	fill(&type); fill(&subtype);
	check(sscanf("TRACK:1 TYPE:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA SUBTYPE:NONE FRAMES:10",
	             CDROM_TRACK_METADATA_SCANF, &track, type.str, subtype.str, &frames) == 2, "overlong TYPE rejected");
	check(strlen(type.str) == CD_METADATA_STRING_SIZE - 1, "overlong TYPE truncated");
	check(intact(&type) && intact(&subtype), "CHTR canary");

	/* every string field of the GD-ROM format is bounded too */
	fill(&type); fill(&subtype); fill(&pgtype); fill(&pgsub);
	sscanf("TRACK:3 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:1 PAD:0 PREGAP:0 PGTYPE:MODE1 "
	       "PGSUB:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB POSTGAP:0",
	       GDROM_TRACK_METADATA_SCANF, &track, type.str, subtype.str, &frames,
	       &pad, &pregap, pgtype.str, pgsub.str, &postgap);
	check(intact(&type) && intact(&subtype) && intact(&pgtype) && intact(&pgsub), "CHGD canary");

	if (failures == 0)
		printf("metadata scanf bounds: ok\n");
	return failures ? 1 : 0;
}
