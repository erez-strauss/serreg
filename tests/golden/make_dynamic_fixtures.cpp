// tests/golden/make_dynamic_fixtures.cpp — writes the v2 golden fixtures (docs/plans-for-v2.0.md §5):
//   <dir>/dynamic.ser    a FILE capture with 9 frames: (DynMsg), (DynNested, DynArr), full-records (DynBase),
//                        println("…", DynMsg) — every dynamic member kind, nested, empty and engaged; 2.2.0
//                        (format 1.6): (DynMap) with map/set members, a top-level std::vector<i32> (compact,
//                        empty, and full records) and println("…", std::vector<i32>, DynMap)
//   <dir>/dynamic.text   tools::dump of dynamic.ser in text, JSON and TOML
//   <dir>/dynamic.json
//   <dir>/dynamic.toml
// A separate executable from make_fixtures so orders.rri keeps its 20 entries. Regenerate by hand after a
// deliberate format change (+ CHANGELOG entry):  ./build/<preset>/tests/golden/make_dynamic_fixtures tests/fixtures/golden
#include "corpus.hpp"

#include <print>
#include <string>

namespace make_dynamic_fixtures
{
using namespace es::serreg;
using namespace es::serreg::literals;

// Writes dynamic.ser and its three dumps into one directory; run() returns the process exit code.
class app
{
public:
    explicit app(const std::string_view dir) : dir_{dir}, ser_{dir_ + "/dynamic.ser"}
    {
    }

    int run() const
    {
        if (const int rc = write_capture())
            return rc;
        return write_dumps();
    }

private:
    std::string dir_;
    std::string ser_;

    // dynamic.ser: 4 frames covering every dynamic member kind of 2.0, then the five 1.6 frames
    int write_capture() const
    {
        capture_file f;
        capture_options co; // 1.7: reproducible — no host items, a fixed clock
        co.info_host = false;
        co.clock = [] noexcept -> std::uint64_t { return 1'700'000'000'000'000'000ull; };
        co.program = "make_dynamic_fixtures";
        if (!f.open(ser_, co))
            return 1;
        if (!serialize(f, corpus::sample_dyn_msg()))
            return 1;
        if (!serialize(f, corpus::sample_dyn_nested(), corpus::sample_dyn_arr()))
            return 1;
        if (!serialize_with(serialize_options{.full_records = true}, f, corpus::sample_dyn_base()))
            return 1;
        if (!println(f, "msg={} note={}"_, corpus::sample_dyn_msg(), "text"))
            return 1;
        // 2.2.0 (format 1.6): containers as members and as arguments
        if (!serialize(f, "map"_, corpus::sample_dyn_map()))
            return 1;
        const std::vector<std::int32_t> ids{3, 1, 2};
        if (!serialize(f, "ids"_, ids))
            return 1;
        if (!serialize(f, "ids"_, std::vector<std::int32_t>{}))
            return 1;
        if (!serialize_with(serialize_options{.full_records = true}, f, "ids"_, ids))
            return 1;
        if (!println(f, "ids={} map={}"_, ids, corpus::sample_dyn_map()))
            return 1;
        if (!f.close())
            return 1;
        return 0;
    }

    // dynamic.text / .json / .toml: the verified capture dumped in each format
    int write_dumps() const
    {
        const auto bytes = tools::read_file(ser_);
        if (!bytes)
            return 1;
        const auto rep = tools::verify(*bytes);
        if (!rep || !rep->ok() || rep->frames != 9 || rep->records != 18)
        {
            std::println(stderr, "dynamic fixture does not verify ({} problems)",
                         rep ? rep->problems.size() : std::size_t{0});
            if (rep)
                for (const auto& p : rep->problems)
                    std::println(stderr, "  {}", p);
            return 1;
        }
        struct out
        {
            tools::format f;
            const char* ext;
        };
        for (const out& o : {out{tools::format::text, "text"}, out{tools::format::json, "json"},
                             out{tools::format::toml, "toml"}})
        {
            std::string text;
            tools::dump_options opt;
            opt.fmt = o.f;
            if (!tools::dump(*bytes, nullptr, opt, text))
                return 1;
            const std::span<const std::byte> b(reinterpret_cast<const std::byte*>(text.data()),
                                               text.size());
            if (!tools::write_file(dir_ + "/dynamic." + o.ext, b))
                return 1;
        }
        std::println("dynamic fixtures written to {}", dir_);
        return 0;
    }
};
} // namespace make_dynamic_fixtures

int main(const int argc, char** const argv)
{
    if (argc < 2)
    {
        std::println(stderr, "usage: make_dynamic_fixtures <out-dir>");
        return 2;
    }
    return make_dynamic_fixtures::app{argv[1]}.run();
}
