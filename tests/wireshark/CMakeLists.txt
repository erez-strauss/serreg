# Wireshark dissector generation (2.1.9, docs/wireshark.md).
# wireshark_generate: `srdump --wireshark -` writes a dissector with the expected filter fields (no tshark needed).
# wireshark_dissector_parity: tshark decodes captures with generated dissectors and must show exactly the values
# srdump --json shows (tests/wireshark/parity.py); skipped (return code 77) where tshark or Python 3 is missing.
# wireshark_dissector_lua52 / _lua54 (2.1.11): the same checks on FILE captures with the dissector running under a
# plain Lua 5.2 / 5.4 interpreter through wslua_stub.lua (Wireshark ≤ 4.4 embeds 5.2, 4.6 and later 5.4).
if(NOT TARGET srdump)
  return()
endif()

# 2.1.10: a capture of print frames covering std::format's replacement fields (rendered-message parity)
add_executable(wireshark_print_capture print_capture.cpp)
target_link_libraries(wireshark_print_capture PRIVATE es::serreg_tools)
es_serreg_apply_warnings(wireshark_print_capture)

add_test(NAME wireshark_generate
         COMMAND srdump --wireshark - ${ES_SERREG_GOLDEN_FIXTURES}/orders.ser)
set_tests_properties(wireshark_generate PROPERTIES LABELS wireshark
  PASS_REGULAR_EXPRESSION "ProtoField[.]uint32[(]\"serreg[.]corpus_Order[.]qty\", \"qty\", base[.]DEC[)]")

find_program(ES_SERREG_TSHARK tshark)
find_program(ES_SERREG_TEXT2PCAP text2pcap)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(ES_SERREG_TSHARK AND Python3_Interpreter_FOUND)
  set(es_serreg_ws_checks
      fixture:${ES_SERREG_GOLDEN_FIXTURES}/orders.ser fixture:${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser
      backpatched user0 mismatch fuzz prints:$<TARGET_FILE:wireshark_print_capture>)
  if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf") # dissectors generated from ELF sections (none of them without one)
    list(APPEND es_serreg_ws_checks
         elf:$<TARGET_FILE:make_fixtures>:${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
         elf:$<TARGET_FILE:make_dynamic_fixtures>:${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser)
  endif()
  set(es_serreg_ws_t2p)
  if(ES_SERREG_TEXT2PCAP)
    list(APPEND es_serreg_ws_checks udp tcp udp-heuristic tcp-heuristic)
    set(es_serreg_ws_t2p --text2pcap ${ES_SERREG_TEXT2PCAP})
  endif()
  add_test(NAME wireshark_dissector_parity
           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/parity.py --srdump $<TARGET_FILE:srdump>
                   --tshark ${ES_SERREG_TSHARK} ${es_serreg_ws_t2p} --fixtures ${ES_SERREG_GOLDEN_FIXTURES}
                   ${es_serreg_ws_checks})
else()
  add_test(NAME wireshark_dissector_parity
           COMMAND ${CMAKE_COMMAND} -E echo "SKIP: tshark or Python 3 not found; install tshark to run this test")
  set_tests_properties(wireshark_dissector_parity PROPERTIES DISABLED TRUE)
endif()
set_tests_properties(wireshark_dissector_parity PROPERTIES LABELS wireshark SKIP_RETURN_CODE 77 TIMEOUT 300)

# 2.1.11: the dissector under plain Lua 5.2 and 5.4 (tests/wireshark/wslua_stub.lua), one test per version found
set(es_serreg_lua_versions)
foreach(cand lua5.4 lua54 lua5.2 lua52 lua)
  find_program(ES_SERREG_LUA_${cand} ${cand})
  if(ES_SERREG_LUA_${cand})
    execute_process(COMMAND ${ES_SERREG_LUA_${cand}} -e "io.write(_VERSION)" OUTPUT_VARIABLE es_serreg_lua_v
                    RESULT_VARIABLE es_serreg_lua_rc ERROR_QUIET)
    if(es_serreg_lua_rc EQUAL 0 AND es_serreg_lua_v MATCHES "^Lua 5[.]([24])$")
      set(es_serreg_lua_tag "lua5${CMAKE_MATCH_1}")
      if(NOT es_serreg_lua_tag IN_LIST es_serreg_lua_versions)
        list(APPEND es_serreg_lua_versions ${es_serreg_lua_tag})
        set(es_serreg_lua_bin_${es_serreg_lua_tag} ${ES_SERREG_LUA_${cand}})
      endif()
    endif()
  endif()
endforeach()
set(es_serreg_stub_checks
    fixture:${ES_SERREG_GOLDEN_FIXTURES}/orders.ser fixture:${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser backpatched
    prints:$<TARGET_FILE:wireshark_print_capture>)
if(ES_SERREG_IMAGE_FORMAT STREQUAL "elf")
  list(APPEND es_serreg_stub_checks
       elf:$<TARGET_FILE:make_fixtures>:${ES_SERREG_GOLDEN_FIXTURES}/orders.ser
       elf:$<TARGET_FILE:make_dynamic_fixtures>:${ES_SERREG_GOLDEN_FIXTURES}/dynamic.ser)
endif()
foreach(tag lua52 lua54)
  if(Python3_Interpreter_FOUND AND es_serreg_lua_bin_${tag})
    add_test(NAME wireshark_dissector_${tag}
             COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/parity.py --srdump $<TARGET_FILE:srdump>
                     --lua ${es_serreg_lua_bin_${tag}} --fixtures ${ES_SERREG_GOLDEN_FIXTURES} ${es_serreg_stub_checks})
  else()
    add_test(NAME wireshark_dissector_${tag}
             COMMAND ${CMAKE_COMMAND} -E echo "SKIP: no ${tag} interpreter or no Python 3")
    set_tests_properties(wireshark_dissector_${tag} PROPERTIES DISABLED TRUE)
  endif()
  set_tests_properties(wireshark_dissector_${tag} PROPERTIES LABELS wireshark TIMEOUT 300)
endforeach()
