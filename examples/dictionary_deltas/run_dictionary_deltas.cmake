# examples/dictionary_deltas/run_dictionary_deltas.cmake — the ctest example_dictionary_deltas: runs the host with
# the two plugins, then checks the file with srdump: --info must show three dictionary blocks (the opening one
# and one delta frame per plugin) and at least one checkpoint, and --verify must accept every frame.
#   cmake -DHOST=... -DSRDUMP=... -DPLUGIN_TICK=... -DPLUGIN_QUOTE=... -DOUT=... -P run_dictionary_deltas.cmake
foreach(v HOST SRDUMP PLUGIN_TICK PLUGIN_QUOTE OUT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "run_dictionary_deltas.cmake: -D${v}=... is required")
  endif()
endforeach()
file(REMOVE "${OUT}")
execute_process(COMMAND "${HOST}" "${OUT}" "${PLUGIN_TICK}" "${PLUGIN_QUOTE}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dictionary_deltas: host exit ${rc}")
endif()
execute_process(COMMAND "${SRDUMP}" "${OUT}" --info
                RESULT_VARIABLE rc OUTPUT_VARIABLE info ERROR_VARIABLE err TIMEOUT 60)
message("srdump --info:\n${info}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dictionary_deltas: srdump --info exit ${rc}")
endif()
if(NOT info MATCHES "\ndictionary [^\n]*; 3 block\\(s\\) at [0-9]+ [0-9]+ [0-9]+\ncheckpoints [1-9]")
  message(FATAL_ERROR "dictionary_deltas: expected 3 dictionary blocks and a checkpoint chain in srdump --info")
endif()
execute_process(COMMAND "${SRDUMP}" "${OUT}" --verify
                RESULT_VARIABLE rc OUTPUT_VARIABLE verify ERROR_VARIABLE err TIMEOUT 60)
message("srdump --verify: ${verify}${err}")
if(NOT rc EQUAL 0 OR NOT verify MATCHES "^ok: ")
  message(FATAL_ERROR "dictionary_deltas: srdump --verify exit ${rc}")
endif()
message("dictionary_deltas: ok (3 dictionary blocks, verified)")
