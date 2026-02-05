/* Disable unused features of miniz. */
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME

#include "deps/lzma-25.01/src/LzmaDec.c"
#include "deps/miniz-3.1.0/miniz.c"
#include "deps/zstd-1.5.7/zstddeclib.c"
#include "src/libchdr_bitstream.c"
#include "src/libchdr_cdrom.c"
#include "src/libchdr_chd.c"
#include "src/libchdr_flac.c"
#include "src/libchdr_huffman.c"
