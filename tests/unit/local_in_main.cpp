// tests/unit/local_in_main.cpp — 2.1.12 (review C-05): a class local to main() is serialisable. GCC 16 cannot take
// parent_of(main), so until 2.1.11 such a class failed to compile with an uncaught std::meta::exception. Since
// 2.1.14 a class local to a function has no implicit canonical name (two functions in two TUs could produce the
// same one): it is named explicitly, here with the annotation.
// main() defines the local type (the point of the test) and hands it to the application class.
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

namespace local_in_main
{
template<class Local> class app
{
public:
    int run() const
    {
        const std::string_view name = es::serreg::canonical_name_v<Local>;
        std::vector<std::byte> buf;
        es::serreg::vector_sink sink{buf};
        const Local in{7, 2.5};
        Local out{};
        const bool round_trip = es::serreg::serialize(sink, in) &&
                                es::serreg::deserialize(buf, out) && out.id == 7 && out.px == 2.5;
        std::println("local_in_main: canonical name {}, round trip {}", name,
                     round_trip ? "ok" : "FAILED");
        return name == "local_in_main::Local" && round_trip ? 0 : 1;
    }
};
} // namespace local_in_main

int main()
{
    struct[[= es::serreg::name("local_in_main::Local")]] Local
    {
        std::int32_t id;
        double px;
    };
    return local_in_main::app<Local>{}.run();
}
