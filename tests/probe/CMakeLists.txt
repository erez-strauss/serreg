# Compile-time probe (cc F9, plan §5 "F9"): compiling depth4.cpp (depth-4 nesting, 4 members per level) must finish
# within 60 s of wall clock. Invokes the compiler directly (not a build target) so every ctest run recompiles.
add_test(NAME compile_probe_depth4
         COMMAND ${CMAKE_CXX_COMPILER} -std=c++26 ${ES_SERREG_REFLECTION_FLAG} -fno-gnu-unique -DES_SERREG_SHARED
                 -I${PROJECT_SOURCE_DIR}/include -c ${CMAKE_CURRENT_SOURCE_DIR}/depth4.cpp -o ${CMAKE_CURRENT_BINARY_DIR}/depth4.o)
set_tests_properties(compile_probe_depth4 PROPERTIES TIMEOUT 300) # 2.1.12 (review B-13): 14 s / 885 MB alone; slow under -j on small runners
# ... and the probe also links and runs (the descriptors it produces are usable)
add_executable(probe_depth4 depth4.cpp)
target_link_libraries(probe_depth4 PRIVATE es::serreg)
es_serreg_apply_warnings(probe_depth4)
add_test(NAME compile_probe_depth4_runs COMMAND probe_depth4)
