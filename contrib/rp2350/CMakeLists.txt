# libchdr benchmark for the Waveshare RP2350-PiZero (RP2350B, Cortex-M33).
#
# Needs the Pico SDK and carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico:
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   export SDFAT_PATH=/path/to/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
#   cmake -S contrib/rp2350 -B build-rp2350 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-rp2350 -j
# then copy build-rp2350/rp2350-bench.uf2 onto the board in BOOTSEL.
#
# Neither dependency is vendored: this is a test harness, and libchdr's tree
# stays free of them.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(rp2350-bench C CXX ASM)
pico_sdk_init()

if(NOT DEFINED ENV{SDFAT_PATH})
	message(FATAL_ERROR "SDFAT_PATH is not set - see the header of this file")
endif()
add_subdirectory($ENV{SDFAT_PATH}/src build_sdfat)

get_filename_component(LIBCHDR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# LOWRAM_TARGET=1 by default: measured on this board it costs 1.00x on the CD
# codecs and cuts peak heap 271,528 -> 111,708 bytes.
if(NOT DEFINED LOWRAM_TARGET_VAL)
	set(LOWRAM_TARGET_VAL 1)
endif()
if(NOT DEFINED BENCH_HUNK_CAP)
	set(BENCH_HUNK_CAP 0)
endif()
if(NOT DEFINED SD_BAUD_HZ)
	set(SD_BAUD_HZ 25000000)
endif()


# FLAC backend. micro-flac is the default here, unlike libchdr itself: it is
# measurably faster on every board this was tried on (1.198x on a CD-FLAC hunk
# with I/O excluded on an ESP32-S3, 1.12-1.14x on two real discs here), its
# peak heap is lower on most images, and its worst-case largest-free-block is
# better - and an MCU integrator is already cloning an SDK and a toolchain, so
# one more pinned checkout costs them nothing. libchdr's own default stays
# dr_flac, because a desktop consumer vendoring src/ must not have to fetch
# anything.
#
# Set CHDR_FLAC_BACKEND=drflac to go back, e.g. if Apache-2.0 does not suit
# (micro-flac is Apache-2.0; libchdr stays BSD-3 because this is fetched, never
# vendored).
if(NOT DEFINED CHDR_FLAC_BACKEND)
	set(CHDR_FLAC_BACKEND microflac)
endif()
set(CHDR_MICROFLAC_GIT_TAG "ffbe8a9ba5e78c16e535a4695a8b2418b0c091ee"
	CACHE STRING "micro-flac commit to fetch when no local checkout is given")
if(CHDR_FLAC_BACKEND STREQUAL "microflac")
	enable_language(CXX)
	if(NOT CHDR_MICROFLAC_SOURCE_DIR)
		# Same shape as libchdr's own root CMakeLists: pinned SHA, because the
		# backend compiles against micro-flac's internals.
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
	set(CHDR_FLAC_SRC
		${LIBCHDR_ROOT}/src/libchdr_flac_microflac.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/flac_decoder.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/decorrelation.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/frame_header.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/pcm_packing.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/crc.cpp
		${CHDR_MICROFLAC_SOURCE_DIR}/src/lpc.cpp)
else()
	set(CHDR_FLAC_SRC ${LIBCHDR_ROOT}/src/libchdr_flac.c)
endif()

add_executable(rp2350-bench
	benchmark_main.c
	hw_config.c
	${CHDR_FLAC_SRC}
	${LIBCHDR_ROOT}/src/libchdr_bitstream.c
	${LIBCHDR_ROOT}/src/libchdr_cdrom.c
	${LIBCHDR_ROOT}/src/libchdr_chd.c
	${LIBCHDR_ROOT}/src/libchdr_codec_avhuff.c
	${LIBCHDR_ROOT}/src/libchdr_codec_cdfl.c
	${LIBCHDR_ROOT}/src/libchdr_codec_cdlz.c
	${LIBCHDR_ROOT}/src/libchdr_codec_cdzl.c
	${LIBCHDR_ROOT}/src/libchdr_codec_cdzs.c
	${LIBCHDR_ROOT}/src/libchdr_codec_flac.c
	${LIBCHDR_ROOT}/src/libchdr_codec_huff.c
	${LIBCHDR_ROOT}/src/libchdr_codec_lzma.c
	${LIBCHDR_ROOT}/src/libchdr_codec_zlib.c
	${LIBCHDR_ROOT}/src/libchdr_codec_zstd.c
	${LIBCHDR_ROOT}/src/libchdr_huffman.c
	${LIBCHDR_ROOT}/deps/lzma-26.02/src/LzmaDec.c
	${LIBCHDR_ROOT}/deps/miniz-3.1.2/miniz.c
	${LIBCHDR_ROOT}/deps/zstd-1.5.7/zstddeclib.c
)

target_include_directories(rp2350-bench PRIVATE
	${LIBCHDR_ROOT}/include
	${LIBCHDR_ROOT}/src
	${CMAKE_CURRENT_LIST_DIR}
)
if(CHDR_FLAC_BACKEND STREQUAL "microflac")
	# Only micro-flac's public include dir goes on the target path. Its src/
	# dir must NOT: it holds a crc.h that shadows the SD driver's own, and a C
	# translation unit then pulls in a C++ header. micro-flac's sources find
	# their private headers relative to themselves.
	target_include_directories(rp2350-bench PRIVATE
		${CHDR_MICROFLAC_SOURCE_DIR}/include)
	target_compile_definitions(rp2350-bench PRIVATE
		CHDR_FLAC_BACKEND_MICROFLAC MICRO_FLAC_DISABLE_OGG)
	target_compile_options(rp2350-bench PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
		$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
		$<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
endif()

target_compile_definitions(rp2350-bench PRIVATE
	WANT_RAW_DATA_SECTOR=1
	WANT_SUBCODE=1
	VERIFY_BLOCK_CRC=1
	LOWRAM_TARGET=${LOWRAM_TARGET_VAL}
	BENCH_HUNK_CAP=${BENCH_HUNK_CAP}
	SD_BAUD_HZ=${SD_BAUD_HZ}
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_DEFLATE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
)

target_compile_options(rp2350-bench PRIVATE -O3)
target_link_libraries(rp2350-bench
	pico_stdlib
	hardware_clocks
	no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)
pico_enable_stdio_usb(rp2350-bench 1)
pico_enable_stdio_uart(rp2350-bench 0)
pico_add_extra_outputs(rp2350-bench)
