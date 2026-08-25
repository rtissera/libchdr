cmake_minimum_required(VERSION 3.15)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CONFIG_FREERTOS 1)
set(CONFIG_CHERRYUSB 1)
set(CONFIG_CHERRYUSB_HOST 1)
set(CONFIG_BSP_SDH_SDCARD 1)
set(CONFIG_FATFS 1)
set(CONFIG_FATFS_USBH 1)
set(CONFIG_FATFS_SDH_SDCARD 1)

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138k")
    add_definitions(-DTANG_MEGA138K)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
endif()

# libchdr: LOWRAM_TARGET on (real ~365KB free SRAM budget on this board, see
# project memory - whole-hunk decode does not fit, streaming-decode design does)
add_definitions(
    -DLOWRAM_TARGET=1
    -DWANT_RAW_DATA_SECTOR=1
    -DWANT_SUBCODE=1
    -DVERIFY_BLOCK_CRC=1
    -DMINIZ_NO_ARCHIVE_APIS
    -DMINIZ_NO_DEFLATE_APIS
    -DMINIZ_NO_STDIO
    -DMINIZ_NO_TIME
)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Add include directories
sdk_add_include_directories(
    . usb fpga ui core utils chd
    thirdparty/libchdr/include
    thirdparty/libchdr/src
)

# Add source files
file(GLOB_RECURSE MY_SOURCES
    "usb/*.cpp"
    "fpga/*.cpp"
    "ui/*.cpp"
    "core/*.cpp"
    "utils/*.cpp"
)

file(GLOB LIBCHDR_SOURCES
    "chd/*.c"
    "thirdparty/libchdr/src/*.c"
    "thirdparty/libchdr/deps/lzma-26.02/src/LzmaDec.c"
    "thirdparty/libchdr/deps/miniz-3.1.2/miniz.c"
    "thirdparty/libchdr/deps/zstd-1.5.7/zstddeclib.c"
)

target_sources(app PRIVATE ${MY_SOURCES} ${LIBCHDR_SOURCES})
sdk_set_main_file(main.cpp)

project(tangcore CXX)

