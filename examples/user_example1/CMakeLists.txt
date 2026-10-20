# examples/user_example1 — writer / reader1 / reader2 / reader3 (v2.0.6; simplified in 2.1.10). Builds inside the ser-reg tree (added by
# examples/CMakeLists.txt) or standalone against an installed ser-reg:
#   cmake -S examples/user_example1 -B build-ue1 -G Ninja -DCMAKE_PREFIX_PATH=<ser-reg install prefix>
#   cmake --build build-ue1 && build-ue1/writer && build-ue1/reader1 && build-ue1/reader2 && build-ue1/reader3
cmake_minimum_required(VERSION 4.0)
project(serreg_user_example1 LANGUAGES CXX)
if(NOT TARGET es::serreg)
  find_package(serreg CONFIG REQUIRED)
endif()

foreach(prog writer reader1 reader2 reader3)
  add_executable(user_example1_${prog} ${prog}.cpp)
  set_target_properties(user_example1_${prog} PROPERTIES OUTPUT_NAME ${prog})
  target_link_libraries(user_example1_${prog} PRIVATE es::serreg)
  if(COMMAND es_serreg_apply_warnings)
    es_serreg_apply_warnings(user_example1_${prog})
  endif()
endforeach()

if(ES_SERREG_BUILD_TESTS)
  set(_ue1_file ${CMAKE_CURRENT_BINARY_DIR}/user_example1.ser)
  add_test(NAME user_example1_writer COMMAND user_example1_writer ${_ue1_file})
  add_test(NAME user_example1_reader1 COMMAND user_example1_reader1 ${_ue1_file})
  add_test(NAME user_example1_reader1_blocks COMMAND user_example1_reader1 --blocks ${_ue1_file})
  add_test(NAME user_example1_reader2 COMMAND user_example1_reader2 ${_ue1_file})
  add_test(NAME user_example1_reader2_blocks COMMAND user_example1_reader2 --blocks ${_ue1_file})
  add_test(NAME user_example1_reader3 COMMAND user_example1_reader3 ${_ue1_file})
  add_test(NAME user_example1_reader3_blocks COMMAND user_example1_reader3 --blocks ${_ue1_file})
  set_tests_properties(user_example1_writer PROPERTIES FIXTURES_SETUP ue1 PASS_REGULAR_EXPRESSION "^wrote ")
  set_tests_properties(user_example1_reader1 user_example1_reader1_blocks PROPERTIES FIXTURES_REQUIRED ue1
                       PASS_REGULAR_EXPRESSION "reader1: 1 book, 3 orders, 3 fills, 2 other log lines, matches the writer")
  set_tests_properties(user_example1_reader2 user_example1_reader2_blocks PROPERTIES FIXTURES_REQUIRED ue1
                       PASS_REGULAR_EXPRESSION "reader2: 9 frames, 28 records, [0-9]+ values, [0-9]+ types in the dictionary, 0 errors")
  set_tests_properties(user_example1_reader3 user_example1_reader3_blocks PROPERTIES FIXTURES_REQUIRED ue1
                       PASS_REGULAR_EXPRESSION "reader3: 6 typed orders \\(6 fills inside them\\)")
endif()
