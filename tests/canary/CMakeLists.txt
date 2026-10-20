# Format-drift canary (cc F1 / plan §1.6 (c)): decodes the checked-in orders.rri from literal offsets with no
# es/serreg header and no link against libserreg. Deliberately NOT given the include directory or the library.
add_executable(rri_canary rri_canary.cpp)
es_serreg_apply_warnings(rri_canary)
# the current image (format 1.7: 40-byte RRI header, hash_algo 2, ABI tag; 20 entries: 3 types, 8 literals, 9 calls)
add_test(NAME format_canary_rri COMMAND rri_canary ${ES_SERREG_GOLDEN_FIXTURES}/orders.rri 7 20 8 9)
set_tests_properties(format_canary_rri PROPERTIES PASS_REGULAR_EXPRESSION "^canary: ok")
