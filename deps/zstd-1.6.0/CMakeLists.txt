add_library(zstd STATIC
  zstd.h
  zstd_errors.h
  zstddeclib.c
)

# Prevent symbols being exported, to improve optimistation.
target_compile_definitions(zstd PUBLIC
  ZSTDLIB_VISIBLE=ZSTDLIB_HIDDEN
  ZSTDERRORLIB_VISIBLE=ZSTDERRORLIB_HIDDEN
)

set_target_properties(zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
