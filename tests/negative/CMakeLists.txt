# Negative compile tests (spec §10.2 #2, FR-S7, §6.2b, gk 1.14/1.17, cc F4): each .cpp must FAIL to compile and the
# failure must carry the expected diagnostic text. Checked twice: at configure time with try_compile (a file that
# unexpectedly compiles is a configure error), and as a ctest that re-runs the compiler and matches the diagnostic
# (so the result is visible in ctest and re-checked on every run).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)   # compile only: the diagnostics are compile-time, nothing links
set(_neg_flags -std=c++26 ${ES_SERREG_REFLECTION_FLAG} -fno-gnu-unique -I${PROJECT_SOURCE_DIR}/include -DES_SERREG_SHARED)

function(es_serreg_negative_test name expected)
  set(_src ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)
  try_compile(_neg_ok
    SOURCES ${_src}
    CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    COMPILE_DEFINITIONS ${ES_SERREG_REFLECTION_FLAG} -fno-gnu-unique -DES_SERREG_SHARED
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
    OUTPUT_VARIABLE _neg_out)
  if(_neg_ok)
    message(FATAL_ERROR "negative test ${name} compiled but must be rejected")
  endif()
  string(FIND "${_neg_out}" "${expected}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "negative test ${name} failed to compile, but without the expected diagnostic \"${expected}\":\n${_neg_out}")
  endif()
  message(STATUS "es::serreg: negative test ${name}: rejected with the expected diagnostic")
  add_test(NAME negative_${name} COMMAND ${CMAKE_CXX_COMPILER} ${_neg_flags} -fsyntax-only ${_src})
  string(REPLACE "(" "\\(" _re "${expected}")
  string(REPLACE ")" "\\)" _re "${_re}")
  string(REPLACE "<" "\\<" _re "${_re}")
  set_tests_properties(negative_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${_re}")
endfunction()

es_serreg_negative_test(neg_unnamed_struct    "unnamed types cannot be serialised")
es_serreg_negative_test(neg_pointer_member    "pointers are not serialisable")
es_serreg_negative_test(neg_nested_as_flat    "as_flat<T> applies to top-level arguments only")
es_serreg_negative_test(neg_enum_u64_overflow "enumerator values must fit int64_t")
es_serreg_negative_test(neg_print_bad_spec    "check_format")   # 1.1 FR-P2: {:.2f} on an int
es_serreg_negative_test(neg_print_arg_count   "check_format")   # 1.1 FR-P2: placeholder/argument count
es_serreg_negative_test(neg_multimap_member   "this standard-library type is not in the serialisable vocabulary")  # S12: std::priority_queue is outside the vocabulary (std::multimap is a member kind)
es_serreg_negative_test(neg_smart_pointer_member "smart pointers (std::unique_ptr, std::shared_ptr, std::weak_ptr) are not serialisable")  # Q4
es_serreg_negative_test(neg_empty_tuple_member "std::tuple<> has no parts to serialise")  # FR-A6
es_serreg_negative_test(neg_string_view_member "std::string_view is not serialisable as a member")  # 2.0
es_serreg_negative_test(neg_router_no_params  "the handler's parameters are the signature")  # 2.1.10
es_serreg_negative_test(neg_print_nested_field "nested replacement fields")  # 2.1.12 review T-05
es_serreg_negative_test(neg_router_generic_lambda "the handler has no single call signature")  # 2.1.12 review T-10
es_serreg_negative_test(neg_zero_size_element "must occupy bytes in the image")  # 2.1.12 review D-05
es_serreg_negative_test(neg_anon_without_name "has no unique canonical name")  # 2.1.14 review C-03/C-05
es_serreg_negative_test(neg_local_without_name "has no unique canonical name")  # 2.1.14 review C-05
es_serreg_negative_test(neg_recursive_type    "recursive types are not supported")  # 2.1.14 review C-07
es_serreg_negative_test(neg_bad_explicit_name "an explicit canonical name must be non-empty")  # 2.1.14
es_serreg_negative_test(neg_two_explicit_names "the canonical name is given twice")  # 2.1.14
es_serreg_negative_test(neg_pointer_argument  "never serialised")  # 2.1.16 B10 (2.2.0: a pointer; containers are arguments now)

# control: the harness must accept a valid file (compiles at configure time and as a ctest)
try_compile(_neg_control_ok
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/neg_control_compiles.cpp
  CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  COMPILE_DEFINITIONS ${ES_SERREG_REFLECTION_FLAG} -fno-gnu-unique -DES_SERREG_SHARED
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/include"
  OUTPUT_VARIABLE _neg_control_out)
if(NOT _neg_control_ok)
  message(FATAL_ERROR "negative-test control file does not compile:\n${_neg_control_out}")
endif()
add_test(NAME negative_control_compiles COMMAND ${CMAKE_CXX_COMPILER} ${_neg_flags} -fsyntax-only ${CMAKE_CURRENT_SOURCE_DIR}/neg_control_compiles.cpp)
es_serreg_negative_test(neg_const_member     "const and volatile members are not serialisable")  # 2.2.6 review B4
es_serreg_negative_test(neg_volatile_member  "const and volatile members are not serialisable")  # 2.2.6 review B4
