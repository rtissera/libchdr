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
# 32 KB by default, as on the RP2350. Measured on the ESP32-S3 over every hunk
# of the five-image A/B set: 1.11x, and the only lever tried here that wins on
# every file (0.848x on the best, 0.952x on the worst). It costs exactly its
# 32 KB of heap, affordable only because the CD codecs no longer keep a
# hunk-sized scratch each - see CHDR_CD_SCRATCH_BUFFER.
# Pass -D BENCH_CACHE_BUDGET=0 to turn it off.
if(NOT DEFINED BENCH_CACHE_BUDGET)
	set(BENCH_CACHE_BUDGET 32768)
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
if(SD_SPI_FREQ_KHZ)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE SD_SPI_FREQ_KHZ=${SD_SPI_FREQ_KHZ})
endif()

# five-image subset, for A/B-ing a change in ~17 minutes instead of two hours
if(BENCH_AB)
	target_compile_definitions(${COMPONENT_LIB} PRIVATE BENCH_AB=1)
endif()
