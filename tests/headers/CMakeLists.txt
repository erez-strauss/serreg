# headers_self_contained (2.1.13, AGENTS.md §4): every header includes what it needs and is protected by
# #pragma once. One generated translation unit per header contains only that header, included twice; building the
# object libraries below is the check (a missing include or a missing #pragma once breaks the build). Each family is
# compiled with only what its users are promised: es::ipc as C++23 without reflection flags, the rest with
# es::serreg. scripts/check-includes.py checks the include order and the #pragma once line itself.
function(es_serreg_header_check target std)
  set(_tus)
  foreach(_h IN LISTS ARGN)
    file(RELATIVE_PATH _rel ${PROJECT_SOURCE_DIR} ${_h})
    string(MAKE_C_IDENTIFIER "${_rel}" _id)
    set(_tu ${CMAKE_CURRENT_BINARY_DIR}/${target}/${_id}.cpp)
    file(CONFIGURE OUTPUT ${_tu} CONTENT "// generated: ${_rel} on its own, twice\n#include \"${_h}\"\n#include \"${_h}\"\n")
    list(APPEND _tus ${_tu})
  endforeach()
  add_library(${target} OBJECT ${_tus})
  set_target_properties(${target} PROPERTIES CXX_STANDARD ${std} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  es_serreg_apply_warnings(${target})
endfunction()

file(GLOB _ipc_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/es/ipc/*.hpp)
es_serreg_header_check(serreg_headers_ipc 23 ${_ipc_headers})
target_link_libraries(serreg_headers_ipc PRIVATE es::ipc)

file(GLOB _serreg_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/es/serreg/*.hpp
                                            ${PROJECT_SOURCE_DIR}/include/es/serreg/detail/*.hpp)
es_serreg_header_check(serreg_headers_public 26 ${_serreg_headers})
target_link_libraries(serreg_headers_public PRIVATE es::serreg)

file(GLOB _internal_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.hpp)
es_serreg_header_check(serreg_headers_internal 26 ${_internal_headers})
target_link_libraries(serreg_headers_internal PRIVATE es::serreg)

file(GLOB _test_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/golden/*.hpp ${PROJECT_SOURCE_DIR}/tests/support/*.hpp
                                          ${PROJECT_SOURCE_DIR}/tests/bench/*.hpp ${PROJECT_SOURCE_DIR}/tests/dlopen/*.hpp
                                          ${PROJECT_SOURCE_DIR}/tests/fuzz/*.hpp ${PROJECT_SOURCE_DIR}/tests/link/*.hpp
                                          ${PROJECT_SOURCE_DIR}/examples/user_example1/*.hpp)
es_serreg_header_check(serreg_headers_tests 26 ${_test_headers})
target_link_libraries(serreg_headers_tests PRIVATE es::serreg es::ipc Catch2::Catch2)

# The include order and the #pragma once line (scripts/check-includes.py) as a ctest
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME headers_include_rules COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/check-includes.py)
  set_tests_properties(headers_include_rules PROPERTIES PASS_REGULAR_EXPRESSION "include rules: 0 problem")
endif()
