# Warning set from spec §3 / AGENTS.md §4, applied to every target we build. 2.2.1 (maintainer): -Wshadow and the
# GCC/Clang diagnostics that catch real defects without false positives in this code base; every preset has
# ES_SERREG_WERROR on, so the whole tree, the tests and the examples compile clean under all of them.
function(es_serreg_apply_warnings tgt)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
                         -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wformat=2
                         -Wimplicit-fallthrough -Wmisleading-indentation)
  # not -Wnull-dereference: at -O2 it fires on std::expected::error() after a has_value() check (a false positive
  # GCC documents), so it would make release builds depend on the optimiser's view
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${tgt} PRIVATE -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
  endif()
  if(ES_SERREG_WERROR)
    target_compile_options(${tgt} PRIVATE -Werror)
  endif()
endfunction()
