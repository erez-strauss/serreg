// tests/link/validate_read_consumer.cpp — B3 (2.2.6, DECISIONS A-16): ES_SERREG_VALIDATE_READ is a whole-program
// switch. This one-file consumer is compiled and linked by ctest against the built libserreg twice: with the
// library's value it links (validate_read_match_links); with the other value the fuse in registry.hpp leaves
// `validate_read_build<N>` undefined and the link must fail (validate_read_mismatch_fails_to_link).
#include "link_types.hpp"

#include <es/serreg/serreg.hpp>

#include <cstddef>
#include <vector>

namespace validate_read_consumer
{
struct app
{
    int run() const
    {
        std::vector<std::byte> out;
        es::serreg::vector_sink s{out};
        if (!es::serreg::serialize(s, lnk::Msg{1, lnk::Kind::a, 1}))
            return 1;
        lnk::Msg m{};
        return es::serreg::deserialize(out, m) && m.seq == 1 ? 0 : 2;
    }
};
} // namespace validate_read_consumer

int main()
{
    return validate_read_consumer::app{}.run();
}
