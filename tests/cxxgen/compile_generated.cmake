# tests/cxxgen/compile_generated.cmake — 2.1.16 (plan A5): runs `srdump --cxx --cxx-router` on a dictionary source
# and compiles a translation unit that includes the output. The generated header carries its own static_asserts
# (sizes, offsets, canonical names, schema and layout hashes), so MODE=syntax (-fsyntax-only) is already the whole
# regeneration check; MODE=run links the TU against the built libraries and runs it on CAPTURE, so the regenerated
# types also decode the golden frames. Run as:
#   cmake -DSRDUMP=… -DSOURCE_ARGS="--rri;x.rri" -DOUT=dir/x.hpp -DCXX=… -DFLAGS="…" -DDEFS="…" -DSRC=check.cpp
#         -DMODE=syntax|run [-DLIBS="a.so;b.so" -DLINK_FLAGS="…" -DEXE=dir/prog -DCAPTURE=x.ser] -P compile_generated.cmake
execute_process(COMMAND ${SRDUMP} ${SOURCE_ARGS} --cxx ${OUT} --cxx-router
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "srdump --cxx failed (${rc}):\n${out}${err}")
endif()
message(STATUS "${err}")
get_filename_component(gen_dir ${OUT} DIRECTORY)
separate_arguments(flag_list UNIX_COMMAND "${FLAGS}")
separate_arguments(def_list UNIX_COMMAND "${DEFS}")
list(TRANSFORM def_list PREPEND "-D")
if(MODE STREQUAL "syntax")
  execute_process(COMMAND ${CXX} ${flag_list} ${def_list} -fsyntax-only -I${gen_dir} ${SRC}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "compiling ${SRC} with ${OUT} failed (${rc}):\n${out}${err}")
  endif()
  message(STATUS "${SRC}: the generated ${OUT} compiles; every static_assert holds")
  return()
endif()
# Compile and link in two steps, as a build does: the sanitizer presets carry -fsanitize in both CMAKE_CXX_FLAGS
# and the linker flags, and GCC 16.2 given the option twice on one command line miscompiles the constant
# evaluation of the generated static_asserts ("'&"std::monostate" == 0' is not a constant expression", 2.1.16
# full run under gcc16-asan-ubsan).
separate_arguments(link_list UNIX_COMMAND "${LINK_FLAGS}")
execute_process(COMMAND ${CXX} ${flag_list} ${def_list} -I${gen_dir} -c ${SRC} -o ${EXE}.o
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "compiling ${SRC} with ${OUT} failed (${rc}):\n${out}${err}")
endif()
execute_process(COMMAND ${CXX} ${EXE}.o -o ${EXE} ${LIBS} ${link_list}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "linking ${EXE} failed (${rc}):\n${out}${err}")
endif()
execute_process(COMMAND ${EXE} ${CAPTURE} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${EXE} ${CAPTURE} failed (${rc})")
endif()
