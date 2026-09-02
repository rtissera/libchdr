# Wraps libchdr as a plain ESP-IDF component, pointing straight at this
# repo's real src/include/deps layout instead of vendoring a copy - the
# codec headers under src/ resolve their dep headers via file-relative
# quote-includes (e.g. src/codec_lzma.h does
# #include "../deps/lzma-26.02/include/LzmaDec.h"), so as long as the real
# directory structure stays intact, no extra include path is needed for
# them - only include/ has to be exposed, for <libchdr/chd.h> et al.
#
# Only the decoder half of each dep is built (LzmaDec.c, miniz.c with the
# archive/deflate/stdio/time APIs compiled out, zstd's single-file decoder
# amalgamation) - same source list CMakeLists.txt's deps/*/CMakeLists.txt
# targets use, just registered directly as this component's sources instead
# of as nested static-lib targets, since ESP-IDF's build graph doesn't need
# libchdr's own multi-target dependency setup (WITH_SYSTEM_ZLIB/ZSTD, shared
# lib, install rules, etc.) - none of that applies to a firmware image.

set(LIBCHDR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")

idf_component_register(
	SRCS
		"${LIBCHDR_ROOT}/src/libchdr_bitstream.c"
		"${LIBCHDR_ROOT}/src/libchdr_cdrom.c"
		"${LIBCHDR_ROOT}/src/libchdr_chd.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_avhuff.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_cdfl.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_cdlz.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_cdzl.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_cdzs.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_flac.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_huff.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_lzma.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_zlib.c"
		"${LIBCHDR_ROOT}/src/libchdr_codec_zstd.c"
		"${LIBCHDR_ROOT}/src/libchdr_flac.c"
		"${LIBCHDR_ROOT}/src/libchdr_huffman.c"
		"${LIBCHDR_ROOT}/deps/lzma-26.02/src/LzmaDec.c"
		"${LIBCHDR_ROOT}/deps/miniz-3.1.2/miniz.c"
		"${LIBCHDR_ROOT}/deps/zstd-1.5.7/zstddeclib.c"
	INCLUDE_DIRS
		"${LIBCHDR_ROOT}/include"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	WANT_RAW_DATA_SECTOR=1
	WANT_SUBCODE=1
	VERIFY_BLOCK_CRC=1
	LOWRAM_TARGET=0
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_DEFLATE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
	# CHDR_DEBUG_ZLIB: failure-path-only diagnostics (zlib_codec_decompress's
	# hex dump, cd_codec_decompress's base/subcode stage tag, cdfl's
	# flac/subcode stage tag) - an earlier version of this printed
	# unconditionally on every decompress() call and, over a 100+-file real
	# corpus, flooded UART with blocking printf()s badly enough to
	# destabilize the run on its own. Safe across a full corpus now.
	CHDR_DEBUG_ZLIB
)

# ESP-IDF's default -Werror=all catches a real GCC 14 false positive in
# zstd's single-file decoder amalgamation (HUF_decompress4X{1,2}...
# _usingDTable_internal_fast(): a local struct is fully assigned through a
# helper before the flagged read, GCC just can't see it at -Og) - scoped to
# this one file/component only, not a blanket warning suppression.
set_source_files_properties(
	"${LIBCHDR_ROOT}/deps/zstd-1.5.7/zstddeclib.c"
	PROPERTIES COMPILE_OPTIONS "-Wno-error=maybe-uninitialized"
)

# RESOLVED (was: "miniz inflate() returns Z_DATA_ERROR on real P4 silicon").
# Root cause was a link-time symbol collision, not silicon, codegen or the
# allocator: Espressif's ROMs export an older ROM-baked miniz's
# tinfl_decompress() from their linker scripts as an *absolute* symbol, which
# outranks this component's own definition. Full mechanism, and the symbol
# list, live in cmake/EspRomMinizWorkaround.cmake at the repo root (kept out
# of deps/ so a miniz upgrade can't silently drop it); measured before/after
# is in ../../README.md.
#
# This component compiles miniz.c itself rather than going through libchdr's
# CMake, so it applies the same renames directly.
include("${LIBCHDR_ROOT}/cmake/EspRomMinizWorkaround.cmake")
libchdr_apply_esp_rom_miniz_workaround(${COMPONENT_LIB})
