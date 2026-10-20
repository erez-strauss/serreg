# Unit tests (spec §10.2 #1, #3, #4, #5, #6): one Catch2 executable over all test TUs (multi-TU linking is itself
# part of what is under test), a framework-free pre-main registry check, and a separate FR-R4 conflict test.
set(ES_SERREG_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../golden)

function(es_serreg_catch2_exe name)
  add_executable(${name} ${ARGN})
  # es::serreg_tools: serreg_tests covers the CLI, the Wireshark generator and the diff too (2.1.13: own library)
  target_link_libraries(${name} PRIVATE es::serreg es::serreg_tools Catch2::Catch2WithMain)
  target_include_directories(${name} PRIVATE ${ES_SERREG_CORPUS_DIR} ${ES_SERREG_TEST_SUPPORT_DIR})
  target_compile_definitions(${name} PRIVATE ES_SERREG_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}"
                                             ES_SERREG_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  es_serreg_apply_warnings(${name})
  # 2.1.12 (review B-08): list the test cases when ctest runs, not as a post-build step (a build must not run
  # test binaries, and a sanitizer build that cannot run them here still builds)
  catch_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

es_serreg_catch2_exe(serreg_tests
  test_roundtrip.cpp
  test_readers.cpp
  test_registry.cpp
  test_capture.cpp
  test_anon_tu.cpp
  test_empty_member.cpp
  test_sinks.cpp        # v1.0.2: gk 1.3/1.7/1.8/1.9, cc F6, FR-W7 tracker
  test_tools.cpp        # v1.0.2: gk 1.4/1.11/1.12, cc F16/F19/F24
  test_skip.cpp         # v1.0.2: cc F4 skip semantics
  test_print.cpp        # v1.1.0: scalar/string arguments, FR-L6, print/println, render; 1.3-only refusal
  test_dynamic.cpp      # v2.0.0: dynamic types (canonical images): round trips, bounds, tools
  test_hash.cpp         # v2.1.0: FR-H5 XXH3-64 known answers, consteval = run time
  test_register_namespace.cpp  # 2.2.5: A-22 ES_SERREG_REGISTER_NAMESPACE — the walk, selectors, filter, registration
  test_limits.cpp       # 2.1.16: decode_limits (FR-D5) and the windowed capture readers (A6)
  test_containers.cpp   # 2.2.0: maps, sets, containers as arguments (format 1.6)
  test_format17.cpp     # 2.2.1: dictionary chain, CRC32C, checkpoints, writer info, --resync (format 1.7)
  test_log.cpp          # 2.2.1: logger, level gate, envelope policies, scoped dictionaries, --level/--since/--until, on_log
  test_to_string.cpp    # 2.2.1: to_string / to_json / render for any value, formattable<T>
  test_evolution.cpp    # R-9: schema evolution level 1 for flat types (projection by member name)
  ${ES_SERREG_CORPUS_DIR}/hashes.cpp    # golden STATIC_REQUIREs (spec §10.2 #1)
  ${ES_SERREG_CORPUS_DIR}/blobs.cpp)    # ENTRY blob hash/size lock (cc F1)

# FR-D5: the read-side enum range check is a compile-time policy, off by default. serreg_tests is compiled with it
# on (test_limits.cpp exercises the "report" behaviour); serreg_validate_off_tests keeps the default and asserts
# that the same bytes pass through untouched.
target_compile_definitions(serreg_tests PRIVATE ES_SERREG_VALIDATE_READ=1)
es_serreg_catch2_exe(serreg_validate_off_tests test_validate_off.cpp)

# 2.2.6 (review B1): serialize() of a type that may sort an unordered container is not noexcept; a failing
# allocation of the sort's order table (above 64 entries) propagates as std::bad_alloc. Separate executable: the
# TU replaces the global operator new with one that fails on demand.
es_serreg_catch2_exe(serreg_alloc_tests test_alloc.cpp)

# FR-R4: a hand-built node with the same schema hash but another layout hash must be recorded as a conflict.
# Separate executable: the injected conflict stays in the process registry for its lifetime.
es_serreg_catch2_exe(serreg_conflict test_conflict.cpp)

# es::ipc — regions and the variable-size SPSC queue / SPMC broadcast (2.1.13a, docs/ipc/; threads and processes). Separate
# executable: the types it serializes must not enter serreg_tests, whose registry contents the golden tests lock.
es_serreg_catch2_exe(serreg_ipc_tests test_ipc.cpp)
target_link_libraries(serreg_ipc_tests PRIVATE es::ipc) # 2.1.13: es::ipc is its own library

# ---- pre-main registry count + capture producer (FR-R1/R2/R3) ---------------------------------------------
# main()'s first statement reads registry::size(): every entry used by this executable must already be there.
add_executable(premain_check premain_check.cpp)
target_link_libraries(premain_check PRIVATE es::serreg)
target_include_directories(premain_check PRIVATE ${ES_SERREG_CORPUS_DIR})
es_serreg_apply_warnings(premain_check)

set(_cap ${CMAKE_CURRENT_BINARY_DIR}/premain.ser)
set(_rri ${CMAKE_CURRENT_BINARY_DIR}/premain_dump.rri)
add_test(NAME premain_registry_count COMMAND premain_check ${_cap})
set_tests_properties(premain_registry_count PROPERTIES FIXTURES_SETUP premain_capture)

# FR-R9 / §6.11: ES_SERREG_DUMP_REGISTRY writes the registry at exit; the file must parse as an RRI.
add_test(NAME premain_env_dump COMMAND premain_check ${CMAKE_CURRENT_BINARY_DIR}/premain_env.ser)
set_tests_properties(premain_env_dump PROPERTIES
  ENVIRONMENT "ES_SERREG_DUMP_REGISTRY=${_rri};ES_SERREG_DUMP_FORMAT=rri"
  FIXTURES_SETUP premain_dump)

if(TARGET srdump)
  add_test(NAME srdump_verify_capture COMMAND srdump ${_cap} --verify)
  set_tests_properties(srdump_verify_capture PROPERTIES FIXTURES_REQUIRED premain_capture
    PASS_REGULAR_EXPRESSION "^ok: 4 frames, 8 records")
  add_test(NAME srdump_json_capture COMMAND srdump ${_cap} --json)
  set_tests_properties(srdump_json_capture PROPERTIES FIXTURES_REQUIRED premain_capture
    PASS_REGULAR_EXPRESSION "corpus::Order")
  add_test(NAME srdump_env_dump_parses COMMAND srdump --rri ${_rri})
  set_tests_properties(srdump_env_dump_parses PROPERTIES FIXTURES_REQUIRED premain_dump
    PASS_REGULAR_EXPRESSION "corpus::Order")
  if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
    # FR-D6: --elf on the test binaries lists their types
    add_test(NAME srdump_elf_premain COMMAND srdump --elf $<TARGET_FILE:premain_check> --json)
    set_tests_properties(srdump_elf_premain PROPERTIES PASS_REGULAR_EXPRESSION "corpus::Order.*corpus::Side|corpus::Side.*corpus::Order")
    add_test(NAME srdump_elf_serreg_tests COMMAND srdump --elf $<TARGET_FILE:serreg_tests> --json)
    set_tests_properties(srdump_elf_serreg_tests PROPERTIES PASS_REGULAR_EXPRESSION "corpus::Local")
    # cc F4: a [[=skip]]ped nested type must not leak into the ELF dictionary section (test_skip.cpp defines it)
    add_test(NAME srdump_elf_no_skipped_type COMMAND srdump --elf $<TARGET_FILE:serreg_tests> --json)
    set_tests_properties(srdump_elf_no_skipped_type PROPERTIES PASS_REGULAR_EXPRESSION "skipns::SkipsNested"
                                                              FAIL_REGULAR_EXPRESSION "skipns::Hidden")
  else() # 2.1.11, gcc16-static-none: no es_serreg_dict section, so --elf finds nothing (and says so)
    add_test(NAME srdump_elf_none_has_no_section COMMAND srdump --elf $<TARGET_FILE:premain_check>)
    set_tests_properties(srdump_elf_none_has_no_section PROPERTIES PASS_REGULAR_EXPRESSION "not_found")
  endif()
  # cc F24: --version / --program-hash
  add_test(NAME srdump_version COMMAND srdump --version)
  set_tests_properties(srdump_version PROPERTIES PASS_REGULAR_EXPRESSION "^srdump ${PROJECT_VERSION_MAJOR}\\.${PROJECT_VERSION_MINOR}\\.${PROJECT_VERSION_PATCH} \\(format 1\\.7, hash_algo 2\\)")
  add_test(NAME srdump_program_hash_capture COMMAND srdump ${_cap} --program-hash)
  set_tests_properties(srdump_program_hash_capture PROPERTIES FIXTURES_REQUIRED premain_capture PASS_REGULAR_EXPRESSION "^0x[0-9a-f]+\n")
endif()

# gk 1.13 / §6.11: ES_SERREG_DUMP_SIGNAL — sigwait helper thread, kill() from the parent, clean shutdown.
# 2.1.11: many registrations before main() publish one index (O(n) startup, not O(n²))
add_executable(registry_scale registry_scale.cpp)
target_link_libraries(registry_scale PRIVATE es::serreg)
es_serreg_apply_warnings(registry_scale)
add_test(NAME registry_scale_one_index_for_startup COMMAND registry_scale)
set_tests_properties(registry_scale_one_index_for_startup PROPERTIES
  PASS_REGULAR_EXPRESSION "registry_scale: [0-9]+ entries, 0 indexes published before main's first read, 1 after, 1 later")

# 2.1.12 (review C-05): a class local to main() compiles; since 2.1.14 it is named explicitly (local_in_main::Local)
add_executable(local_in_main local_in_main.cpp)
target_link_libraries(local_in_main PRIVATE es::serreg)
es_serreg_apply_warnings(local_in_main)
add_test(NAME local_class_in_main_serialises COMMAND local_in_main)

add_executable(signal_dump_test signal_dump_test.cpp)
target_link_libraries(signal_dump_test PRIVATE es::serreg)
es_serreg_apply_warnings(signal_dump_test)
add_test(NAME signal_dump_usr2 COMMAND signal_dump_test ${CMAKE_CURRENT_BINARY_DIR}/signal_dump.rri)
set_tests_properties(signal_dump_usr2 PROPERTIES TIMEOUT 60)
