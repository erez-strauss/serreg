# Golden lock (spec §10.2 #1, FR-H1/H4, §6.2b; cc F1): corpus.hpp, hashes.cpp and blobs.cpp live here and are
# compiled into the serreg_tests executable (tests/unit/CMakeLists.txt) so the golden STATIC_REQUIREs share one
# multi-TU binary with the round-trip, reader, registry and capture tests.
#
# Checked-in fixtures (plan §1.6 (b)): make_fixtures regenerates orders.{ser,rri,text,json,toml} and features.ser into the build
# tree; each is compared byte for byte with tests/fixtures/golden/. To refreeze after a deliberate format change:
#   ./build/<preset>/tests/golden/make_fixtures tests/fixtures/golden     (+ CHANGELOG entry)
add_executable(make_fixtures make_fixtures.cpp)
target_link_libraries(make_fixtures PRIVATE es::serreg)
target_include_directories(make_fixtures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
es_serreg_apply_warnings(make_fixtures)

add_executable(make_dynamic_fixtures make_dynamic_fixtures.cpp)   # 2.0: dynamic.* (separate registry)
target_link_libraries(make_dynamic_fixtures PRIVATE es::serreg)
target_include_directories(make_dynamic_fixtures PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
es_serreg_apply_warnings(make_dynamic_fixtures)

set(_fx_out ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
file(MAKE_DIRECTORY ${_fx_out})
add_test(NAME golden_fixtures_generate COMMAND make_fixtures ${_fx_out})
set_tests_properties(golden_fixtures_generate PROPERTIES FIXTURES_SETUP golden_fixtures)
add_test(NAME golden_dynamic_fixtures_generate COMMAND make_dynamic_fixtures ${_fx_out})
set_tests_properties(golden_dynamic_fixtures_generate PROPERTIES FIXTURES_SETUP golden_fixtures)
add_test(NAME golden_fixture_features_byte_exact   # 1.7: CRC trailers, checkpoints with full dictionaries, writer info
         COMMAND ${CMAKE_COMMAND} -E compare_files ${_fx_out}/features.ser ${ES_SERREG_GOLDEN_FIXTURES}/features.ser)
set_tests_properties(golden_fixture_features_byte_exact PROPERTIES FIXTURES_REQUIRED golden_fixtures)
foreach(ext ser rri text json toml)
  add_test(NAME golden_fixture_${ext}_byte_exact
           COMMAND ${CMAKE_COMMAND} -E compare_files ${_fx_out}/orders.${ext} ${ES_SERREG_GOLDEN_FIXTURES}/orders.${ext})
  set_tests_properties(golden_fixture_${ext}_byte_exact PROPERTIES FIXTURES_REQUIRED golden_fixtures)
endforeach()
foreach(ext ser text json toml)   # 2.0: dynamic types (canonical images), docs/plans-for-v2.0.md §2
  add_test(NAME golden_fixture_dynamic_${ext}_byte_exact
           COMMAND ${CMAKE_COMMAND} -E compare_files ${_fx_out}/dynamic.${ext} ${ES_SERREG_GOLDEN_FIXTURES}/dynamic.${ext})
  set_tests_properties(golden_fixture_dynamic_${ext}_byte_exact PROPERTIES FIXTURES_REQUIRED golden_fixtures)
endforeach()
if(TARGET srdump)
  # the checked-in capture verifies with the shipped tool, and --json/--toml of it equal the checked-in renderings
  add_test(NAME golden_srdump_verify COMMAND srdump ${ES_SERREG_GOLDEN_FIXTURES}/orders.ser --verify)
  set_tests_properties(golden_srdump_verify PROPERTIES PASS_REGULAR_EXPRESSION "^ok: 11 frames, 34 records")
  add_test(NAME golden_srdump_verify_dynamic COMMAND srdump ${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser --verify)
  set_tests_properties(golden_srdump_verify_dynamic PROPERTIES PASS_REGULAR_EXPRESSION "^ok: 9 frames, 18 records")
  # 1.7: the features file verifies (every CRC checked) and --info follows its chains without a frame walk
  add_test(NAME golden_srdump_verify_features COMMAND srdump ${ES_SERREG_GOLDEN_FIXTURES}/features.ser --verify)
  set_tests_properties(golden_srdump_verify_features PROPERTIES PASS_REGULAR_EXPRESSION "^ok: 11 frames, 34 records")
  add_test(NAME golden_srdump_info_features COMMAND srdump ${ES_SERREG_GOLDEN_FIXTURES}/features.ser --info)
  set_tests_properties(golden_srdump_info_features PROPERTIES PASS_REGULAR_EXPRESSION "^format 1\\.7 .*flags crc checkpoints info\n.*\ncheckpoints [1-9]")

  foreach(fmtname json toml text)
    add_test(NAME golden_srdump_${fmtname}_matches_fixture
             COMMAND ${CMAKE_COMMAND} -DSRDUMP=$<TARGET_FILE:srdump> -DCAPTURE=${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
                     -DFLAG=--${fmtname} -DEXPECTED=${ES_SERREG_GOLDEN_FIXTURES}/orders.${fmtname}
                     -DOUT=${_fx_out}/srdump.${fmtname} -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_srdump.cmake)
  endforeach()
  # --program-hash of the capture equals that of the RRI
  add_test(NAME golden_srdump_program_hash_agrees
           COMMAND ${CMAKE_COMMAND} -DSRDUMP=$<TARGET_FILE:srdump> -DCAPTURE=${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
                   -DRRI=${ES_SERREG_GOLDEN_FIXTURES}/orders.rri
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_program_hash.cmake)
  # --program-hash of an ELF dictionary (bare ENTRY stream, no header) is recomputed from the type table (FR-H3);
  # a build without the section (ES_SERREG_IMAGE_FORMAT=none, 2.1.11) has no ELF dictionary to compare
  if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
    add_test(NAME golden_srdump_program_hash_elf_agrees
             COMMAND ${CMAKE_COMMAND} -DSRDUMP=$<TARGET_FILE:srdump> -DCAPTURE=${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
                     -DRRI=${ES_SERREG_GOLDEN_FIXTURES}/orders.rri -DELF=$<TARGET_FILE:make_fixtures>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/compare_program_hash.cmake)
  endif()
endif()

# 2.1.7 (review G2-18): hashes.cpp also compiles without Catch2 (-DES_SERREG_GOLDEN_STANDALONE: every STATIC_REQUIRE is a
# static_assert). CI and Compiler Explorer use that mode for a second compiler (FR-H4); this ctest keeps it compiling.
add_test(NAME golden_hashes_standalone_compiles
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 ${ES_SERREG_REFLECTION_FLAG} -fsyntax-only -DES_SERREG_GOLDEN_STANDALONE
                 -I${PROJECT_SOURCE_DIR}/include -I${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/hashes.cpp)
set_tests_properties(golden_hashes_standalone_compiles PROPERTIES TIMEOUT 300)
