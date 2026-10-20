add_executable(file_store file_store.cpp)
target_link_libraries(file_store PRIVATE es::serreg)
es_serreg_apply_warnings(file_store)
add_executable(ipc_ring ipc_ring.cpp)
target_link_libraries(ipc_ring PRIVATE es::serreg es::ipc)   # 2.1.13: es::ipc is linked on its own
es_serreg_apply_warnings(ipc_ring)

# The examples double as smoke tests when the test suite is enabled (they are what the README shows).
if(ES_SERREG_BUILD_TESTS)
  add_test(NAME example_file_store COMMAND file_store ${CMAKE_CURRENT_BINARY_DIR}/file_store.ser)
  set_tests_properties(example_file_store PROPERTIES PASS_REGULAR_EXPRESSION "5 frames, 10 records, [0-9]+ bytes, ok")
  add_test(NAME example_ipc_ring COMMAND ipc_ring)
  set_tests_properties(example_ipc_ring PROPERTIES PASS_REGULAR_EXPRESSION "orders=75000 cancels=25000 literals=100000" TIMEOUT 120)
endif()
# print / println into two sinks (the whole-registry dictionary vs a scoped one) and the ADL spellings
add_executable(print_sinks print_sinks.cpp)
target_link_libraries(print_sinks PRIVATE es::serreg es::serreg_tools)   # tools::info reads the dictionaries back
es_serreg_apply_warnings(print_sinks)
if(ES_SERREG_BUILD_TESTS)
  add_test(NAME example_print_sinks COMMAND print_sinks ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(example_print_sinks PROPERTIES
    PASS_REGULAR_EXPRESSION "print_sinks.log.ser.*dictionary: 0 types, 4 literals, 4 calls")
endif()

add_executable(register_namespace register_namespace.cpp)   # 2.2.5: ES_SERREG_REGISTER_NAMESPACE (A-22)
target_link_libraries(register_namespace PRIVATE es::serreg es::serreg_tools)
es_serreg_apply_warnings(register_namespace)
if(ES_SERREG_BUILD_TESTS)
  add_test(NAME example_register_namespace COMMAND register_namespace ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(example_register_namespace PROPERTIES
    PASS_REGULAR_EXPRESSION "registered: Order true Side true v2::MsgOrder true \\| 5 of 6 selected.*register_namespace.ser: dictionary 5 types, 0 calls, 0 frames")
endif()

add_executable(embedded_cli embedded_cli.cpp)   # v1.0.3: the srdump command line embedded in an application
target_link_libraries(embedded_cli PRIVATE es::serreg_tools)   # 2.1.13: the CLI lives in es::serreg_tools
es_serreg_apply_warnings(embedded_cli)
if(ES_SERREG_BUILD_TESTS)
  add_test(NAME example_embedded_cli_version COMMAND embedded_cli --version)
  set_tests_properties(example_embedded_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^embedded_cli 0\\.1 \\(format 1\\.7, hash_algo 2\\)")
  add_test(NAME example_embedded_cli_selftest COMMAND embedded_cli --trace --selftest ${CMAKE_CURRENT_BINARY_DIR}/embedded.ser --verify ${CMAKE_CURRENT_BINARY_DIR}/embedded.ser)
  set_tests_properties(example_embedded_cli_selftest PROPERTIES PASS_REGULAR_EXPRESSION "ok: 3 frames, 6 records")
  add_test(NAME example_embedded_cli_default COMMAND embedded_cli)
  set_tests_properties(example_embedded_cli_default PROPERTIES PASS_REGULAR_EXPRESSION "app::Tick")
  # 2.1.10: --wireshark with no other source describes the application's own registry
  add_test(NAME example_embedded_cli_wireshark COMMAND embedded_cli --wireshark -)
  set_tests_properties(example_embedded_cli_wireshark PROPERTIES
    PASS_REGULAR_EXPRESSION "the registry of embedded_cli.*ProtoField[.][a-z0-9]+[(]\"serreg[.]app_Tick[.]")
endif()

# 2.1.13a: es::ipc between two programs, the consumer rendering from the producer's in-band dictionary
add_subdirectory(ipc_log)

# v2.0.6: a user-style example in its own directory (also buildable standalone with find_package)
add_subdirectory(user_example1)

# a capture whose dictionary grows while it is written: dlopen()ed plugins, delta frames, the CHAIN, checkpoints
add_subdirectory(dictionary_deltas)
