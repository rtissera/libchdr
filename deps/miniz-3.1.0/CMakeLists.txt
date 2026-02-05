add_library(miniz STATIC
  miniz.c
  miniz.h
)

set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Disable unused features.
target_compile_definitions(miniz PUBLIC
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
)
