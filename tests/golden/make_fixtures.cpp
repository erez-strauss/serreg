// tests/golden/make_fixtures.cpp — writes the checked-in golden fixtures (cc F1 / plan §1.6 (b)):
//   <dir>/orders.ser    a FILE capture (header + dictionary + 11 frames: compact and full records; since format
//                       1.1 also a print frame, a println frame and a string/scalar frame; since format 1.5 a
//                       compact, a full-records and a println frame with compile-time constants)
//   <dir>/orders.rri    registry::export_image() of this process
//   <dir>/orders.text   tools::dump of orders.ser in text, JSON and TOML
//   <dir>/orders.json
//   <dir>/orders.toml
// Run ONCE by hand to create tests/fixtures/golden/*; ctest regenerates into the build directory and compares
// byte for byte. This executable uses exactly: types corpus::Order, corpus::Side, corpus::Cancel; literals
// "new order", "why", "evt", "hello {} {} {}", "worlds", "px={:.2f} qty={} side={}"; calls ("new order",Order),
// (Cancel,"why"), (Side), ("evt",Order), print("hello {} {} {}",string,i32,"worlds"),
// println("px=…",f64,u32,Side), (string,i32,f64,"evt"); format 1.5 adds literals "fill", "limit={} on={}" and
// calls ("fill",42_,Order,k<Side::sell>,-1.5_), println("limit={} on={}",5'000'000'000_,k<true>) → 20 registry
// entries. Anything that changes the bytes of
// any of these files is a wire-format or rendering change and needs a CHANGELOG entry (and a format_minor bump).
// The previous minor's files stay in tests/fixtures/corpus/capture/*-1.6.* as the compatibility corpus; 2.2.1 adds
// features.ser (CRC + checkpoints + writer info).
#include "corpus.hpp"

#include <print>
#include <string>

namespace make_fixtures
{
using namespace es::serreg;
using namespace es::serreg::literals;

// Writes the five golden files into one directory; run() returns the process exit code.
class app
{
public:
    explicit app(const std::string_view dir) : dir_{dir}, ser_{dir_ + "/orders.ser"}
    {
    }

    int run() const
    {
        if (const int rc = write_capture())
            return rc;
        if (const int rc = export_registry())
            return rc;
        if (const int rc = write_features())
            return rc;
        return write_dumps();
    }

private:
    std::string dir_;
    std::string ser_;

    // 1.7: the golden files are reproducible — a fixed wall clock and no host items in the writer-info frame
    static std::uint64_t fixed_clock() noexcept
    {
        return 1'700'000'000'000'000'000ull;
    }
    static capture_options golden_options()
    {
        capture_options o;
        o.info_host = false;
        o.clock = &fixed_clock;
        o.program = "make_fixtures";
        return o;
    }

    // orders.ser: 11 frames — compact and full records, print, println, a string/scalar frame, constants
    int write_capture() const
    {
        return write_frames(ser_, golden_options());
    }
    // features.ser (1.7): the same 11 frames with a CRC32C trailer on every frame and a checkpoint every 256
    // bytes of frames (a full dictionary in each) — the golden file of the 1.7 integrity features
    int write_features() const
    {
        capture_options o = golden_options();
        o.crc = true;
        o.checkpoint_bytes = 256;
        o.checkpoint_dictionary = true;
        return write_frames(dir_ + "/features.ser", o);
    }
    int write_frames(const std::string& path, const capture_options& co) const
    {
        capture_file f;
        if (!f.open(path, co))
        {
            std::println(stderr, "cannot open {}", path);
            return 1;
        }
        corpus::Order o = corpus::sample_order();
        if (!serialize(f, "new order"_lit, o))
            return 1;
        o.id = 43;
        o.px = -1;
        o.qty = 0;
        o.side = corpus::Side::sell;
        std::memcpy(o.sym, "MSFT\0\0\0", 8);
        if (!serialize(f, "new order"_lit, o))
            return 1;
        if (!serialize(f, corpus::sample_cancel(), "why"_lit))
            return 1;
        if (!serialize(f, corpus::Side::buy))
            return 1;
        if (!serialize_with(serialize_options{.full_records = true}, f, "evt"_lit,
                            corpus::sample_order()))
            return 1;
        // format 1.1: print / println / top-level string and scalar arguments (plan v1.1 §1-§2)
        if (!print(f, "hello {} {} {}"_, "new", 100, "worlds"_))
            return 1;
        if (!println(f, "px={:.2f} qty={} side={}"_, 2.5, 7u, corpus::Side::sell))
            return 1;
        if (!serialize(f, "abc", 42, 1.5, "evt"_))
            return 1;
        // format 1.5: compile-time constants — no bytes in the compact frame, scalar/enum records in the full one
        if (!serialize(f, "fill"_, 42_, corpus::sample_order(), k<corpus::Side::sell>, -1.5_))
            return 1;
        if (!serialize_with(serialize_options{.full_records = true}, f, "fill"_, 42_,
                            corpus::sample_order(), k<corpus::Side::sell>, -1.5_))
            return 1;
        if (!println(f, "limit={} on={}"_, 5'000'000'000_, k<true>))
            return 1;
        if (!f.close())
        {
            std::println(stderr, "close failed");
            return 1;
        }
        return 0;
    }

    // orders.rri: the registry of this process, which must hold exactly the 20 entries listed above
    int export_registry() const
    {
        if (registry::instance().size() != 20)
        {
            std::println(stderr, "registry has {} entries, expected 20",
                         registry::instance().size());
            return 1;
        }
        if (!registry::instance().export_image(dir_ + "/orders.rri"))
            return 1;
        return 0;
    }

    // orders.text / .json / .toml: the verified capture dumped in each format
    int write_dumps() const
    {
        const auto bytes = tools::read_file(ser_);
        if (!bytes)
            return 1;
        const auto rep = tools::verify(*bytes);
        if (!rep || !rep->ok() || rep->frames != 11 || rep->records != 34)
        {
            std::println(stderr, "fixture capture does not verify");
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
            if (!tools::write_file(dir_ + "/orders." + o.ext, b))
                return 1;
        }
        std::println("fixtures written to {}", dir_);
        return 0;
    }
};
} // namespace make_fixtures

int main(const int argc, char** const argv)
{
    if (argc < 2)
    {
        std::println(stderr, "usage: make_fixtures <out-dir>");
        return 2;
    }
    return make_fixtures::app{argv[1]}.run();
}
