/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles, Romain Tisserand
 *
 * CD sector access by LBA: the track metadata parsing and frame offset
 * arithmetic of MAME's cdrom_file (src/lib/util/cdrom.cpp), in C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/cdrom.h"

/* one INDEX line of an embedded cue sheet */
struct cue_index
{
	uint8_t		track;
	uint8_t		index;
	int32_t		ofs;			/* frames from the track's INDEX 01 */
};

struct _chd_cd
{
	chd_file *	chd;
	chd_cd_toc	toc;
	uint32_t	framesperhunk;
	uint32_t	cachedhunk;		/* hunk held in `hunk`, ~0 = none */
	uint8_t *	hunk;
	struct cue_index *indices;	/* NULL without an embedded cue sheet */
	uint32_t	numindices;
};

/* a cue sheet is text a few hundred bytes long; anything far larger is not
 * one */
#define CUESHEET_MAX_BYTES	65536
/* the shortest INDEX line, "INDEX 01 00:00:00", is 17 bytes */
#define CUESHEET_MAX_INDICES(len)	((len) / 17 + 1)

static const struct
{
	const char *name;
	uint32_t type;
	uint32_t datasize;
} track_types[] =
{
	{ "MODE1",          CD_TRACK_MODE1,          2048 },
	{ "MODE1/2048",     CD_TRACK_MODE1,          2048 },
	{ "MODE1_RAW",      CD_TRACK_MODE1_RAW,      2352 },
	{ "MODE1/2352",     CD_TRACK_MODE1_RAW,      2352 },
	{ "MODE2",          CD_TRACK_MODE2,          2336 },
	{ "MODE2/2336",     CD_TRACK_MODE2,          2336 },
	{ "MODE2_FORM1",    CD_TRACK_MODE2_FORM1,    2048 },
	{ "MODE2/2048",     CD_TRACK_MODE2_FORM1,    2048 },
	{ "MODE2_FORM2",    CD_TRACK_MODE2_FORM2,    2324 },
	{ "MODE2/2324",     CD_TRACK_MODE2_FORM2,    2324 },
	{ "MODE2_FORM_MIX", CD_TRACK_MODE2_FORM_MIX, 2336 },
	{ "MODE2_RAW",      CD_TRACK_MODE2_RAW,      2352 },
	{ "MODE2/2352",     CD_TRACK_MODE2_RAW,      2352 },
	{ "CDI/2352",       CD_TRACK_MODE2_RAW,      2352 },
	{ "AUDIO",          CD_TRACK_AUDIO,          2352 },
	{ "CDG",            CD_TRACK_AUDIO,          2352 }
};

/* 0 datasize = unknown type */
static void type_info(const char *name, uint32_t *type, uint32_t *datasize)
{
	size_t i;
	*type = CD_TRACK_MODE1;
	*datasize = 0;
	for (i = 0; i < sizeof(track_types) / sizeof(track_types[0]); i++)
		if (strcmp(name, track_types[i].name) == 0)
		{
			*type = track_types[i].type;
			*datasize = track_types[i].datasize;
			return;
		}
}

static void subtype_info(const char *name, uint32_t *subtype, uint32_t *subsize)
{
	*subtype = CD_SUB_NONE;
	*subsize = 0;
	if (strcmp(name, "RW") == 0)
		*subtype = CD_SUB_NORMAL;
	else if (strcmp(name, "RW_RAW") == 0)
		*subtype = CD_SUB_RAW;
	if (*subtype != CD_SUB_NONE)
		*subsize = CD_MAX_SUBCODE_DATA;
}

/* NUL-terminated metadata entry; CHDERR_METADATA_NOT_FOUND past the last */
static chd_error get_text_metadata(chd_file *chd, uint32_t tag, uint32_t index, char *out, uint32_t outlen)
{
	uint32_t len = 0;
	chd_error err = chd_get_metadata(chd, tag, index, out, outlen - 1, &len, NULL, NULL);
	if (err == CHDERR_NONE)
		out[len < outlen - 1 ? len : outlen - 1] = 0;
	return err;
}

static chd_error parse_tracks(chd_file *chd, chd_cd_toc *toc)
{
	char meta[256];
	uint32_t i;

	for (i = 0; i < CD_MAX_TRACKS; i++)
	{
		chd_cd_track *track = &toc->tracks[i];
		char type[CD_METADATA_STRING_SIZE], subtype[CD_METADATA_STRING_SIZE];
		char pgtype[CD_METADATA_STRING_SIZE], pgsub[CD_METADATA_STRING_SIZE];
		int tracknum = -1, frames = 0, pregap = 0, postgap = 0, pad = 0;
		uint32_t pgtrktype;
		chd_error err;

		type[0] = subtype[0] = pgtype[0] = pgsub[0] = 0;

		err = get_text_metadata(chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta));
		if (err == CHDERR_NONE)
		{
			if (sscanf(meta, CDROM_TRACK_METADATA_SCANF, &tracknum, type, subtype, &frames) != 4)
				return CHDERR_INVALID_METADATA;
		}
		else if (err == CHDERR_METADATA_NOT_FOUND
		         && (err = get_text_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta))) == CHDERR_NONE)
		{
			if (sscanf(meta, CDROM_TRACK_METADATA2_SCANF, &tracknum, type, subtype, &frames,
			           &pregap, pgtype, pgsub, &postgap) != 8)
				return CHDERR_INVALID_METADATA;
		}
		else if (err == CHDERR_METADATA_NOT_FOUND)
		{
			err = get_text_metadata(chd, GDROM_OLD_METADATA_TAG, i, meta, sizeof(meta));
			if (err == CHDERR_NONE)
				toc->flags |= CD_FLAG_GDROMLE;
			else if (err == CHDERR_METADATA_NOT_FOUND)
				err = get_text_metadata(chd, GDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta));
			if (err == CHDERR_METADATA_NOT_FOUND)
				break;
			if (err != CHDERR_NONE)
				return err;
			if (sscanf(meta, GDROM_TRACK_METADATA_SCANF, &tracknum, type, subtype, &frames,
			           &pad, &pregap, pgtype, pgsub, &postgap) != 9)
				return CHDERR_INVALID_METADATA;
			toc->flags |= CD_FLAG_GDROM;
		}
		if (err != CHDERR_NONE)
			return err;

		/* chdman writes tracks in order; anything else is not a layout we
		 * can place frames for */
		if (tracknum != (int)i + 1 || frames < 0 || pregap < 0 || postgap < 0)
			return CHDERR_INVALID_METADATA;

		track->number = tracknum;
		type_info(type, &track->type, &track->datasize);
		if (track->datasize == 0)
			return CHDERR_INVALID_METADATA;
		subtype_info(subtype, &track->subtype, &track->subsize);
		track->frames = frames;
		track->pregap = pregap;
		track->postgap = postgap;
		track->pgdatasize = 0;
		/* a 'V' pregap type means its sectors are stored in the CHD */
		if (pregap > 0 && pgtype[0] == 'V')
			type_info(pgtype + 1, &pgtrktype, &track->pgdatasize);
		if (track->pgdatasize != 0 && track->pregap > track->frames)
			return CHDERR_INVALID_METADATA;
	}

	/* shortcut: the pre-v3 binary CHCD layout and multi-session CHSS tags are
	 * not read; add them if an image that needs them turns up */
	toc->numtracks = i;
	return i > 0 ? CHDERR_NONE : CHDERR_INVALID_METADATA;
}

/* MAME's cdrom_file constructor: CHD frames are padded per track, logical
 * LBAs skip pregaps that are not stored and count postgaps */
static chd_error compute_offsets(chd_cd_toc *toc)
{
	uint64_t chdofs = 0, logofs = 0;
	uint32_t i;

	for (i = 0; i < toc->numtracks; i++)
	{
		chd_cd_track *track = &toc->tracks[i];
		if (track->pgdatasize == 0)
			logofs += track->pregap;
		track->logframeofs = (uint32_t)logofs + (track->pgdatasize != 0 ? track->pregap : 0);
		track->chdframeofs = (uint32_t)chdofs;
		logofs += (uint64_t)track->frames + track->postgap;
		chdofs += ((uint64_t)track->frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;
		if (logofs > UINT32_MAX || chdofs > UINT32_MAX)
			return CHDERR_INVALID_METADATA;
	}
	toc->leadout = (uint32_t)logofs;
	return CHDERR_NONE;
}

/* Indices are kept relative to their track's INDEX 01, so they land on the
 * CHD's own track starts whatever the cue's FILE layout: one BIN, or one per
 * track as Redump has it. Returns 0 if the cue sheet is unusable. */
static int parse_cuesheet_text(chd_cd *cd, char *text, uint32_t maxindices)
{
	chd_cd_toc *toc = &cd->toc;
	uint32_t track = 0, first = 0, n = 0, i;
	int32_t index1 = -1;
	int file_since_index = 0, seen_index = 0;
	char *line = text;

	while (line != NULL)
	{
		char *next = strchr(line, '\n');
		char keyword[16];
		unsigned num, m, s, f;
		int used = 0;

		if (next != NULL)
			*next++ = 0;
		if (sscanf(line, " %15s %n", keyword, &used) != 1)
		{
			line = next;
			continue;
		}
		line += used;

		if (strcmp(keyword, "CATALOG") == 0)
			sscanf(line, "%13s", toc->catalog);
		else if (strcmp(keyword, "ISRC") == 0 && track > 0)
			sscanf(line, "%12s", toc->tracks[track - 1].isrc);
		else if (strcmp(keyword, "FILE") == 0)
			file_since_index = seen_index;
		else if (strcmp(keyword, "TRACK") == 0)
		{
			if (sscanf(line, "%u", &num) != 1 || num != track + 1 || num > toc->numtracks
			    || (track > 0 && index1 < 0))
				return 0;
			for (i = first; i < n; i++)
				cd->indices[i].ofs -= index1;
			track = num;
			first = n;
			index1 = -1;
			file_since_index = seen_index = 0;
		}
		else if (strcmp(keyword, "INDEX") == 0)
		{
			/* a FILE between two indices of one track puts them in
			 * different files, and their distance is unknown */
			if (track == 0 || file_since_index || n == maxindices
			    || sscanf(line, "%u %u:%u:%u", &num, &m, &s, &f) != 4
			    || num > 99 || m > 99 || s > 59 || f > 74)
				return 0;
			cd->indices[n].track = (uint8_t)track;
			cd->indices[n].index = (uint8_t)num;
			cd->indices[n].ofs = (int32_t)((m * 60 + s) * 75 + f);
			if (num == 1)
				index1 = cd->indices[n].ofs;
			n++;
			seen_index = 1;
		}
		line = next;
	}

	if (track != toc->numtracks || index1 < 0)
		return 0;
	for (i = first; i < n; i++)
		cd->indices[i].ofs -= index1;
	cd->numindices = n;
	return 1;
}

/* Optional: a missing or unusable cue sheet leaves the TOC as the track
 * metadata describes it. */
static void parse_cuesheet(chd_cd *cd)
{
	uint32_t len = 0, t;
	char dummy, *text;

	if (chd_get_metadata(cd->chd, CDROM_CUESHEET_METADATA_TAG, 0, &dummy, 0, &len, NULL, NULL) != CHDERR_NONE
	    || len == 0 || len > CUESHEET_MAX_BYTES)
		return;
	text = (char *)malloc(len + 1);
	cd->indices = (struct cue_index *)malloc(CUESHEET_MAX_INDICES(len) * sizeof(*cd->indices));
	if (text != NULL && cd->indices != NULL
	    && chd_get_metadata(cd->chd, CDROM_CUESHEET_METADATA_TAG, 0, text, len, NULL, NULL, NULL) == CHDERR_NONE)
	{
		text[len] = 0;
		if (parse_cuesheet_text(cd, text, CUESHEET_MAX_INDICES(len)))
		{
			free(text);
			return;
		}
	}
	free(text);
	free(cd->indices);
	cd->indices = NULL;
	cd->numindices = 0;
	cd->toc.catalog[0] = 0;
	for (t = 0; t < cd->toc.numtracks; t++)
		cd->toc.tracks[t].isrc[0] = 0;
}

CHD_EXPORT chd_error chd_cd_open(chd_file *chd, chd_cd **cd)
{
	const chd_header *header;
	chd_cd *result;
	chd_error err;

	if (chd == NULL || cd == NULL)
		return CHDERR_INVALID_PARAMETER;
	*cd = NULL;

	header = chd_get_header(chd);
	if (header->hunkbytes == 0 || header->hunkbytes % CD_FRAME_SIZE != 0)
		return CHDERR_INVALID_FILE;

	result = (chd_cd *)calloc(1, sizeof(*result));
	if (result == NULL)
		return CHDERR_OUT_OF_MEMORY;
	result->chd = chd;
	result->framesperhunk = header->hunkbytes / CD_FRAME_SIZE;
	result->cachedhunk = ~0u;

	err = parse_tracks(chd, &result->toc);
	if (err == CHDERR_NONE)
		err = compute_offsets(&result->toc);
	if (err == CHDERR_NONE)
		parse_cuesheet(result);
	if (err == CHDERR_NONE)
	{
		result->hunk = (uint8_t *)malloc(header->hunkbytes);
		if (result->hunk == NULL)
			err = CHDERR_OUT_OF_MEMORY;
	}
	if (err != CHDERR_NONE)
	{
		chd_cd_close(result);
		return err;
	}
	*cd = result;
	return CHDERR_NONE;
}

CHD_EXPORT void chd_cd_close(chd_cd *cd)
{
	if (cd == NULL)
		return;
	free(cd->hunk);
	free(cd->indices);
	free(cd);
}

CHD_EXPORT const chd_cd_toc *chd_cd_get_toc(const chd_cd *cd)
{
	return cd != NULL ? &cd->toc : NULL;
}

CHD_EXPORT chd_error chd_cd_get_index(const chd_cd *cd, uint32_t track, uint32_t index, uint32_t *lba)
{
	const chd_cd_track *t;
	uint32_t i;

	if (cd == NULL || lba == NULL || track == 0 || track > cd->toc.numtracks || index > 99)
		return CHDERR_INVALID_PARAMETER;
	t = &cd->toc.tracks[track - 1];

	for (i = 0; i < cd->numindices; i++)
		if (cd->indices[i].track == track && cd->indices[i].index == index)
		{
			int64_t value = (int64_t)t->logframeofs + cd->indices[i].ofs;
			if (value < 0 || value > UINT32_MAX)
				return CHDERR_INVALID_METADATA;
			*lba = (uint32_t)value;
			return CHDERR_NONE;
		}

	if (index == 1)
		*lba = t->logframeofs;
	else if (index == 0 && t->pregap > 0)
		*lba = t->logframeofs - t->pregap;
	else
		return CHDERR_METADATA_NOT_FOUND;
	return CHDERR_NONE;
}

/* where the 2048 user data bytes start in a stored sector, -1 = none */
static int user_data_offset(uint32_t type)
{
	switch (type)
	{
		case CD_TRACK_MODE1:          return 0;
		case CD_TRACK_MODE1_RAW:      return 16;
		case CD_TRACK_MODE2:          return 8;
		case CD_TRACK_MODE2_FORM1:    return 0;
		case CD_TRACK_MODE2_FORM_MIX: return 8;
		case CD_TRACK_MODE2_RAW:      return 24;
		default:                      return -1;
	}
}

CHD_EXPORT chd_error chd_cd_read_sector(chd_cd *cd, uint32_t lba, void *buffer, uint32_t what)
{
	static const uint32_t sizes[] = { 2048, CD_MAX_SECTOR_DATA, CD_MAX_SUBCODE_DATA };
	const chd_cd_track *track;
	uint32_t t, offset, frame, hunknum;
	const uint8_t *src;
	int dataofs = 0;

	if (cd == NULL || buffer == NULL || what > CHD_CD_READ_SUBCODE)
		return CHDERR_INVALID_PARAMETER;
	if (lba >= cd->toc.leadout)
		return CHDERR_HUNK_OUT_OF_RANGE;

	/* the track whose INDEX 00 is the last one at or before lba */
	for (t = cd->toc.numtracks - 1; t > 0; t--)
		if (lba >= cd->toc.tracks[t].logframeofs - cd->toc.tracks[t].pregap)
			break;
	track = &cd->toc.tracks[t];

	if (what == CHD_CD_READ_DATA && (dataofs = user_data_offset(track->type)) < 0)
		return CHDERR_INVALID_PARAMETER;

	/* frame within the stored track; an unstored pregap or the postgap
	 * reads as silence */
	offset = lba - (track->logframeofs - track->pregap);
	if (track->pgdatasize == 0 && offset < track->pregap)
	{
		memset(buffer, 0, sizes[what]);
		return CHDERR_NONE;
	}
	frame = (track->pgdatasize == 0) ? offset - track->pregap : offset;
	if (frame >= track->frames)
	{
		memset(buffer, 0, sizes[what]);
		return CHDERR_NONE;
	}

	frame += track->chdframeofs;
	hunknum = frame / cd->framesperhunk;
	if (hunknum != cd->cachedhunk)
	{
		chd_error err = chd_read(cd->chd, hunknum, cd->hunk);
		cd->cachedhunk = (err == CHDERR_NONE) ? hunknum : ~0u;
		if (err != CHDERR_NONE)
			return err;
	}
	src = cd->hunk + (frame % cd->framesperhunk) * CD_FRAME_SIZE;

	switch (what)
	{
		case CHD_CD_READ_DATA:
			memcpy(buffer, src + dataofs, 2048);
			break;

		case CHD_CD_READ_RAW:
			memcpy(buffer, src, track->datasize);
			memset((uint8_t *)buffer + track->datasize, 0, CD_MAX_SECTOR_DATA - track->datasize);
			/* CHDs hold CDDA big-endian, except legacy GD-ROMs */
			if (track->type == CD_TRACK_AUDIO && !(cd->toc.flags & CD_FLAG_GDROMLE))
			{
				uint8_t *p = (uint8_t *)buffer;
				uint32_t i;
				for (i = 0; i < CD_MAX_SECTOR_DATA; i += 2)
				{
					uint8_t tmp = p[i];
					p[i] = p[i + 1];
					p[i + 1] = tmp;
				}
			}
			break;

		case CHD_CD_READ_SUBCODE:
			if (track->subsize != 0)
				memcpy(buffer, src + CD_MAX_SECTOR_DATA, CD_MAX_SUBCODE_DATA);
			else
				memset(buffer, 0, CD_MAX_SUBCODE_DATA);
			break;
	}
	return CHDERR_NONE;
}
