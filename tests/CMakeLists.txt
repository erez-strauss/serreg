# Catch2 v3 is test-only (spec §10.1, D4). Set ES_SERREG_CATCH2_DIR to a local checkout for offline builds.
include(FetchContent)
if(ES_SERREG_CATCH2_DIR)
  add_subdirectory(${ES_SERREG_CATCH2_DIR} catch2)
else()
  FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.8.1 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
endif()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

set(ES_SERREG_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/golden)
set(ES_SERREG_TEST_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/support)   # test_support.hpp (helpers + KNOWN-BUG workarounds)
set(ES_SERREG_GOLDEN_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden)

# Runtime layer self-test (plain asserts, no framework; written against hand-built ENTRY streams).
add_executable(runtime_selftest unit/runtime_selftest.cpp)
target_link_libraries(runtime_selftest PRIVATE es::serreg_tools) # CLI, Wireshark and diff checks (2.1.13: own library)
es_serreg_apply_warnings(runtime_selftest)
add_test(NAME runtime_selftest COMMAND runtime_selftest ${CMAKE_CURRENT_BINARY_DIR}/selftest.ser)

# 2.2.1 (DECISIONS G-2): the SONAME carries major.minor — the minor is the compatibility unit until 3.0
if(ES_SERREG_BUILD_SHARED)
  add_test(NAME soname_major_minor COMMAND ${CMAKE_COMMAND} -E echo "$<TARGET_SONAME_FILE_NAME:serreg> $<TARGET_SONAME_FILE_NAME:serreg_tools>")
  set_tests_properties(soname_major_minor PROPERTIES PASS_REGULAR_EXPRESSION
    "^libserreg\\.so\\.${PROJECT_VERSION_MAJOR}\\.${PROJECT_VERSION_MINOR} libserreg_tools\\.so\\.${PROJECT_VERSION_MAJOR}\\.${PROJECT_VERSION_MINOR}")
endif()

add_subdirectory(headers)   # 2.1.13: every header self-contained, include rules
add_subdirectory(unit)
add_subdirectory(golden)
add_subdirectory(link)
add_subdirectory(dlopen)
add_subdirectory(canary)
add_subdirectory(negative)
add_subdirectory(probe)
add_subdirectory(bench)
add_subdirectory(fuzz)
add_subdirectory(wireshark)
add_subdirectory(cxxgen)     # 2.1.16 (A5): srdump --cxx output compiles and decodes the golden captures
add_subdirectory(python)     # 2.2.1: the pure-Python reader (python/serreg) decodes what srdump decodes
add_subdirectory(ce)         # 2.2.1: the ce-examples compile (-Wshadow -Werror) and run against this tree's headers
