# Link-model tests (spec §10.2 #3): two-TU dedup (FR-R2), count at main() (FR-R3), --gc-sections survival of the
# registry section (D6, §6.8), -flto, and registration-order independence of program_hash (FR-H3).
include(CheckIPOSupported)

function(es_serreg_link_exe name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE es::serreg)
  es_serreg_apply_warnings(${name})
endfunction()

# (0) B3 (2.2.6, A-16): ES_SERREG_VALIDATE_READ is a whole-program switch with a link-time fuse (registry.hpp:
# detail::validate_read_build<V>, defined in src/registry.cpp for the library's value only). Two ctests compile and
# link one consumer TU against the built libserreg with the compiler itself (the exact flags of this build): with
# the library's value it links and runs; with the other value the link must fail on the fuse symbol. Works for a
# shared and a static libserreg and under every preset (the consumer gets this build's CMAKE_CXX_FLAGS and linker
# flags, sanitizers included).
set(_fuse_src ${CMAKE_CURRENT_SOURCE_DIR}/validate_read_consumer.cpp)
set(_fuse_flags -std=c++26 ${ES_SERREG_REFLECTION_FLAG} -I${CMAKE_CURRENT_SOURCE_DIR} -I${PROJECT_SOURCE_DIR}/include)
if(ES_SERREG_BUILD_SHARED)
  list(APPEND _fuse_flags -DES_SERREG_SHARED)
endif()
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND _fuse_flags -fno-gnu-unique)
  endif()
else()
  list(APPEND _fuse_flags -DES_SERREG_EMBED_NONE)
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _fuse_cfg)
separate_arguments(_fuse_cxx_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_fuse_cfg}}")
separate_arguments(_fuse_ld_flags NATIVE_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
set(_fuse_libs $<TARGET_FILE:serreg> -pthread)
if(CMAKE_DL_LIBS)
  list(APPEND _fuse_libs -l${CMAKE_DL_LIBS})
endif()
# the library's value: what src/registry.cpp was compiled with (reflect.hpp's default unless the build set it)
set(_fuse_lib_value 0)
if(CMAKE_CXX_FLAGS MATCHES "-DES_SERREG_VALIDATE_READ=([0-9]+)")
  set(_fuse_lib_value ${CMAKE_MATCH_1})
endif()
if(_fuse_lib_value EQUAL 0)
  set(_fuse_other_value 1)
else()
  set(_fuse_other_value 0)
endif()
add_test(NAME validate_read_match_links
         COMMAND ${CMAKE_CXX_COMPILER} ${_fuse_flags} ${_fuse_cxx_flags} -DES_SERREG_VALIDATE_READ=${_fuse_lib_value}
                 ${_fuse_src} -o ${CMAKE_CURRENT_BINARY_DIR}/validate_read_match ${_fuse_ld_flags} ${_fuse_libs})
add_test(NAME validate_read_mismatch_fails_to_link
         COMMAND ${CMAKE_CXX_COMPILER} ${_fuse_flags} ${_fuse_cxx_flags} -DES_SERREG_VALIDATE_READ=${_fuse_other_value}
                 ${_fuse_src} -o ${CMAKE_CURRENT_BINARY_DIR}/validate_read_mismatch ${_fuse_ld_flags} ${_fuse_libs})
# passes only when the linker names the fuse symbol (GNU ld: "undefined reference to"; lld: "undefined symbol")
set_tests_properties(validate_read_mismatch_fails_to_link PROPERTIES
  PASS_REGULAR_EXPRESSION "undefined (reference to|symbol:).*validate_read_build<${_fuse_other_value}>")

if(NOT ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
  # 2.1.11, ES_SERREG_IMAGE_FORMAT=none (preset gcc16-static-none): the library itself has the null backend, so the
  # null-backend test links against it directly; the ELF-specific checks below (image bases from dladdr, the
  # es_serreg_dict section under --gc-sections / LTO / no-PIE) do not apply.
  es_serreg_link_exe(link_image_none image_none_main.cpp tu_a.cpp tu_b.cpp)
  add_test(NAME link_image_none_backend COMMAND link_image_none)
  return()
endif()

# (a) two TUs, both serialize lnk::Msg, tu_a also lnk::Extra → 5 distinct entries
es_serreg_link_exe(link_plain main.cpp tu_a.cpp tu_b.cpp)
add_test(NAME link_two_tu_dedup COMMAND link_plain ${CMAKE_CURRENT_BINARY_DIR}/hash_plain.txt)
set_tests_properties(link_two_tu_dedup PROPERTIES FIXTURES_SETUP link_hash_plain)

# (a') a TU that registers lnk::Extra first is linked ahead of the others → different registration order.
es_serreg_link_exe(link_alt_order tu_extra_first.cpp main.cpp tu_a.cpp tu_b.cpp)
add_test(NAME link_alt_registration_order COMMAND link_alt_order ${CMAKE_CURRENT_BINARY_DIR}/hash_alt.txt)
set_tests_properties(link_alt_registration_order PROPERTIES FIXTURES_SETUP link_hash_alt)
add_test(NAME link_program_hash_order_independent
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/hash_plain.txt ${CMAKE_CURRENT_BINARY_DIR}/hash_alt.txt)
set_tests_properties(link_program_hash_order_independent PROPERTIES FIXTURES_REQUIRED "link_hash_plain;link_hash_alt")

# (a'') the same two TUs in reverse link order (the TU with the subset of blobs first)
es_serreg_link_exe(link_reversed main.cpp tu_b.cpp tu_a.cpp)
add_test(NAME link_reversed_order COMMAND link_reversed ${CMAKE_CURRENT_BINARY_DIR}/hash_reversed.txt)
set_tests_properties(link_reversed_order PROPERTIES FIXTURES_SETUP link_hash_reversed)
add_test(NAME link_program_hash_reversed_equal
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/hash_plain.txt ${CMAKE_CURRENT_BINARY_DIR}/hash_reversed.txt)
set_tests_properties(link_program_hash_reversed_equal PROPERTIES FIXTURES_REQUIRED "link_hash_plain;link_hash_reversed")

# (b) --gc-sections: the [[gnu::retain]] section and the guards must survive
es_serreg_link_exe(link_gc main.cpp tu_a.cpp tu_b.cpp)
target_compile_options(link_gc PRIVATE -ffunction-sections -fdata-sections)
target_link_options(link_gc PRIVATE -Wl,--gc-sections)
add_test(NAME link_gc_sections COMMAND link_gc)

# (b') non-PIE executable (cc F10): main_image must equal dladdr(&main).dli_fbase, which for a non-PIE binary is
# the fixed link address rather than the (zero) load bias. main.cpp asserts images() contains it.
es_serreg_link_exe(link_nopie main.cpp tu_a.cpp tu_b.cpp)
target_compile_options(link_nopie PRIVATE -fno-pie)
target_link_options(link_nopie PRIVATE -no-pie)
set_target_properties(link_nopie PROPERTIES POSITION_INDEPENDENT_CODE OFF)
add_test(NAME link_no_pie_main_image COMMAND link_nopie ${CMAKE_CURRENT_BINARY_DIR}/hash_nopie.txt)
set_tests_properties(link_no_pie_main_image PROPERTIES FIXTURES_SETUP link_hash_nopie)
add_test(NAME link_no_pie_program_hash_equal
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/hash_plain.txt ${CMAKE_CURRENT_BINARY_DIR}/hash_nopie.txt)
set_tests_properties(link_no_pie_program_hash_equal PROPERTIES FIXTURES_REQUIRED "link_hash_plain;link_hash_nopie")

# (c) -flto
check_ipo_supported(RESULT es_serreg_ipo_ok OUTPUT es_serreg_ipo_msg LANGUAGES CXX)
if(es_serreg_ipo_ok)
  es_serreg_link_exe(link_lto main.cpp tu_a.cpp tu_b.cpp)
  set_property(TARGET link_lto PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
  add_test(NAME link_lto COMMAND link_lto)
else()
  message(STATUS "es::serreg: LTO not available for this toolchain, link_lto test disabled: ${es_serreg_ipo_msg}")
  add_test(NAME link_lto COMMAND ${CMAKE_COMMAND} -E echo "LTO unsupported by toolchain")
  set_tests_properties(link_lto PROPERTIES DISABLED TRUE)
endif()

# (d) A4 (2.1.8): the null image backend. A private static libserreg with src/image_none.cpp and no embedded section,
# built only in Debug next to the default ELF library (one extra library build; the gcc16-static and release presets
# skip it). Same TUs, same program_hash as link_plain.
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(es_serreg_none_sources ${ES_SERREG_SOURCES})
  list(TRANSFORM es_serreg_none_sources REPLACE "image_elf" "image_none")
  list(TRANSFORM es_serreg_none_sources PREPEND ${PROJECT_SOURCE_DIR}/)
  add_library(serreg_image_none STATIC ${es_serreg_none_sources})
  target_include_directories(serreg_image_none PUBLIC ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(serreg_image_none PUBLIC cxx_std_26)
  target_compile_options(serreg_image_none PUBLIC ${ES_SERREG_REFLECTION_FLAG})   # no -fno-gnu-unique: ELF only (2.1.11)
  target_compile_definitions(serreg_image_none PUBLIC ES_SERREG_EMBED_NONE)
  target_link_libraries(serreg_image_none PUBLIC Threads::Threads)
  es_serreg_apply_warnings(serreg_image_none)

  add_executable(link_image_none image_none_main.cpp tu_a.cpp tu_b.cpp)
  target_link_libraries(link_image_none PRIVATE serreg_image_none)
  es_serreg_apply_warnings(link_image_none)
  add_test(NAME link_image_none_backend COMMAND link_image_none ${CMAKE_CURRENT_BINARY_DIR}/hash_none.txt)
  set_tests_properties(link_image_none_backend PROPERTIES FIXTURES_SETUP link_hash_none)
  add_test(NAME link_image_none_program_hash_equal
           COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/hash_plain.txt ${CMAKE_CURRENT_BINARY_DIR}/hash_none.txt)
  set_tests_properties(link_image_none_program_hash_equal PROPERTIES FIXTURES_REQUIRED "link_hash_plain;link_hash_none")
endif()
