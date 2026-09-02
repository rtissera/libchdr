#include "codec_zlib.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef CHDR_DEBUG_ZLIB
#include <stdio.h>
#endif

#ifdef CHDR_SYSTEM_ZLIB
static voidpf zlib_fast_alloc(voidpf opaque, zlib_alloc_size items, zlib_alloc_size size);
static void zlib_fast_free(voidpf opaque, voidpf address);
static void zlib_allocator_free(voidpf opaque);
#endif

/*-------------------------------------------------
    zlib_codec_init - initialize the ZLIB codec
-------------------------------------------------*/

#ifndef CHDR_SYSTEM_ZLIB

/* ---- bundled miniz: drive tinfl directly, no 32KB dictionary ---- */

chd_error zlib_codec_init(void *codec, uint32_t hunkbytes)
{
	zlib_codec_data *data = (zlib_codec_data *)codec;

	(void)hunkbytes;

	memset(data, 0, sizeof(zlib_codec_data));
	data->inflater = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
	if (data->inflater == NULL)
		return CHDERR_OUT_OF_MEMORY;
	tinfl_init(data->inflater);
	return CHDERR_NONE;
}

void zlib_codec_free(void *codec)
{
	zlib_codec_data *data = (zlib_codec_data *)codec;

	if (data != NULL && data->inflater != NULL)
	{
		free(data->inflater);
		data->inflater = NULL;
	}
}

chd_error zlib_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	zlib_codec_data *data = (zlib_codec_data *)codec;
	size_t in_bytes = complen;
	size_t out_bytes = destlen;
	tinfl_status status;

	if (data->inflater == NULL)
		return CHDERR_DECOMPRESSION_ERROR;

	/* one hunk == one complete raw-deflate stream, decoded in a single call
	 * into a buffer that holds all of it: no zlib header, no further input to
	 * come, and the output buffer doubles as the dictionary */
	tinfl_init(data->inflater);
	status = tinfl_decompress(data->inflater, (const mz_uint8 *)src, &in_bytes,
		(mz_uint8 *)dest, (mz_uint8 *)dest, &out_bytes,
		TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

	if (status != TINFL_STATUS_DONE || out_bytes != destlen) {
#ifdef CHDR_DEBUG_ZLIB
		printf("zlib_codec_decompress: FAILED complen=%u destlen=%u status=%d in_used=%u out=%u src=",
			(unsigned)complen, (unsigned)destlen, (int)status,
			(unsigned)in_bytes, (unsigned)out_bytes);
		for (uint32_t dbg_i = 0; dbg_i < complen; dbg_i++)
			printf("%02x", src[dbg_i]);
		printf("\n");
#endif
		return CHDERR_DECOMPRESSION_ERROR;
	}

	return CHDERR_NONE;
}

#else /* CHDR_SYSTEM_ZLIB */

chd_error zlib_codec_init(void *codec, uint32_t hunkbytes)
{
	int zerr;
	chd_error err;
	zlib_codec_data *data = (zlib_codec_data*)codec;

	(void)hunkbytes;

	/* clear the buffers */
	memset(data, 0, sizeof(zlib_codec_data));

	/* init the inflater first */
	data->inflater.next_in = (Bytef *)data;	/* bogus, but that's ok */
	data->inflater.avail_in = 0;
	data->inflater.zalloc = zlib_fast_alloc;
	data->inflater.zfree = zlib_fast_free;
	data->inflater.opaque = &data->allocator;
	zerr = inflateInit2(&data->inflater, -MAX_WBITS);

	/* convert errors */
	if (zerr == Z_MEM_ERROR)
		err = CHDERR_OUT_OF_MEMORY;
	else if (zerr != Z_OK)
		err = CHDERR_CODEC_ERROR;
	else
		err = CHDERR_NONE;

	return err;
}

/*-------------------------------------------------
    zlib_codec_free - free data for the ZLIB
    codec
-------------------------------------------------*/

void zlib_codec_free(void *codec)
{
	zlib_codec_data *data = (zlib_codec_data *)codec;

	/* deinit the streams */
	if (data != NULL)
	{
		inflateEnd(&data->inflater);

		/* free our fast memory */
		zlib_allocator_free(&data->allocator);
	}
}

/*-------------------------------------------------
    zlib_codec_decompress - decompress data using
    the ZLIB codec
-------------------------------------------------*/

chd_error zlib_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	zlib_codec_data *data = (zlib_codec_data *)codec;
	int zerr;

	/* reset the decompressor */
	data->inflater.next_in = (Bytef *)src;
	data->inflater.avail_in = complen;
	data->inflater.total_in = 0;
	data->inflater.next_out = (Bytef *)dest;
	data->inflater.avail_out = destlen;
	data->inflater.total_out = 0;
	zerr = inflateReset(&data->inflater);
	if (zerr != Z_OK) {
#ifdef CHDR_DEBUG_ZLIB
		printf("zlib_codec_decompress: inflateReset FAILED zerr=%d\n", zerr);
#endif
		return CHDERR_DECOMPRESSION_ERROR;
	}

	/* do it */
	zerr = inflate(&data->inflater, Z_FINISH);
	if (data->inflater.total_out != destlen) {
#ifdef CHDR_DEBUG_ZLIB
		/* only dump on the failure path - an unconditional per-call hex
		 * dump of every compressed block is too slow/UART-heavy to run
		 * across a real multi-hundred-file corpus (was previously seen to
		 * destabilize a full run outright). */
		printf("zlib_codec_decompress: FAILED complen=%u destlen=%u zerr=%d total_out=%u avail_in=%u avail_out=%u data=%p inflater.state=%p src=",
			(unsigned)complen, (unsigned)destlen, zerr, (unsigned)data->inflater.total_out,
			(unsigned)data->inflater.avail_in, (unsigned)data->inflater.avail_out,
			(void*)data, (void*)data->inflater.state);
		for (uint32_t dbg_i = 0; dbg_i < complen; dbg_i++)
			printf("%02x", src[dbg_i]);
		printf("\n");
#endif
		return CHDERR_DECOMPRESSION_ERROR;
	}

	return CHDERR_NONE;
}

#endif /* CHDR_SYSTEM_ZLIB */

#ifdef CHDR_SYSTEM_ZLIB

/*-------------------------------------------------
    zlib_fast_alloc - fast malloc for ZLIB, which
    allocates and frees memory frequently
-------------------------------------------------*/

/* Huge alignment values for possible SIMD optimization by compiler (NEON, SSE, AVX) */
#define ZLIB_MIN_ALIGNMENT_BITS 512
#define ZLIB_MIN_ALIGNMENT_BYTES (ZLIB_MIN_ALIGNMENT_BITS / 8)

static voidpf zlib_fast_alloc(voidpf opaque, zlib_alloc_size items, zlib_alloc_size size)
{
	zlib_allocator *alloc = (zlib_allocator *)opaque;
	uintptr_t paddr = 0;
	uint32_t *ptr;
	int i;

	/* compute the size, rounding to the nearest 1k */
	size = (size * items + 0x3ff) & ~0x3ff;

	/* reuse a hunk if we can */
	for (i = 0; i < MAX_ZLIB_ALLOCS; i++)
	{
		ptr = alloc->allocptr[i];
		if (ptr && size == *ptr)
		{
			/* set the low bit of the size so we don't match next time */
			*ptr |= 1;

			/* return aligned block address */
			return (voidpf)(alloc->allocptr2[i]);
		}
	}

	/* alloc a new one */
    ptr = (uint32_t *)malloc(size + sizeof(uint32_t) + ZLIB_MIN_ALIGNMENT_BYTES);
	if (!ptr)
		return NULL;

	/* put it into the list */
	for (i = 0; i < MAX_ZLIB_ALLOCS; i++)
		if (!alloc->allocptr[i])
		{
			alloc->allocptr[i] = ptr;
			paddr = (((uintptr_t)ptr) + sizeof(uint32_t) + (ZLIB_MIN_ALIGNMENT_BYTES-1)) & (~(ZLIB_MIN_ALIGNMENT_BYTES-1));
			alloc->allocptr2[i] = (uint32_t*)paddr;
			break;
		}

	/* set the low bit of the size so we don't match next time */
	*ptr = size | 1;

	/* return aligned block address */
	return (voidpf)paddr;
}

/*-------------------------------------------------
    zlib_fast_free - fast free for ZLIB, which
    allocates and frees memory frequently
-------------------------------------------------*/

static void zlib_fast_free(voidpf opaque, voidpf address)
{
	zlib_allocator *alloc = (zlib_allocator *)opaque;
	uint32_t *ptr = (uint32_t *)address;
	int i;

	/* find the hunk */
	for (i = 0; i < MAX_ZLIB_ALLOCS; i++)
		if (ptr == alloc->allocptr2[i])
		{
			/* clear the low bit of the size to allow matches */
			*(alloc->allocptr[i]) &= ~1;
			return;
		}
}

/*-------------------------------------------------
    zlib_allocator_free
-------------------------------------------------*/
static void zlib_allocator_free(voidpf opaque)
{
	zlib_allocator *alloc = (zlib_allocator *)opaque;
	int i;

	for (i = 0; i < MAX_ZLIB_ALLOCS; i++)
		if (alloc->allocptr[i])
			free(alloc->allocptr[i]);
}

#endif /* CHDR_SYSTEM_ZLIB */
