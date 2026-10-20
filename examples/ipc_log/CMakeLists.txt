# examples/ipc_log — two programs over a named es::ipc SPSC queue (README.md): the producer logs with
# println and sends its dictionary in band; the consumer knows one type (messages.hpp) and renders the
# rest from that dictionary.
add_executable(ipc_log_producer producer.cpp)
target_link_libraries(ipc_log_producer PRIVATE es::serreg es::ipc)
es_serreg_apply_warnings(ipc_log_producer)
add_executable(ipc_log_consumer consumer.cpp)
target_link_libraries(ipc_log_consumer PRIVATE es::serreg es::ipc)
es_serreg_apply_warnings(ipc_log_consumer)

if(ES_SERREG_BUILD_TESTS)
  # Both programs at once (run_ipc_log.cmake); the consumer's text must equal the producer's reference text.
  add_test(NAME example_ipc_log
    COMMAND ${CMAKE_COMMAND} -DPRODUCER=$<TARGET_FILE:ipc_log_producer> -DCONSUMER=$<TARGET_FILE:ipc_log_consumer>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ipc_log.cmake)
  set_tests_properties(example_ipc_log PROPERTIES PASS_REGULAR_EXPRESSION "ipc_log: identical" TIMEOUT 120)
endif()
