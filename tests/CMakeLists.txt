add_executable(chdr-benchmark benchmark.c)
target_link_libraries(chdr-benchmark PRIVATE chdr-static)

# reads a CHD's hunks in a given order and dumps them to stdout, so two
# builds (e.g. LOWRAM_TARGET on vs off) can be diffed byte-for-byte instead
# of only relying on the internal per-hunk CRC check - see
# lowram-correctness.yml
add_executable(chd_dump_order chd_dump_order.c)
target_link_libraries(chd_dump_order PRIVATE chdr-static)

add_executable(chdr-avhuff-regression avhuff_regression.c)
target_link_libraries(chdr-avhuff-regression PRIVATE chdr-static)

# fuzzing
if(BUILD_FUZZER)
  add_executable(chdr-fuzz fuzz.c)
  target_link_options(chdr-fuzz PRIVATE "-fsanitize=address,fuzzer")
  target_link_libraries(chdr-fuzz PRIVATE chdr-static)
  add_custom_target(fuzz
    COMMAND "$<TARGET_FILE:chdr-fuzz>" "-max_len=131072"
    DEPENDS chdr-fuzz)
endif()

# known-answer tests for the hand-optimised ECC and CRC16 primitives, whose
# output is not otherwise checked by anything that would fail loudly
add_executable(chdr-kat kat.c)
target_link_libraries(chdr-kat PRIVATE chdr-static)
add_test(NAME known-answer COMMAND chdr-kat)
