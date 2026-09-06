# The tests compile against libchdr's headers, and those headers branch on the
# build configuration (cdrom.h hides ecc_generate() when WANT_RAW_DATA_SECTOR is
# off, for one). chdr-static keeps CHDR_DEFINES PRIVATE so it does not push
# unnamespaced macros onto downstream consumers, so hand them to the test
# targets here instead - otherwise a test compiles against chdconfig.h defaults
# while the library it links was built with something else.
function(chdr_test_defines target)
	target_compile_definitions(${target} PRIVATE ${CHDR_DEFINES})
endfunction()

add_executable(chdr-benchmark benchmark.c)
target_link_libraries(chdr-benchmark PRIVATE chdr-static)
chdr_test_defines(chdr-benchmark)

# reads a CHD's hunks in a given order and dumps them to stdout, so two
# builds (e.g. LOWRAM_TARGET on vs off) can be diffed byte-for-byte instead
# of only relying on the internal per-hunk CRC check - see
# lowram-correctness.yml
add_executable(chd_dump_order chd_dump_order.c)
target_link_libraries(chd_dump_order PRIVATE chdr-static)
chdr_test_defines(chd_dump_order)

add_executable(chdr-avhuff-regression avhuff_regression.c)
target_link_libraries(chdr-avhuff-regression PRIVATE chdr-static)
chdr_test_defines(chdr-avhuff-regression)

# fuzzing
if(BUILD_FUZZER)
  add_executable(chdr-fuzz fuzz.c)
  target_link_options(chdr-fuzz PRIVATE "-fsanitize=address,fuzzer")
  target_link_libraries(chdr-fuzz PRIVATE chdr-static)
chdr_test_defines(chdr-fuzz)
  add_custom_target(fuzz
    COMMAND "$<TARGET_FILE:chdr-fuzz>" "-max_len=131072"
    DEPENDS chdr-fuzz)
endif()

# known-answer tests for the hand-optimised ECC and CRC16 primitives, whose
# output is not otherwise checked by anything that would fail loudly
add_executable(chdr-kat kat.c)
target_link_libraries(chdr-kat PRIVATE chdr-static)
chdr_test_defines(chdr-kat)
add_test(NAME known-answer COMMAND chdr-kat)

# CHDs with a parent: seeds/ has none, and cannot (a child needs its parent, and
# the workflows there open every file standalone), so COMPRESSION_PARENT would
# otherwise go untested - including through the in-place CD spread, where a
# parent-referenced hunk reaches the caller's buffer by a different route.
add_executable(chdr-parent-decode parent_decode.c)
target_link_libraries(chdr-parent-decode PRIVATE chdr-static)
chdr_test_defines(chdr-parent-decode)
add_test(NAME parent-decode
         COMMAND chdr-parent-decode "${CMAKE_CURRENT_SOURCE_DIR}/corpus/parent")
