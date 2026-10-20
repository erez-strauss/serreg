# 2.2.1 (maintainer): the Compiler Explorer examples are part of every build/test run — each is compiled as a
# single translation unit against its own header symlinks (ce-examples/ce-exampleN/es → include/), with the flags
# the maintainer uses on the command line (-O1 -W -Wall -Wextra -Wshadow -Werror), then run; the golden checks
# inside them must report no mismatch. A header added to the library needs a new symlink in every example that
# includes it (derive with `g++ -MM`; ce-examples/README.md), or the matching test fails here first.
set(_ce_flags "-std=c++26 ${ES_SERREG_REFLECTION_FLAG}")
set(_ce_expect_0 "sym @20 = AAPL")
set(_ce_expect_1 "0 golden mismatches")
set(_ce_expect_2 "0 golden mismatches")
set(_ce_expect_3 "0 mismatches")
set(_ce_expect_4 "0 mismatches")
set(_ce_expect_5 "ok: 1000 messages")
set(_ce_expect_6 "typed read: .*identical")
set(_ce_expect_7 "log frames: 3 written, 3 read back, trace argument evaluated 0 times")
foreach(n 0 1 2 3 4 5 6 7)
  add_test(NAME ce_example${n}_builds_and_runs
           COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER} "-DFLAGS=${_ce_flags}"
                   -DDIR=${PROJECT_SOURCE_DIR}/ce-examples/ce-example${n}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ce${n} "-DEXPECT=${_ce_expect_${n}}"
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/build_and_run.cmake)
  set_tests_properties(ce_example${n}_builds_and_runs PROPERTIES TIMEOUT 600 LABELS ce)
endforeach()
