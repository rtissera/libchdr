idf_component_register(
	SRCS "benchmark_main.c"
	INCLUDE_DIRS "."
	REQUIRES libchdr
	PRIV_REQUIRES esp_timer fatfs sdmmc esp_driver_sdmmc
)

# single-file bisect mode: idf.py -DBENCH_ONE_FILE='"/sdcard/path.chd"'
if(BENCH_ONE_FILE)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_ONE_FILE=${BENCH_ONE_FILE})
endif()
if(BENCH_PROGRESS_EVERY)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_PROGRESS_EVERY=${BENCH_PROGRESS_EVERY})
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
if(BENCH_FATFS_BACKEND)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_FATFS_BACKEND=1)
endif()
