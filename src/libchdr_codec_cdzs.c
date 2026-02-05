#include "../include/libchdr/codec_cdzs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/cdrom.h"

chd_error cdzs_codec_init(void* codec, uint32_t hunkbytes)
{
	chd_error ret;
	cdzs_codec_data* cdzs = (cdzs_codec_data*) codec;

	/* allocate buffer */
	cdzs->buffer = (uint8_t*)malloc(sizeof(uint8_t) * hunkbytes);
	if (cdzs->buffer == NULL)
		return CHDERR_OUT_OF_MEMORY;

	/* make sure the CHD's hunk size is an even multiple of the frame size */
	ret = zstd_codec_init(&cdzs->base_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SECTOR_DATA);
	if (ret != CHDERR_NONE)
		return ret;

#ifdef WANT_SUBCODE
	ret = zstd_codec_init(&cdzs->subcode_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SUBCODE_DATA);
	if (ret != CHDERR_NONE)
		return ret;
#endif

	if (hunkbytes % CD_FRAME_SIZE != 0)
		return CHDERR_CODEC_ERROR;

	return CHDERR_NONE;
}

void cdzs_codec_free(void* codec)
{
	cdzs_codec_data* cdzs = (cdzs_codec_data*) codec;
	free(cdzs->buffer);
	zstd_codec_free(&cdzs->base_decompressor);
#ifdef WANT_SUBCODE
	zstd_codec_free(&cdzs->subcode_decompressor);
#endif
}

chd_error cdzs_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	uint32_t framenum;
	cdzs_codec_data* cdzs = (cdzs_codec_data*)codec;
	chd_error decomp_err;
	uint32_t complen_base;

	/* determine header bytes */
	const uint32_t frames = destlen / CD_FRAME_SIZE;
	const uint32_t complen_bytes = (destlen < 65536) ? 2 : 3;
	const uint32_t ecc_bytes = (frames + 7) / 8;
	const uint32_t header_bytes = ecc_bytes + complen_bytes;

	/* input may be truncated, double-check */
	if (complen < (ecc_bytes + 2))
		return CHDERR_DECOMPRESSION_ERROR;

	/* extract compressed length of base */
	complen_base = (src[ecc_bytes + 0] << 8) | src[ecc_bytes + 1];
	if (complen_bytes > 2)
	{
		if (complen < (ecc_bytes + 3))
			return CHDERR_DECOMPRESSION_ERROR;

		complen_base = (complen_base << 8) | src[ecc_bytes + 2];
	}
	if (complen < (header_bytes + complen_base))
		return CHDERR_DECOMPRESSION_ERROR;

	/* reset and decode */
	decomp_err = zstd_codec_decompress(&cdzs->base_decompressor, &src[header_bytes], complen_base, &cdzs->buffer[0], frames * CD_MAX_SECTOR_DATA);
	if (decomp_err != CHDERR_NONE)
		return decomp_err;
#ifdef WANT_SUBCODE
	decomp_err = zstd_codec_decompress(&cdzs->subcode_decompressor, &src[header_bytes + complen_base], complen - complen_base - header_bytes, &cdzs->buffer[frames * CD_MAX_SECTOR_DATA], frames * CD_MAX_SUBCODE_DATA);
	if (decomp_err != CHDERR_NONE)
		return decomp_err;
#endif

	/* reassemble the data */
	for (framenum = 0; framenum < frames; framenum++)
	{
		uint8_t *sector;

		memcpy(&dest[framenum * CD_FRAME_SIZE], &cdzs->buffer[framenum * CD_MAX_SECTOR_DATA], CD_MAX_SECTOR_DATA);
#ifdef WANT_SUBCODE
		memcpy(&dest[framenum * CD_FRAME_SIZE + CD_MAX_SECTOR_DATA], &cdzs->buffer[frames * CD_MAX_SECTOR_DATA + framenum * CD_MAX_SUBCODE_DATA], CD_MAX_SUBCODE_DATA);
#endif

#ifdef WANT_RAW_DATA_SECTOR
		/* reconstitute the ECC data and sync header */
		sector = (uint8_t *)&dest[framenum * CD_FRAME_SIZE];
		if ((src[framenum / 8] & (1 << (framenum % 8))) != 0)
		{
			memcpy(sector, s_cd_sync_header, sizeof(s_cd_sync_header));
			ecc_generate(sector);
		}
#endif
	}
	return CHDERR_NONE;
}
