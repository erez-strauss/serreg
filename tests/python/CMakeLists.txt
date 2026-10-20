# The stand-alone pure-Python reader (python/serreg, 2.2.1) against the reference tool.
# python_reader_parity: tests/python/parity.py runs `srdump --json` and `python3 -m serreg --json` on the golden and
# corpus captures (FILE 1.6 and 1.7, bare frame streams with an external RRI, a log capture written by
# python_log_capture) and compares them frame by frame and record by record; `--verify` counts and, for 1.7 files,
# `--info`'s dictionary blocks and checkpoints are compared too. Skipped (return code 77) where Python 3 is missing.
if(NOT TARGET srdump)
  return()
endif()

# a 1.7 capture of log frames (both envelope policies), a println frame and a record frame, under CRC trailers
add_executable(python_log_capture log_capture.cpp)
target_link_libraries(python_log_capture PRIVATE es::serreg)
es_serreg_apply_warnings(python_log_capture)

set(es_serreg_py_corpus ${PROJECT_SOURCE_DIR}/tests/fixtures/corpus)
set(es_serreg_py_captures
    ${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
    ${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser
    ${ES_SERREG_GOLDEN_FIXTURES}/features.ser
    ${es_serreg_py_corpus}/capture/orders-1.6.ser
    ${es_serreg_py_corpus}/capture/dynamic-1.6.ser
    ${es_serreg_py_corpus}/capture/selftest.ser
    # a dictionary chain of three blocks (examples/dictionary_deltas): the opening one and two delta frames
    ${es_serreg_py_corpus}/capture/dictionary_deltas-1.7.ser
    # bare frame streams: the dictionary comes from an RRI, for srdump and for the reader alike
    ${es_serreg_py_corpus}/capture/dict_frame_stream.bin:${es_serreg_py_corpus}/rri/selftest.rri
    ${es_serreg_py_corpus}/capture/bare_frames.bin:${es_serreg_py_corpus}/rri/selftest.rri)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME python_reader_parity
           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/parity.py --srdump $<TARGET_FILE:srdump>
                   --package ${PROJECT_SOURCE_DIR}/python --log-writer $<TARGET_FILE:python_log_capture>
                   ${es_serreg_py_captures})
else() # no interpreter: the test reports SKIP (77), as parity.py itself does when it cannot run
  add_test(NAME python_reader_parity COMMAND sh -c "echo 'SKIP: Python 3 not found'; exit 77")
endif()
set_tests_properties(python_reader_parity PROPERTIES LABELS python SKIP_RETURN_CODE 77 TIMEOUT 300)
