# Runs `srdump <capture> <flag>` and compares stdout byte for byte with the checked-in rendering.
execute_process(COMMAND ${SRDUMP} ${CAPTURE} ${FLAG} OUTPUT_FILE ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "srdump ${FLAG} exited ${rc}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT} ${EXPECTED} RESULT_VARIABLE cmp)
if(NOT cmp EQUAL 0)
  message(FATAL_ERROR "srdump ${FLAG} output differs from ${EXPECTED}")
endif()
