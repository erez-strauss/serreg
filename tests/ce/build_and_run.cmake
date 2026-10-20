# tests/ce/build_and_run.cmake — 2.2.1: compiles one ce-examples/ce-exampleN/main.cpp exactly as the maintainer does
# on a plain command line (-O1 -W -Wall -Wextra -Wshadow -Werror, -I. so the example's own header symlinks are what
# is compiled) and runs it. Run as: cmake -DCXX=… -DFLAGS=… -DDIR=… -DOUT=… -DEXPECT=<regex> -P build_and_run.cmake
separate_arguments(flag_list UNIX_COMMAND "${FLAGS}")
execute_process(COMMAND ${CXX} ${flag_list} -O1 -W -Wall -Wextra -Wshadow -Werror -I. main.cpp -o ${OUT} -lpthread
                WORKING_DIRECTORY ${DIR} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "compiling ${DIR}/main.cpp failed:\n${out}${err}")
endif()
# 2.2.5: run in the build directory, not the source tree — ce-example6 writes (and removes) orders.ser in its cwd,
# and no *.ser may appear outside tests/fixtures (make-release.sh refuses the tree).
get_filename_component(out_dir ${OUT} DIRECTORY)
execute_process(COMMAND ${OUT} WORKING_DIRECTORY ${out_dir} RESULT_VARIABLE rc OUTPUT_VARIABLE out
                ERROR_VARIABLE err TIMEOUT 120)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${OUT} exited with ${rc}:\n${out}${err}")
endif()
if(NOT "${out}${err}" MATCHES "${EXPECT}")
  message(FATAL_ERROR "${OUT}: expected output matching \"${EXPECT}\":\n${out}${err}")
endif()
message(STATUS "${DIR}: ok")
