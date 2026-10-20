# dlopen/dlclose tests (spec §10.2 #3, §6.3b, FR-R6/R7/R10/R11/R12; v1.0.2: gk 1.5, cc F11/F12/F13/F15/F16; 2.1.13:
# decision A — heap copies at load, no abort, retained entries, reload). Needs the
# shared libserreg: one registry per process is what makes a plugin's entries visible to the host.
if(NOT ES_SERREG_BUILD_SHARED)
  message(STATUS "es::serreg: dlopen tests need ES_SERREG_BUILD_SHARED=ON; skipped")
  return()
endif()

function(es_serreg_plugin name src)
  add_library(${name} MODULE ${src})
  target_link_libraries(${name} PRIVATE es::serreg)
  set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}" CXX_VISIBILITY_PRESET hidden)
  es_serreg_apply_warnings(${name})
  target_compile_definitions(${name} PRIVATE ${ARGN})
endfunction()

es_serreg_plugin(serreg_plugin        plugin.cpp)
es_serreg_plugin(serreg_plugin_b      plugin.cpp ES_PLUGIN_NS=plugin_b)
es_serreg_plugin(serreg_plugin_c      plugin.cpp ES_PLUGIN_NS=plugin_c)
es_serreg_plugin(serreg_plugin_v2     plugin.cpp ES_PLUGIN_V2)   # 2.1.13: the same plugin, Tick with one more member
es_serreg_plugin(serreg_plugin_nested plugin_nested.cpp)
es_serreg_plugin(serreg_plugin_conf_a plugin_conf_a.cpp)
es_serreg_plugin(serreg_plugin_conf_b plugin_conf_b.cpp)
es_serreg_plugin(serreg_plugin_alias  plugin_alias.cpp)
es_serreg_plugin(serreg_plugin_scoped plugin_scoped.cpp)   # 2.2.1: a scoped sink inside a plugin (P-16, A-9)

# cc F12: a plugin built WITHOUT any of the flags the library recommends (-fno-gnu-unique, hidden visibility):
# the ES_SERREG_HIDDEN attributes alone must keep every per-image symbol out of STB_GNU_UNIQUE, otherwise the DSO
# becomes DF_1_NODELETE: glibc never unloads it, and a reload does not run its new code (2.1.13). Linked against the library file directly so that the
# PUBLIC -fno-gnu-unique of the es::serreg target is NOT inherited.
add_library(serreg_plugin_nounique MODULE plugin.cpp)
target_compile_definitions(serreg_plugin_nounique PRIVATE ES_PLUGIN_NS=plugin_nu ES_SERREG_SHARED)
target_include_directories(serreg_plugin_nounique PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(serreg_plugin_nounique PRIVATE ${ES_SERREG_REFLECTION_FLAG})
target_link_libraries(serreg_plugin_nounique PRIVATE $<TARGET_FILE:serreg> ${CMAKE_DL_LIBS} Threads::Threads)
add_dependencies(serreg_plugin_nounique serreg)
set_target_properties(serreg_plugin_nounique PROPERTIES PREFIX "" OUTPUT_NAME "serreg_plugin_nounique")

add_executable(dlopen_host host.cpp)
target_link_libraries(dlopen_host PRIVATE es::serreg ${CMAKE_DL_LIBS} Threads::Threads)
es_serreg_apply_warnings(dlopen_host)
add_dependencies(dlopen_host serreg_plugin serreg_plugin_b serreg_plugin_c serreg_plugin_v2 serreg_plugin_nested serreg_plugin_conf_a serreg_plugin_conf_b serreg_plugin_nounique serreg_plugin_alias)

set(_P  $<TARGET_FILE:serreg_plugin>)
set(_PB $<TARGET_FILE:serreg_plugin_b>)
set(_PC $<TARGET_FILE:serreg_plugin_c>)
# 2.1.13 (decision A): no test sets ES_SERREG_ALLOW_DLCLOSE any more; dlclose never aborts.
add_test(NAME dlopen_A_dlclose_keeps_entries    COMMAND dlopen_host ${_P} keep)
add_test(NAME dlopen_B_reload_revives_entries   COMMAND dlopen_host ${_P} reload)
add_test(NAME dlopen_C_rtld_nodelete            COMMAND dlopen_host ${_P} nodelete)
add_test(NAME dlopen_D_nested_dlclose           COMMAND dlopen_host ${_PB} nested $<TARGET_FILE:serreg_plugin_nested>)
add_test(NAME dlopen_E_changed_layout_reload    COMMAND dlopen_host ${_P} changed $<TARGET_FILE:serreg_plugin_v2>)
add_test(NAME dlopen_F_concurrent_dlclose       COMMAND dlopen_host ${_PB} concurrent ${_PC})
# cc F15/F16: two plugins with conflicting layouts of one schema hash; 2.1.13: the second takes over when the first unloads
add_test(NAME dlopen_H_conflicting_plugins COMMAND dlopen_host $<TARGET_FILE:serreg_plugin_conf_a> conflict $<TARGET_FILE:serreg_plugin_conf_b>)
# cc F12: a plugin built without the recommended flags still unloads; its entries are retained
add_test(NAME dlopen_I_plugin_without_flags_unloads COMMAND dlopen_host $<TARGET_FILE:serreg_plugin_nounique> nounique)
# 2.2.1 (P-16, A-9): a plugin's (scope, call) node is deep-copied; scoped exports and deltas include what the
# plugin's call brought into the scope; the node is retained after dlclose
add_test(NAME dlopen_M_scoped_sink_in_plugin COMMAND dlopen_host $<TARGET_FILE:serreg_plugin_scoped> scoped)
# cc F13: readers (export, snapshot, lookups) during dlopen/dlclose churn
add_test(NAME dlopen_J_readers_vs_dlclose_churn COMMAND dlopen_host ${_PB} churn)
set_tests_properties(dlopen_J_readers_vs_dlclose_churn PROPERTIES TIMEOUT 120)
# 2.1.5 (review G2-10): a plugin made only of aliases still notifies on_change observers; and no image of a normal
# process prints the Q10 "two copies" diagnostic
add_test(NAME dlopen_K_alias_only_plugin_notifies COMMAND dlopen_host $<TARGET_FILE:serreg_plugin_alias> alias)
set_tests_properties(dlopen_K_alias_only_plugin_notifies PROPERTIES FAIL_REGULAR_EXPRESSION "two copies of libserreg")
# 2.1.5 (review G2-5): loads and unloads on two threads at once; a lock-order inversion would hang (alarm 60 s)
add_test(NAME dlopen_L_two_thread_load_unload COMMAND dlopen_host ${_PB} churn2 ${_PC})
set_tests_properties(dlopen_L_two_thread_load_unload PROPERTIES TIMEOUT 120)
# 2.1.5 (review Q10): a second copy of libserreg in the process is named on stderr at startup
add_executable(registry_second_copy second_copy.cpp)
target_link_libraries(registry_second_copy PRIVATE es::serreg)
set_target_properties(registry_second_copy PROPERTIES ENABLE_EXPORTS ON)
es_serreg_apply_warnings(registry_second_copy)
add_test(NAME registry_second_copy_diagnostic COMMAND registry_second_copy)
set_tests_properties(registry_second_copy_diagnostic PROPERTIES
  PASS_REGULAR_EXPRESSION "es::serreg: two copies of libserreg in one process \\(.*registry_second_copy and .*libserreg[.]so")

if(TARGET srdump)
  add_test(NAME srdump_elf_conflicting_plugins_exit_1 COMMAND srdump --elf $<TARGET_FILE:serreg_plugin_conf_a> --elf $<TARGET_FILE:serreg_plugin_conf_b>)
  set_tests_properties(srdump_elf_conflicting_plugins_exit_1 PROPERTIES WILL_FAIL TRUE)
  add_test(NAME srdump_elf_conflicting_plugins_message COMMAND srdump --elf $<TARGET_FILE:serreg_plugin_conf_a> --elf $<TARGET_FILE:serreg_plugin_conf_b>)
  set_tests_properties(srdump_elf_conflicting_plugins_message PROPERTIES PASS_REGULAR_EXPRESSION "schema conflict: conf::Msg")
  add_test(NAME srdump_elf_single_plugin_exit_0 COMMAND srdump --elf $<TARGET_FILE:serreg_plugin_conf_a>)
  set_tests_properties(srdump_elf_single_plugin_exit_0 PROPERTIES PASS_REGULAR_EXPRESSION "conf::Msg")
endif()
