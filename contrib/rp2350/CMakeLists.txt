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

# The LZMA SDK ships decoder assembly for x86-64 and AArch64 only; Cortex-M33
# is ARMv8-M Thumb, so neither applies. Z7_LZMA_PROB32 is the one portable
# knob: it widens the probability model from UInt16 to UInt32, which the SDK
# notes "can increase the speed on some CPUs" at double the probs memory
# (+15,980 bytes per LZMA instance at CHD's lc=3/lp=0). Off by default.
option(CHDR_LZMA_PROB32 "Use 32-bit LZMA probability model" OFF)

add_executable(rp2350-bench
	benchmark_main.c
	hw_config.c
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
	${LIBCHDR_ROOT}/src/libchdr_flac.c
	${LIBCHDR_ROOT}/src/libchdr_huffman.c
	${LIBCHDR_ROOT}/deps/lzma-26.02/src/LzmaDec.c
	${LIBCHDR_ROOT}/deps/miniz-3.1.2/miniz.c
	${LIBCHDR_ROOT}/deps/zstd-1.5.7/zstddeclib.c
)

target_include_directories(rp2350-bench PRIVATE
	${LIBCHDR_ROOT}/include
	${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(rp2350-bench PRIVATE
	WANT_RAW_DATA_SECTOR=1
	WANT_SUBCODE=1
	VERIFY_BLOCK_CRC=1
	LOWRAM_TARGET=${LOWRAM_TARGET_VAL}
	BENCH_HUNK_CAP=${BENCH_HUNK_CAP}
	MINIZ_NO_ARCHIVE_APIS
	MINIZ_NO_DEFLATE_APIS
	MINIZ_NO_STDIO
	MINIZ_NO_TIME
)

if(CHDR_LZMA_PROB32)
	set_source_files_properties(${LIBCHDR_ROOT}/deps/lzma-26.02/src/LzmaDec.c
		PROPERTIES COMPILE_DEFINITIONS Z7_LZMA_PROB32)
endif()

target_compile_options(rp2350-bench PRIVATE -O3)
target_link_libraries(rp2350-bench
	pico_stdlib
	hardware_clocks
	no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)
pico_enable_stdio_usb(rp2350-bench 1)
pico_enable_stdio_uart(rp2350-bench 0)
pico_add_extra_outputs(rp2350-bench)
