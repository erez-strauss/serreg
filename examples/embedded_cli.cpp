// examples/embedded_cli.cpp — an application that embeds the srdump command line (es::serreg::cli, v1.0.3):
// every srdump flag works under the application's own name, plus the application's flags, and with no
// arguments the application does its own work — here: writes a capture and dumps its own registry.
//
//   embedded_cli --version                     → "embedded_cli 0.1 (format 1.3, hash_algo 2)"
//   embedded_cli --elf ./embedded_cli --toml   → the dictionary compiled into this very binary
//   embedded_cli --trace --selftest out.ser  → application flags + the srdump surface on out.ser
#include <es/serreg/serreg.hpp>

#include <print>
#include <span>
#include <string>
#include <string_view>

namespace app
{
struct Tick
{
    std::uint64_t ts;
    double px;
    std::uint32_t qty;
};
} // namespace app

namespace embedded_cli
{
using namespace es::serreg;

// The application: the whole srdump command line under its own name (cli::app + cli::options), two flags of
// its own, and a default action when the srdump surface has nothing to do.
class application
{
public:
    application() = default;
    application(const application&) = delete; // me_.on_extra points back at this object
    application& operator=(const application&) = delete;

    // srdump's flags go into options_, ours through on_extra(). Returns 0 to go on, else the exit code (2 after a
    // usage error).
    int parse(const int argc, char** const argv)
    {
        return cli::parse(argc, argv, options_, me_);
    }

    int run() const
    {
        if (verbose_)
            std::println(stderr, "embedded_cli: {} registry entries, selftest={:d}",
                         registry::instance().size(), selftest_);
        if (nothing_for_srdump())
            return dump_own_registry();
        return cli::execute(options_, me_);
    }

private:
    bool verbose_ = false;
    bool selftest_ = false;
    cli::options options_;
    const cli::app me_{.name = "embedded_cli",
                       .version = "0.1",
                       .extra_usage =
                           "  --trace              say what the application does\n"
                           "  --selftest <file>    write three Tick frames to <file> first\n",
                       .on_extra = [this](const std::string_view a, std::span<char* const>& rest) {
                           return on_extra(a, rest);
                       }};

    // The application's own flags: --trace (--verbose is srdump's own), and --selftest <file>, which consumes its value.
    bool on_extra(const std::string_view a, std::span<char* const>& rest)
    {
        if (a == "--trace")
            return verbose_ = true;
        if (a == "--selftest" && !rest.empty())
        {
            selftest_ = true;
            write_ticks(rest.front());
            rest = rest.subspan(1);
            return true;
        }
        return false;
    }

    static void write_ticks(const char* const path)
    {
        capture_file f{path};
        for (std::uint32_t i = 0; i < 3; ++i)
            static_cast<void>(serialize<"tick">(f, app::Tick{i, 1.5 * i, i}));
        static_cast<void>(f.close());
    }

    bool nothing_for_srdump() const
    {
        const cli::options& o = options_;
        return o.capture.empty() && !o.use_stdin && o.elfs.empty() && o.dicts.empty() &&
               o.rri.empty() && o.wireshark.empty() && o.diff_a.empty() && !o.help && !o.version;
    }

    // The application's own default action: print the dictionary of its own registry.
    static int dump_own_registry()
    {
        std::string text;
        if (const auto d = tools::load_dictionary_rri(registry::instance().export_image());
            d && tools::dump_dictionary(*d, tools::format::text, text))
            std::print("{}", text);
        return 0;
    }
};
} // namespace embedded_cli

int main(const int argc, char** const argv)
{
    embedded_cli::application program;
    if (const int rc = program.parse(argc, argv))
        return rc;
    return program.run();
}
