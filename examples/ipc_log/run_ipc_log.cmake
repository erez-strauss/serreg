# examples/ipc_log/run_ipc_log.cmake — the ctest example_ipc_log: starts the consumer and the producer at the same
# time (execute_process runs its COMMANDs concurrently), under a region name unique to this run, and compares the
# consumer's text with the reference text the producer wrote, byte for byte.
#   cmake -DPRODUCER=... -DCONSUMER=... -DWORK_DIR=... -P run_ipc_log.cmake
foreach(v PRODUCER CONSUMER WORK_DIR)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "run_ipc_log.cmake: -D${v}=... is required")
  endif()
endforeach()
string(RANDOM LENGTH 10 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789" suffix)
set(name "serreg_ipc_log_${suffix}")
set(expected "${WORK_DIR}/ipc_log.expected.txt")
set(actual "${WORK_DIR}/ipc_log.actual.txt")
file(REMOVE "${expected}" "${actual}")
# The consumer is listed first: it waits for the region the producer creates. stdout of the producer (empty) is
# piped into the consumer's stdin, which it does not read; both write their status lines to stderr.
execute_process(
  COMMAND "${CONSUMER}" --name "${name}" --out "${actual}" --timeout-ms 60000
  COMMAND "${PRODUCER}" --name "${name}" --expect "${expected}" --linger-ms 60000
  RESULTS_VARIABLE codes
  ERROR_VARIABLE err
  TIMEOUT 110)
message("${err}")
list(GET codes 0 consumer_rc)
list(GET codes 1 producer_rc)
if(NOT consumer_rc EQUAL 0 OR NOT producer_rc EQUAL 0)
  message(FATAL_ERROR "ipc_log: consumer exit ${consumer_rc}, producer exit ${producer_rc}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${expected}" "${actual}" RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "ipc_log: ${actual} differs from ${expected}")
endif()
file(SIZE "${actual}" bytes)
file(STRINGS "${actual}" lines)
list(LENGTH lines n)
message("ipc_log: identical (${n} lines, ${bytes} bytes)")
