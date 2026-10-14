/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
***************************************************************************

    cdrom.h

    Generic MAME cd-rom implementation

***************************************************************************/

#pragma once

#ifndef __CDROM_H__
#define __CDROM_H__

#include <stdint.h>
#include "chd.h"
#include "chdconfig.h"
#include "macros.h"

/***************************************************************************
    CONSTANTS
***************************************************************************/

/* tracks are padded to a multiple of this many frames */
#define CD_TRACK_PADDING   	(4)
#define CD_MAX_TRACKS           (99)    /* AFAIK the theoretical limit */
#define CD_MAX_SECTOR_DATA      (2352)
#define CD_MAX_SUBCODE_DATA     (96)

#define CD_FRAME_SIZE           (CD_MAX_SECTOR_DATA + CD_MAX_SUBCODE_DATA)
#define CD_FRAMES_PER_HUNK      (8)

#define CD_METADATA_WORDS       (1+(CD_MAX_TRACKS * 6))

enum
{
	CD_TRACK_MODE1 = 0,         /* mode 1 2048 bytes/sector */
	CD_TRACK_MODE1_RAW,         /* mode 1 2352 bytes/sector */
	CD_TRACK_MODE2,             /* mode 2 2336 bytes/sector */
	CD_TRACK_MODE2_FORM1,       /* mode 2 2048 bytes/sector */
	CD_TRACK_MODE2_FORM2,       /* mode 2 2324 bytes/sector */
	CD_TRACK_MODE2_FORM_MIX,    /* mode 2 2336 bytes/sector */
	CD_TRACK_MODE2_RAW,         /* mode 2 2352 bytes / sector */
	CD_TRACK_AUDIO,         /* redbook audio track 2352 bytes/sector (588 samples) */

	CD_TRACK_RAW_DONTCARE       /* special flag for cdrom_read_data: just return me whatever is there */
};

enum
{
	CD_SUB_NORMAL = 0,          /* "cooked" 96 bytes per sector */
	CD_SUB_RAW,                 /* raw uninterleaved 96 bytes per sector */
	CD_SUB_NONE                 /* no subcode data stored */
};

#define CD_FLAG_GDROM   0x00000001  /* disc is a GD-ROM, all tracks should be stored with GD-ROM metadata */
#define CD_FLAG_GDROMLE 0x00000002  /* legacy GD-ROM, with little-endian CDDA data */

/***************************************************************************
    SECTOR ACCESS BY LBA

    A CD CHD stores tracks back to back as 2448-byte frames (2352 sector
    bytes, 96 subcode bytes), each track padded to CD_TRACK_PADDING frames,
    and the layout lives in per-track metadata. chd_cd_open() parses that
    metadata into a TOC and chd_cd_read_sector() reads by LBA, so callers no
    longer redo the hunk and padding arithmetic themselves (#155).

    LBAs follow MAME's logical addressing: LBA 0 is the start of track 1,
    and pregaps and postgaps take up LBAs whether or not their sectors are
    stored. Use msf_to_lba() / lba_to_msf() below to address by MSF.
***************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _chd_cd_track chd_cd_track;
struct _chd_cd_track
{
	uint32_t	number;			/* track number, 1-based */
	uint32_t	type;			/* CD_TRACK_* */
	uint32_t	subtype;		/* CD_SUB_* */
	uint32_t	datasize;		/* sector bytes stored per frame */
	uint32_t	subsize;		/* subcode bytes stored per frame */
	uint32_t	frames;			/* frames stored, pregap included when pgdatasize != 0 */
	uint32_t	pregap;			/* pregap frames, INDEX 00 to INDEX 01 */
	uint32_t	pgdatasize;		/* non-zero when the pregap is stored in the CHD */
	uint32_t	postgap;		/* postgap frames, never stored */
	uint32_t	logframeofs;	/* LBA of INDEX 01 */
	uint32_t	chdframeofs;	/* first frame of the track within the CHD */
	char		isrc[13];		/* from an embedded cue sheet, "" if none */
};

typedef struct _chd_cd_toc chd_cd_toc;
struct _chd_cd_toc
{
	uint32_t		numtracks;
	uint32_t		flags;		/* CD_FLAG_* */
	uint32_t		leadout;	/* first LBA past the last track */
	char			catalog[14];	/* from an embedded cue sheet, "" if none */
	chd_cd_track	tracks[CD_MAX_TRACKS];
};

/* what chd_cd_read_sector() returns, and how many bytes it writes */
enum
{
	CHD_CD_READ_DATA = 0,	/* 2048 bytes of user data; data tracks only */
	CHD_CD_READ_RAW,		/* CD_MAX_SECTOR_DATA bytes as a .bin would hold them:
							 * audio little-endian, a cooked track's datasize
							 * bytes followed by zeros */
	CHD_CD_READ_SUBCODE		/* CD_MAX_SUBCODE_DATA bytes, zeros if none stored */
};

typedef struct _chd_cd chd_cd;

/* Parse the track metadata of an open CD CHD. The handle borrows `chd`,
 * which must outlive it. CHDERR_INVALID_METADATA when the track metadata is
 * missing or inconsistent. */
CHD_EXPORT chd_error chd_cd_open(chd_file *chd, chd_cd **cd);
CHD_EXPORT void chd_cd_close(chd_cd *cd);
CHD_EXPORT const chd_cd_toc *chd_cd_get_toc(const chd_cd *cd);

/* Read one sector. Sectors of a pregap or postgap that the CHD does not
 * store read as zeros. The last decoded hunk is kept, so reading the frames
 * of a hunk in a row decodes it once. CHDERR_HUNK_OUT_OF_RANGE past the
 * leadout; CHDERR_INVALID_PARAMETER for CHD_CD_READ_DATA on an audio or
 * Mode 2 Form 2 track. */
CHD_EXPORT chd_error chd_cd_read_sector(chd_cd *cd, uint32_t lba, void *buffer, uint32_t what);

/* LBA of INDEX `index` (0-99) of track `track` (1-based). INDEX 01 is always
 * known, INDEX 00 when the track has a pregap, and any other INDEX when an
 * embedded cue sheet (CDROM_CUESHEET_METADATA_TAG) lists it; otherwise
 * CHDERR_METADATA_NOT_FOUND. A cue sheet whose tracks do not match the
 * CHD's is ignored as a whole, catalog and ISRCs included. */
CHD_EXPORT chd_error chd_cd_get_index(const chd_cd *cd, uint32_t track, uint32_t index, uint32_t *lba);

#ifdef __cplusplus
}
#endif

/***************************************************************************
    FUNCTION PROTOTYPES
***************************************************************************/

#if WANT_RAW_DATA_SECTOR
/* ECC utilities */
int ecc_verify(const uint8_t *sector);
void ecc_generate(uint8_t *sector);
void ecc_clear(uint8_t *sector);
#endif

chd_error cd_codec_decompress(
	uint8_t *scratch,
	void *base_decompressor, chd_codec_interface_decompress base_decompress,
	void *subcode_decompressor, chd_codec_interface_decompress subcode_decompress,
	const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen);


/***************************************************************************
    INLINE FUNCTIONS
***************************************************************************/

static CHDR_INLINE uint32_t msf_to_lba(uint32_t msf)
{
	return ( ((msf&0x00ff0000)>>16) * 60 * 75) + (((msf&0x0000ff00)>>8) * 75) + ((msf&0x000000ff)>>0);
}

static CHDR_INLINE uint32_t lba_to_msf(uint32_t lba)
{
	uint8_t m, s, f;

	m = lba / (60 * 75);
	lba -= m * (60 * 75);
	s = lba / 75;
	f = lba % 75;

	return ((m / 10) << 20) | ((m % 10) << 16) |
			((s / 10) << 12) | ((s % 10) <<  8) |
			((f / 10) <<  4) | ((f % 10) <<  0);
}

/**
 * segacd needs it like this.. investigate
 * Angelo also says PCE tracks often start playing at the
 * wrong address.. related?
 **/
static CHDR_INLINE uint32_t lba_to_msf_alt(int lba)
{
	uint32_t ret = 0;

	ret |= ((lba / (60 * 75))&0xff)<<16;
	ret |= (((lba / 75) % 60)&0xff)<<8;
	ret |= ((lba % 75)&0xff)<<0;

	return ret;
}

#endif  /* __CDROM_H__ */
