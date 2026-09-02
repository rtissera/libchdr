idf_component_register(
	SRCS "benchmark_main.c"
	INCLUDE_DIRS "."
	REQUIRES libchdr
	PRIV_REQUIRES esp_timer fatfs sdmmc esp_driver_sdmmc
)
