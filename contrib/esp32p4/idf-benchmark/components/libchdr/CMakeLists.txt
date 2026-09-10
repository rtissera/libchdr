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

if(NOT DEFINED LOWRAM_TARGET_VAL)
	set(LOWRAM_TARGET_VAL 1)
endif()

set(LIBCHDR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")

# FLAC backend. micro-flac is the default here, unlike libchdr itself: on an
# ESP32-S3 with I/O excluded it is 1.198x on a CD-FLAC hunk and 1.233x on raw
# FLAC, its peak heap is lower on most images, and an MCU integrator already
# clones an SDK and a toolchain, so one more pinned checkout costs nothing.
# libchdr's own default stays dr_flac: a desktop consumer vendoring src/ must
# not have to fetch anything.
#
# The ESP32-P4 has not been re-measured since the STREAMINFO block-size fix,
# which took an oversized buffer out of dr_flac and most of micro-flac's lead
# with it - the older 1.41x quoted here is not valid.
#
# Set CHDR_FLAC_BACKEND=drflac to go back, e.g. if Apache-2.0 does not suit
# (micro-flac is Apache-2.0; libchdr stays BSD-3 because this is fetched, never
# vendored). Or point CHDR_MICROFLAC_SOURCE_DIR at your own checkout, including
# a managed component at
# ${CMAKE_BINARY_DIR}/../managed_components/esphome__micro-flac.
if(NOT DEFINED CHDR_FLAC_BACKEND)
	set(CHDR_FLAC_BACKEND microflac)
endif()

# ESP-IDF runs every component CMakeLists in script mode first, just to collect
# REQUIRES, and FetchContent is not scriptable there (define_property fails).
# The backend choice does not affect requirements, so that pass takes the
# in-tree source and the fetch happens once, in the real configure.
if(CMAKE_SCRIPT_MODE_FILE)
	set(CHDR_FLAC_BACKEND drflac)
endif()
set(CHDR_MICROFLAC_GIT_TAG "ffbe8a9ba5e78c16e535a4695a8b2418b0c091ee"
	CACHE STRING "micro-flac commit to fetch when no local checkout is given")
if(CHDR_FLAC_BACKEND STREQUAL "microflac")
	if(NOT CHDR_MICROFLAC_SOURCE_DIR)
		# Pinned SHA, because the backend compiles against micro-flac's
		# internals. Fetched at configure time rather than through
		# idf_component.yml so that this component stands alone.
		include(FetchContent)
		# SOURCE_SUBDIR names a directory that does not exist on purpose:
		# it downloads the sources without running micro-flac's own
		# CMakeLists, whose targets would otherwise be configured for the
		# host and joined to this cross build.
		FetchContent_Declare(microflac
			GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
			GIT_TAG ${CHDR_MICROFLAC_GIT_TAG}
			SOURCE_SUBDIR chdr-sources-only)
		FetchContent_MakeAvailable(microflac)
		set(CHDR_MICROFLAC_SOURCE_DIR "${microflac_SOURCE_DIR}")
	endif()
	# micro-flac enables its Xtensa LPC assembly whenever the core has the
	# required features, so the .S files must be compiled in on esp32/esp32s3
	# or the link fails on restore_lpc_*_asm. Worth ~1.064x on its own; the
	# RISC-V parts take the C path and need nothing extra.
	set(CHDR_MICROFLAC_ASM "")
	if(IDF_TARGET STREQUAL "esp32" OR IDF_TARGET STREQUAL "esp32s3")
		set(CHDR_MICROFLAC_ASM
			"${CHDR_MICROFLAC_SOURCE_DIR}/src/xtensa/lpc_32_xtensa.S"
			"${CHDR_MICROFLAC_SOURCE_DIR}/src/xtensa/lpc_64_xtensa.S")
	endif()
	set(CHDR_FLAC_SRCS
		"${LIBCHDR_ROOT}/src/libchdr_flac_microflac.cpp"
		${CHDR_MICROFLAC_ASM}
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/flac_decoder.cpp"
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/decorrelation.cpp"
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/frame_header.cpp"
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/pcm_packing.cpp"
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/crc.cpp"
		"${CHDR_MICROFLAC_SOURCE_DIR}/src/lpc.cpp")
else()
	set(CHDR_FLAC_SRCS "${LIBCHDR_ROOT}/src/libchdr_flac.c")
endif()

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
		${CHDR_FLAC_SRCS}
		"${LIBCHDR_ROOT}/src/libchdr_huffman.c"
		"${LIBCHDR_ROOT}/deps/lzma-26.02/src/LzmaDec.c"
		"${LIBCHDR_ROOT}/deps/miniz-3.1.2/miniz.c"
		"${LIBCHDR_ROOT}/deps/zstd-1.5.7/zstddeclib.c"
	INCLUDE_DIRS
		"${LIBCHDR_ROOT}/include"
)

if(CHDR_FLAC_BACKEND STREQUAL "microflac")
	# Only micro-flac's public include dir: its src/ holds a crc.h that will
	# shadow a consumer's own header of that name. Its sources reach their
	# private headers relative to themselves.
	target_include_directories(${COMPONENT_LIB} PRIVATE
		"${CHDR_MICROFLAC_SOURCE_DIR}/include")
	target_compile_definitions(${COMPONENT_LIB} PRIVATE
		CHDR_FLAC_BACKEND_MICROFLAC MICRO_FLAC_DISABLE_OGG)
	# Keeps libstdc++'s exception, RTTI and guard machinery out of the image.
	target_compile_options(${COMPONENT_LIB} PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
		$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
		$<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	WANT_RAW_DATA_SECTOR=1
	WANT_SUBCODE=1
	VERIFY_BLOCK_CRC=1
	LOWRAM_TARGET=${LOWRAM_TARGET_VAL}
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_DEFLATE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
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

# Passed through from idf.py -D so the configuration can be A/B'd without
# editing this file. Tested with DEFINED, not for truth: 0 is a meaningful
# value. CHDR_CD_SCRATCH_BUFFER=1 restores the per-codec hunk-sized scratch
# buffer that LOWRAM_TARGET=1 otherwise turns off - about 16.5 KB per CD
# codec, at no measured CPU cost either way.
if(DEFINED CHDR_CD_SCRATCH_BUFFER)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE CHDR_CD_SCRATCH_BUFFER=${CHDR_CD_SCRATCH_BUFFER})
endif()
