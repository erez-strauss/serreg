# Benchmarks (cc §6): built by default, run by ctest under the label `bench` (exclude with `ctest -LE bench`).
# Always optimised: the ceilings are about the hot path, not about the build type of the test suite.
add_executable(serreg_bench bench.cpp)
target_link_libraries(serreg_bench PRIVATE es::serreg Catch2::Catch2WithMain)
target_include_directories(serreg_bench PRIVATE ${ES_SERREG_CORPUS_DIR})
target_compile_options(serreg_bench PRIVATE -O2)
target_compile_definitions(serreg_bench PRIVATE $<$<CONFIG:Debug>:ES_SERREG_BENCH_DEBUG_LIB>)
es_serreg_apply_warnings(serreg_bench)
add_test(NAME bench_hot_path_ceilings COMMAND serreg_bench --benchmark-samples 20 --benchmark-no-analysis)
set_tests_properties(bench_hot_path_ceilings PROPERTIES LABELS bench TIMEOUT 300)

# v2.0.4: half-RTT benchmarks — two processes, two es::ipc SPMC rings (include/es/ipc/; the variable-size rings
# since 2.1.13a, `--ring spsc` for SPSC queues); raw packets
# vs es::serreg frames. Run for numbers with scripts/bench-hrtt-raw.sh / scripts/bench-hrtt-serreg.sh (release
# preset, pinned CPUs, dated report in logs/). Here: functional smoke tests only (every reply arrives, in order,
# nobody lapped) — gating, not labelled bench; no timing is checked.
foreach(kind raw serreg)
  add_executable(serreg_hrtt_${kind} hrtt_${kind}.cpp)
  target_link_libraries(serreg_hrtt_${kind} PRIVATE es::serreg es::ipc)
  es_serreg_apply_warnings(serreg_hrtt_${kind})
  foreach(mode process thread)
    add_test(NAME hrtt_${kind}_smoke_${mode}
             COMMAND serreg_hrtt_${kind} --mode ${mode} --iters 2000 --warmup 200 --timeout 60 --quiet)
    set_tests_properties(hrtt_${kind}_smoke_${mode} PROPERTIES PASS_REGULAR_EXPRESSION "ok: 2000 round trips"
                         TIMEOUT 120)
  endforeach()
endforeach()

# 2.1.12 (cc review 2.1.10, C-08): serialize() leaves no call at -O2 (the fill lambda is always_inline)
add_test(NAME serialize_inlines_at_O2
         COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
                 "-DFLAGS=-std=c++26 ${ES_SERREG_REFLECTION_FLAG} -DES_SERREG_SHARED -I${PROJECT_SOURCE_DIR}/include -I${ES_SERREG_CORPUS_DIR}"
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/hot_inline.cpp -DOUT=${CMAKE_CURRENT_BINARY_DIR}/hot_inline.s
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_inline.cmake)
set_tests_properties(serialize_inlines_at_O2 PROPERTIES TIMEOUT 120)


# 2.2.1 (DECISIONS G-5): the registry at scale — push, publish, lookup, export, parse for N synthetic entries of
# B bytes; the 10k/10 MB/10 ms and 100k/100 MB/100 ms budgets are enforced in release builds only (--assert),
# reported elsewhere. Numbers for the record come from the reference box (docs/ser-reg-architecture.md).
add_executable(registry_measure registry_measure.cpp)
target_link_libraries(registry_measure PRIVATE es::serreg_tools)
target_compile_options(registry_measure PRIVATE -O2)
target_compile_definitions(registry_measure PRIVATE $<$<CONFIG:Debug>:ES_SERREG_BENCH_DEBUG_LIB>)
es_serreg_apply_warnings(registry_measure)
add_test(NAME registry_measure_10k COMMAND registry_measure --entries 10000 --bytes 10485760 --assert)
add_test(NAME registry_measure_100k COMMAND registry_measure --entries 100000 --bytes 104857600 --assert)
set_tests_properties(registry_measure_10k registry_measure_100k PROPERTIES LABELS bench TIMEOUT 600)
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf") # the reader side needs the es_serreg_dict section
  add_test(NAME registry_measure_elf COMMAND registry_measure --elf $<TARGET_FILE:serreg_bench>)
  set_tests_properties(registry_measure_elf PROPERTIES PASS_REGULAR_EXPRESSION "types, [0-9]+ literals" LABELS bench)
endif()
