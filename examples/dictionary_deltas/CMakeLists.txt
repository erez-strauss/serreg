# examples/dictionary_deltas — a capture whose dictionary grows while it is written (README.md): the host writes
# its own frames, then dlopen()s two plugins whose types enter the registry after the opening dictionary, so the
# file carries dictionary delta frames linked from the opening block's CHAIN, and checkpoints with full copies.
# Needs the shared libserreg (one registry per process is what makes a plugin's entries visible to the host).
if(NOT ES_SERREG_BUILD_SHARED)
  message(STATUS "es::serreg: examples/dictionary_deltas needs ES_SERREG_BUILD_SHARED=ON; skipped")
  return()
endif()

foreach(plugin tick quote)
  add_library(dictionary_deltas_plugin_${plugin} MODULE plugin_${plugin}.cpp)
  target_link_libraries(dictionary_deltas_plugin_${plugin} PRIVATE es::serreg)
  set_target_properties(dictionary_deltas_plugin_${plugin} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  es_serreg_apply_warnings(dictionary_deltas_plugin_${plugin})
endforeach()

add_executable(dictionary_deltas_host host.cpp)
target_link_libraries(dictionary_deltas_host PRIVATE es::serreg ${CMAKE_DL_LIBS})
es_serreg_apply_warnings(dictionary_deltas_host)
add_dependencies(dictionary_deltas_host dictionary_deltas_plugin_tick dictionary_deltas_plugin_quote)

if(ES_SERREG_BUILD_TESTS AND TARGET srdump)
  # Runs the host, then srdump --info (the dictionary chain must have three blocks: the opening one and one delta
  # per plugin) and srdump --verify on the file it wrote (run_dictionary_deltas.cmake).
  add_test(NAME example_dictionary_deltas
    COMMAND ${CMAKE_COMMAND} -DHOST=$<TARGET_FILE:dictionary_deltas_host> -DSRDUMP=$<TARGET_FILE:srdump>
            -DPLUGIN_TICK=$<TARGET_FILE:dictionary_deltas_plugin_tick>
            -DPLUGIN_QUOTE=$<TARGET_FILE:dictionary_deltas_plugin_quote>
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/dictionary_deltas.ser
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_dictionary_deltas.cmake)
  set_tests_properties(example_dictionary_deltas PROPERTIES
    PASS_REGULAR_EXPRESSION "dictionary_deltas: ok \\(3 dictionary blocks, verified\\)" TIMEOUT 120)
  # The checked-in copy of that file (tests/fixtures/corpus/capture/dictionary_deltas-1.7.ser; also a fuzz seed
  # and a python_reader_parity input): the shipped tool verifies it, and its merged dictionary names both
  # plugins' types.
  set(_dd_fixture ${PROJECT_SOURCE_DIR}/tests/fixtures/corpus/capture/dictionary_deltas-1.7.ser)
  add_test(NAME example_dictionary_deltas_fixture_verify COMMAND srdump ${_dd_fixture} --verify)
  set_tests_properties(example_dictionary_deltas_fixture_verify PROPERTIES
    PASS_REGULAR_EXPRESSION "^ok: 9 frames, 18 records")
  add_test(NAME example_dictionary_deltas_fixture_json COMMAND srdump ${_dd_fixture} --json --dict-only)
  set_tests_properties(example_dictionary_deltas_fixture_json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"name\": \"plugin_quote::Quote\".*\"name\": \"plugin_tick::Tick\"")
  add_test(NAME example_dictionary_deltas_fixture_info COMMAND srdump ${_dd_fixture} --info)
  set_tests_properties(example_dictionary_deltas_fixture_info PROPERTIES
    PASS_REGULAR_EXPRESSION "^format 1\\.7 .*flags checkpoints info\ndictionary .*; 3 block\\(s\\) at [0-9]+ [0-9]+ [0-9]+\ncheckpoints [1-9]")
endif()
