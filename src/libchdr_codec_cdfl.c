#include "codec_cdfl.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef CHDR_DEBUG_ZLIB
#include <stdio.h>
#endif

#include "../include/libchdr/cdrom.h"

/* CHDR_PROFILE_CDFL: attribute cdfl hunk time across its three stages.
 * cdfl measured ~3.6x cdlz's codec-only time per hunk on ESP32-P4, which is
 * the opposite of what raw FLAC vs LZMA decode cost would predict - the
 * suspicion being flac_decoder_reset()'s per-hunk drflac_open_with_metadata()
 * (full decoder teardown + rebuild, ~40KB alloc, STREAMINFO reparse) rather
 * than the audio decode itself. The host supplies the clock so this stays
 * free of any platform dependency. */
#ifdef CHDR_PROFILE_CDFL
extern int64_t chdr_prof_now_us(void);
uint64_t chdr_prof_flac_reset_us;
uint64_t chdr_prof_flac_decode_us;
uint64_t chdr_prof_subcode_us;
uint64_t chdr_prof_cdfl_hunks;
#define PROF_T0() int64_t prof_t = chdr_prof_now_us()
#define PROF_ACC(acc) do { acc += (uint64_t)(chdr_prof_now_us() - prof_t); } while (0)
#else
#define PROF_T0() do {} while (0)
#define PROF_ACC(acc) do {} while (0)
#endif

static uint32_t cdfl_codec_blocksize(uint32_t bytes)
{
	/* for CDs it seems that CD_MAX_SECTOR_DATA is the right target */
	uint32_t blocksize = bytes / 4;
	while (blocksize > CD_MAX_SECTOR_DATA)
		blocksize /= 2;
	return blocksize;
}

chd_error cdfl_codec_init(void *codec, uint32_t hunkbytes)
{
#if WANT_SUBCODE
	chd_error ret;
#endif
	cdfl_codec_data *cdfl = (cdfl_codec_data*)codec;

	/* make sure the CHD's hunk size is an even multiple of the frame size */
	if (hunkbytes % CD_FRAME_SIZE != 0)
		return CHDERR_CODEC_ERROR;

	cdfl->buffer = (uint8_t*)malloc(sizeof(uint8_t) * hunkbytes);
	if (cdfl->buffer == NULL)
		return CHDERR_OUT_OF_MEMORY;

	/* determine whether we want native or swapped samples */
	cdfl->swap_endian = flac_decoder_detect_native_endian();

#if WANT_SUBCODE
	/* init zlib inflater */
	ret = zlib_codec_init(&cdfl->subcode_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SECTOR_DATA);
	if (ret != CHDERR_NONE)
		return ret;
#endif

	/* flac decoder init */
	if (flac_decoder_init(&cdfl->decoder))
		return CHDERR_OUT_OF_MEMORY;

	return CHDERR_NONE;
}

void cdfl_codec_free(void *codec)
{
	cdfl_codec_data *cdfl = (cdfl_codec_data*)codec;
	flac_decoder_free(&cdfl->decoder);
#if WANT_SUBCODE
	zlib_codec_free(&cdfl->subcode_decompressor);
#endif
	if (cdfl->buffer)
		free(cdfl->buffer);
}

chd_error cdfl_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	uint32_t framenum;
	uint8_t *buffer;
#if WANT_SUBCODE
	uint32_t offset;
	chd_error ret;
#endif
	cdfl_codec_data *cdfl = (cdfl_codec_data*)codec;

	/* reset and decode */
	uint32_t frames = destlen / CD_FRAME_SIZE;

	{ PROF_T0();
	if (!flac_decoder_reset(&cdfl->decoder, 44100, 2, cdfl_codec_blocksize(frames * CD_MAX_SECTOR_DATA), src, complen)) {
#ifdef CHDR_DEBUG_ZLIB
		printf("cdfl_codec_decompress: stage=flac_reset complen=%u alloc_failed=%d\n",
			(unsigned)complen, cdfl->decoder.alloc_failed);
#endif
		/* reset() allocates a fresh ~40KB drflac decoder per hunk; on a
		 * small-RAM target that, not the stream, is what usually fails */
		return cdfl->decoder.alloc_failed ? CHDERR_OUT_OF_MEMORY : CHDERR_DECOMPRESSION_ERROR;
	}
	PROF_ACC(chdr_prof_flac_reset_us); }
	buffer = &cdfl->buffer[0];
	{ PROF_T0();
	if (!flac_decoder_decode_interleaved(&cdfl->decoder, (int16_t *)(buffer), frames * CD_MAX_SECTOR_DATA/4, cdfl->swap_endian)) {
#ifdef CHDR_DEBUG_ZLIB
		printf("cdfl_codec_decompress: stage=flac_decode complen=%u alloc_failed=%d\n",
			(unsigned)complen, cdfl->decoder.alloc_failed);
#endif
		/* alloc_failed is cleared by the reset() above, so it can only be
		 * set here by an allocation the decode itself attempted */
		return cdfl->decoder.alloc_failed ? CHDERR_OUT_OF_MEMORY : CHDERR_DECOMPRESSION_ERROR;
	}
	PROF_ACC(chdr_prof_flac_decode_us); }

#if WANT_SUBCODE
	/* inflate the subcode data */
	{ PROF_T0();
	offset = flac_decoder_finish(&cdfl->decoder);
	ret = zlib_codec_decompress(&cdfl->subcode_decompressor, src + offset, complen - offset, &cdfl->buffer[frames * CD_MAX_SECTOR_DATA], frames * CD_MAX_SUBCODE_DATA);
	if (ret != CHDERR_NONE) {
#ifdef CHDR_DEBUG_ZLIB
		printf("cdfl_codec_decompress: stage=subcode offset=%u complen_subcode=%u err=%d\n",
			(unsigned)offset, (unsigned)(complen - offset), ret);
#endif
		return ret;
	}
	PROF_ACC(chdr_prof_subcode_us); }
#else
	flac_decoder_finish(&cdfl->decoder);
#endif
#ifdef CHDR_PROFILE_CDFL
	chdr_prof_cdfl_hunks++;
#endif

	/* reassemble the data */
	for (framenum = 0; framenum < frames; framenum++)
	{
		memcpy(&dest[framenum * CD_FRAME_SIZE], &cdfl->buffer[framenum * CD_MAX_SECTOR_DATA], CD_MAX_SECTOR_DATA);
#if WANT_SUBCODE
		memcpy(&dest[framenum * CD_FRAME_SIZE + CD_MAX_SECTOR_DATA], &cdfl->buffer[frames * CD_MAX_SECTOR_DATA + framenum * CD_MAX_SUBCODE_DATA], CD_MAX_SUBCODE_DATA);
#endif
	}

	return CHDERR_NONE;
}
