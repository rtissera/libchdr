/* Disable unused features of miniz (but allow
   them to be restored by dependent projects). */
#ifndef MINIZ_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#endif

#ifndef MINIZ_DEFLATE_APIS
#define MINIZ_NO_DEFLATE_APIS
#endif

#ifndef MINIZ_STDIO
#define MINIZ_NO_STDIO
#endif

#ifndef MINIZ_TIME
#define MINIZ_NO_TIME
#endif

/* Prevent symbols being exported, to improve optimistation. */
#define CHD_EXPORT
#define ZSTDLIB_VISIBLE ZSTDLIB_HIDDEN
#define ZSTDERRORLIB_VISIBLE ZSTDERRORLIB_HIDDEN

#include "deps/lzma-26.02/src/LzmaDec.c"
#include "deps/miniz-3.1.2/miniz.c"
#include "deps/zstd-1.6.0/zstddeclib.c"

#include "src/libchdr_bitstream.c"
#include "src/libchdr_cdrom.c"
#include "src/libchdr_chd.c"
#include "src/libchdr_codec_avhuff.c"
#include "src/libchdr_codec_cdfl.c"
#include "src/libchdr_codec_cdlz.c"
#include "src/libchdr_codec_cdzl.c"
#include "src/libchdr_codec_cdzs.c"
#include "src/libchdr_codec_flac.c"
#include "src/libchdr_codec_huff.c"
#include "src/libchdr_codec_lzma.c"
#include "src/libchdr_codec_zlib.c"
#include "src/libchdr_codec_zstd.c"
#include "src/libchdr_flac.c"
#include "src/libchdr_huffman.c"
