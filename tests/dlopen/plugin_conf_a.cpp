// tests/dlopen/plugin_conf_a.cpp — the genuine definition of conf::Msg (cc F15/F16 conflict test, FR-R4).
//   types: conf::Msg = 1, calls: (Msg) = 1  → 2 new entries at dlopen()
#include "conf_msg.hpp"

#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" [[gnu::visibility("default")]] int conf_run(std::vector<std::byte>* const out)
{
    es::serreg::vector_sink s{*out};
    return es::serreg::serialize(s, conf::Msg{1, 2}) ? 0 : 1;
}
