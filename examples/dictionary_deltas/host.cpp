// examples/dictionary_deltas/host.cpp — a capture whose dictionary grows while it is written (README.md).
//
//   dictionary_deltas_host <out.ser> <plugin.so>... [--frames N]
//
// The host opens a capture_file (its opening dictionary holds what the registry has at that moment: the host's
// own types), writes N frames of its own feed::Order, then dlopen()s each plugin in turn and lets it write N
// frames of its own types. A plugin's types enter the one process registry when it is loaded, after the opening
// dictionary, so the first frame after each dlopen is preceded by a dictionary delta frame, linked forward from
// the previous dictionary (the opening block's CHAIN, then the previous delta frame's tail link) with one 8-byte
// patch. With checkpoints on (checkpoint_dictionary = true) every checkpoint also carries a full copy of the
// dictionary as of that point. After close() the host prints the file's layout with tools::info — the same
// seek-only walk `srdump --info` does — and a one-line summary.
//
// The file is reproducible (no host items in the writer-info frame, a fixed clock), so a run's bytes can be
// compared with the checked-in copy in tests/fixtures/corpus/capture/.
#include "messages.hpp"

#include <es/serreg/serreg.hpp>
#include <es/serreg/tools.hpp>

#include <dlfcn.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace dictionary_deltas
{
using namespace es::serreg;
using namespace es::serreg::literals;

struct options
{
    std::string_view out;
    std::vector<std::string_view> plugins;
    std::uint32_t frames = 3;
    bool ok = true;
};

options parse(const int argc, char** const argv)
{
    options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--frames" && i + 1 < argc)
        {
            const std::string_view v = argv[++i];
            if (std::from_chars(v.data(), v.data() + v.size(), o.frames).ec != std::errc{})
                o.ok = false;
        }
        else if (o.out.empty())
            o.out = a;
        else
            o.plugins.push_back(a);
    }
    if (o.out.empty() || o.plugins.empty())
        o.ok = false;
    return o;
}

class app
{
public:
    explicit app(const options& o) : opt_(o)
    {
    }

    int run() const
    {
        if (!opt_.ok)
        {
            std::println(stderr,
                         "usage: dictionary_deltas_host <out.ser> <plugin.so>... [--frames N]");
            return 2;
        }
        if (const int rc = write_capture())
            return rc;
        return describe();
    }

private:
    // 1.7 options: checkpoints every 96 bytes of frames, each with a full dictionary; reproducible writer info.
    static std::uint64_t fixed_clock() noexcept
    {
        static std::uint64_t t = 1'700'000'000'000'000'000ull;
        return t += 1'000'000;
    }
    static capture_options file_options() noexcept
    {
        capture_options o;
        o.checkpoint_bytes = 96;
        o.checkpoint_dictionary = true;
        o.program = "dictionary_deltas";
        o.info_host = false;
        o.clock = &fixed_clock;
        return o;
    }

    int write_capture() const
    {
        capture_file file;
        if (const auto r = file.open(opt_.out, file_options()); !r)
        {
            std::println(stderr, "dictionary_deltas: cannot open {}: {}", opt_.out,
                         to_string(r.error()));
            return 1;
        }
        // phase 0: the host's own frames; feed::Order is in the opening dictionary
        for (std::uint32_t i = 0; i < opt_.frames; ++i)
        {
            const feed::Order order{i, 1'050'000 + static_cast<std::int64_t>(i), 100 + i,
                                    i % 2 ? feed::Side::sell : feed::Side::buy, "AAPL"};
            if (!serialize(file, "new order"_lit, order))
            {
                std::println(stderr, "dictionary_deltas: serialize failed: {}",
                             to_string(file.state().error()));
                return 1;
            }
        }
        // phases 1..: each plugin's types reach the registry at dlopen(); its first frame gets a delta frame
        for (const std::string_view p : opt_.plugins)
        {
            const std::string path(p);
            void* const h = ::dlopen(path.c_str(), RTLD_NOW);
            if (!h)
            {
                std::println(stderr, "dictionary_deltas: dlopen {}: {}", p, ::dlerror());
                return 1;
            }
            registry::instance().refresh(); // publish the plugin's entries now, on this thread
            const auto write = reinterpret_cast<plugin_write_fn>(::dlsym(h, "plugin_write"));
            if (!write)
            {
                std::println(stderr, "dictionary_deltas: {} exports no plugin_write", p);
                return 1;
            }
            if (!write(&file, opt_.frames))
            {
                std::println(stderr, "dictionary_deltas: {} failed to write: {}", p,
                             to_string(file.state().error()));
                return 1;
            }
            // the plugin stays loaded: its code is not needed any more (the registry keeps heap copies of its
            // entries, decision A), but the capture_file is still open and an unload here would be pointless
        }
        if (const auto r = file.close(); !r)
        {
            std::println(stderr, "dictionary_deltas: close failed: {}", to_string(r.error()));
            return 1;
        }
        return 0;
    }

    // The layout as a reader finds it by seeks: header, dictionary chain, checkpoint chain, writer info.
    int describe() const
    {
        const auto info = tools::info(opt_.out);
        if (!info)
        {
            std::println(stderr, "dictionary_deltas: info failed: {}", to_string(info.error()));
            return 1;
        }
        std::print("{}", tools::to_text(*info));
        std::size_t full = 0;
        for (const auto& c : info->checkpoints)
            full += c.full_dictionary;
        std::println(
            "dictionary_deltas: wrote {} ({} bytes): {} dictionary blocks (the opening one + {} "
            "delta frames), {} checkpoints ({} with a full dictionary), {} types",
            opt_.out, info->file_size, info->dictionary_offsets.size(),
            info->dictionary_offsets.size() - 1, info->checkpoints.size(), full,
            info->dict.types.size());
        return 0;
    }

    options opt_;
};
} // namespace dictionary_deltas

int main(const int argc, char** const argv)
{
    return dictionary_deltas::app{dictionary_deltas::parse(argc, argv)}.run();
}
