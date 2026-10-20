# 2.1.16 (plan A5): `srdump --cxx` regenerates a dictionary's types as C++, and the output proves itself when it is
# compiled: static_asserts for every size, offset, canonical name, schema hash and layout hash.
#   cxxgen_golden_compiles          orders.rri → orders.hpp (+ the --cxx-router skeleton); check_orders.cpp compiles
#   cxxgen_golden_dynamic_compiles  dynamic.ser's embedded dictionary → dynamic.hpp; check_dynamic.cpp compiles
#   cxxgen_golden_namespace_compiles orders.rri with --namespace corpus=regen → orders_ns.hpp, compiled beside
#                                   corpus.hpp: the regen:: types have the corpus:: hashes (check_namespace.cpp)
#   cxxgen_golden_roundtrip         orders.hpp compiled and linked into roundtrip_orders, which routes the golden
#                                   orders.ser through the regenerated types and compares the values
#   cxxgen_golden_dynamic_roundtrip the same for dynamic.hpp and dynamic.ser (roundtrip_dynamic)
# Generation happens at test time (a build does not run binaries); compile_generated.cmake runs srdump, then the
# project compiler with the project's flags and warnings (-Werror: the generated header is warning-clean too).
if(NOT TARGET srdump)
  return()
endif()

set(_gen ${CMAKE_CURRENT_BINARY_DIR})
set(_flags "${CMAKE_CXX_FLAGS} -std=c++26 ${ES_SERREG_REFLECTION_FLAG} -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -I${PROJECT_SOURCE_DIR}/include -I${ES_SERREG_CORPUS_DIR}")
set(_defs "$<JOIN:$<TARGET_PROPERTY:serreg,INTERFACE_COMPILE_DEFINITIONS>, >")
set(_link "${CMAKE_EXE_LINKER_FLAGS} -pthread -Wl,-rpath,$<TARGET_FILE_DIR:serreg> -Wl,-rpath,$<TARGET_FILE_DIR:serreg_tools>")

# mode: syntax (compile with -fsyntax-only) | run (compile, link, run on the golden capture); ARGN: srdump's sources
function(es_serreg_cxxgen_test name src header mode capture)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -DSRDUMP=$<TARGET_FILE:srdump> "-DSOURCE_ARGS=${ARGN}" -DOUT=${_gen}/${header}
                   -DCXX=${CMAKE_CXX_COMPILER} "-DFLAGS=${_flags}" "-DDEFS=${_defs}" -DMODE=${mode}
                   "-DLIBS=$<TARGET_FILE:serreg_tools>;$<TARGET_FILE:serreg>" "-DLINK_FLAGS=${_link}"
                   -DEXE=${_gen}/${name} -DCAPTURE=${ES_SERREG_GOLDEN_FIXTURES}/${capture}
                   -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/${src} -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_generated.cmake)
  set_tests_properties(${name} PROPERTIES LABELS cxxgen TIMEOUT 600)
endfunction()

es_serreg_cxxgen_test(cxxgen_golden_compiles check_orders.cpp orders.hpp syntax orders.ser
                      --rri ${ES_SERREG_GOLDEN_FIXTURES}/orders.rri)
es_serreg_cxxgen_test(cxxgen_golden_dynamic_compiles check_dynamic.cpp dynamic.hpp syntax dynamic.ser
                      ${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser)
es_serreg_cxxgen_test(cxxgen_golden_namespace_compiles check_namespace.cpp orders_ns.hpp syntax orders.ser
                      --rri ${ES_SERREG_GOLDEN_FIXTURES}/orders.rri --namespace corpus=regen)
es_serreg_cxxgen_test(cxxgen_golden_roundtrip roundtrip_orders.cpp orders_rt.hpp run orders.ser
                      --rri ${ES_SERREG_GOLDEN_FIXTURES}/orders.rri)
set_tests_properties(cxxgen_golden_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION "cxxgen roundtrip: 11 frames handled, ok")
es_serreg_cxxgen_test(cxxgen_golden_dynamic_roundtrip roundtrip_dynamic.cpp dynamic_rt.hpp run dynamic.ser
                      ${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser)
set_tests_properties(cxxgen_golden_dynamic_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION "cxxgen roundtrip: 9 frames handled, ok")
