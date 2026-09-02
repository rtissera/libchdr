idf_component_register(
	SRCS "benchmark_main.c"
	INCLUDE_DIRS "."
	REQUIRES libchdr
	PRIV_REQUIRES esp_timer fatfs sdmmc esp_driver_sdmmc
)

# Pass C lever sweep selector: idf.py -DBENCH_MODE_LEVER=1 builds the small
# 4-file capped subset used for config comparisons instead of the full
# characterized Pass A sample.
if(BENCH_MODE_LEVER)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_MODE_LEVER=1)
endif()

# Pass D: idf.py -DCHDR_PROFILE_CDFL=1 turns on libchdr's cdfl stage profiler
# (needs the same define on the libchdr component, see its CMakeLists).
if(CHDR_PROFILE_CDFL)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE CHDR_PROFILE_CDFL=1)
endif()
if(SEEK_ELISION)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEK_ELISION=1)
endif()

# single-file bisect mode: idf.py -DBENCH_ONE_FILE='"/sdcard/path.chd"'
if(BENCH_ONE_FILE)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_ONE_FILE=${BENCH_ONE_FILE})
endif()
if(BENCH_PROGRESS_EVERY)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_PROGRESS_EVERY=${BENCH_PROGRESS_EVERY})
endif()
if(BENCH_FSINFO)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_FSINFO=${BENCH_FSINFO})
endif()
if(BENCH_HUNK_CAP)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_HUNK_CAP=${BENCH_HUNK_CAP})
endif()
if(BENCH_CACHE_BUDGET)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_CACHE_BUDGET=${BENCH_CACHE_BUDGET})
endif()
if(BENCH_QUICK)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_QUICK=1)
endif()
if(BENCH_RAWIO)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_RAWIO=1)
endif()
if(BENCH_FATFS_BACKEND)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_FATFS_BACKEND=1)
endif()
