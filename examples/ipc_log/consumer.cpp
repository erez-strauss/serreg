// examples/ipc_log/consumer.cpp — the reading half of the ipc_log example. Compiled with ONE of the producer's
// types (ipc_log::Fill, messages.hpp) and nothing else; it renders every other frame from the producer's
// dictionary, which arrives in band as the first ring record.
//
//   ipc_log_consumer --name serreg_ipc_log [--out text.txt] [--timeout-ms 20000]
//
// Per record (types: messages.hpp):
//   record_dictionary  tools::load_dictionary_rri(payload): the dictionary every later frame is rendered with;
//   record_frame       frame_router::route(frame) — a Fill goes to its typed handler (no dictionary involved);
//                      a frame without a handler is rendered by tools::render_frame(frame, dictionary) (the
//                      println text); a frame whose call is in no dictionary prints "unknown call 0x<hash>";
//                      anything else that cannot be rendered prints "undecodable frame: <error>" — never a crash;
//   record_end         the producer's frame count, checked against the frames received; then exit.
// Exit status: 0 when the stream ended and every frame arrived; 1 otherwise (2: usage).
//
// It attaches to the named region the producer created (retrying until it exists, or --timeout-ms), joins the
// SPSC queue, and then unlinks the name: the mapping stays valid, the producer sees the name gone and may exit.
#include "messages.hpp"

#include <es/ipc/ipc.hpp>
#include <es/ipc/region.hpp>
#include <es/serreg/serreg.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace ipc_log
{
using namespace es::serreg::literals;

struct options
{
    std::string name = "serreg_ipc_log";
    std::string out; // empty: stdout
    std::uint32_t timeout_ms = 20000;
};

class consumer_app
{
public:
    explicit consumer_app(options o) : o_(std::move(o))
    {
        // The one type this program knows: a typed handler, no dictionary needed.
        router_.on("fill"_, [this](const Fill& f) { std::print(out_, "{}", fill_text(f)); });
    }

    int run()
    {
        out_ = o_.out.empty() ? stdout : std::fopen(o_.out.c_str(), "w");
        if (!out_)
        {
            std::println(stderr, "ipc_log_consumer: cannot write {}", o_.out);
            return 1;
        }
        const int rc = consume();
        if (out_ != stdout && std::fclose(out_) != 0)
            return 1;
        return rc;
    }

private:
    int consume()
    {
        auto mem = attach_region();
        if (!mem)
            return 1;
        auto ring = es::ipc::ring::attach(*mem, es::ipc::ring_kind::spsc);
        auto cons = ring ? es::ipc::consumer::join(*ring) : std::unexpected(ring.error());
        if (!cons)
        {
            std::println(stderr, "ipc_log_consumer: ring: {}",
                         std::make_error_code(cons.error()).message());
            return 1;
        }
        // in: the name is no longer needed (the producer waits for this)
        es::ipc::region::remove(o_.name);
        while (!ended_)
        {
            if (cons->poll([this](const es::ipc::message& m) { on_record(m); }) == 0)
            {
                if (cons->lapped() || (!ring->has_producer() && cons->pending_units() == 0))
                    break; // the producer is gone and nothing is left (it never sent record_end)
                std::this_thread::yield();
            }
        }
        std::fflush(out_);
        std::println(
            stderr,
            "ipc_log_consumer: {} frames ({} typed, {} rendered, {} unknown, {} undecodable), "
            "dictionary {} bytes, corrupt records {}",
            frames_, router_.handled(), rendered_, unknown_, undecodable_, dict_bytes_,
            cons->corrupt());
        if (!ended_ || frames_ != expected_frames_ || cons->corrupt() != 0)
        {
            std::println(stderr,
                         "ipc_log_consumer: stream incomplete (ended={}, expected {} frames)",
                         ended_, expected_frames_);
            return 1;
        }
        return 0;
    }

    std::optional<es::ipc::region> attach_region() const
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(o_.timeout_ms);
        for (;;)
        {
            auto mem = es::ipc::region::open(o_.name);
            if (mem)
            { // created, sized; wait until the ring header is written (attach says try again)
                const auto r = es::ipc::ring::attach(*mem, es::ipc::ring_kind::spsc);
                if (r)
                    return std::move(*mem);
                if (r.error() != std::errc::resource_unavailable_try_again)
                {
                    std::println(stderr, "ipc_log_consumer: {}: {}", o_.name,
                                 std::make_error_code(r.error()).message());
                    return std::nullopt;
                }
            }
            else if (mem.error() != std::errc::no_such_file_or_directory &&
                     mem.error() != std::errc::resource_unavailable_try_again)
            {
                std::println(stderr, "ipc_log_consumer: {}: {}", o_.name,
                             std::make_error_code(mem.error()).message());
                return std::nullopt;
            }
            if (std::chrono::steady_clock::now() >= until)
            {
                std::println(stderr, "ipc_log_consumer: no queue named {} within {} ms", o_.name,
                             o_.timeout_ms);
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void on_record(const es::ipc::message& m)
    {
        switch (m.type)
        {
        case record_dictionary:
            if (auto d = es::serreg::tools::load_dictionary_rri(m.data))
            {
                dict_ = std::move(*d);
                dict_bytes_ = m.data.size();
            }
            else
                std::println(stderr, "ipc_log_consumer: dictionary refused: {}",
                             es::serreg::to_string(d.error()));
            break;
        case record_frame:
            ++frames_;
            on_frame(m.data);
            break;
        case record_end:
            if (m.data.size() == sizeof expected_frames_)
                std::memcpy(&expected_frames_, m.data.data(), sizeof expected_frames_);
            ended_ = true;
            break;
        default:
            std::println(stderr, "ipc_log_consumer: record type {} ignored", m.type);
            break;
        }
    }

    void on_frame(const std::span<const std::byte> frame)
    {
        // 1. A handler for this call signature: decoded into the C++ type.
        if (const auto r = router_.route(frame); r && *r)
            return;
        else if (!r)
        {
            ++undecodable_;
            std::print(out_, "undecodable frame: {}\n", es::serreg::to_string(r.error()));
            return;
        }
        // 2. No handler: rendered from the producer's dictionary.
        const auto text = es::serreg::tools::render_frame(frame, dict_);
        if (text)
        {
            ++rendered_;
            std::print(out_, "{}", *text);
            return;
        }
        if (text.error() == es::serreg::error::unknown_call)
        {
            ++unknown_;
            std::print(out_, "unknown call 0x{:016x}\n",
                       es::serreg::read_frame_header(frame)->call_hash);
            return;
        }
        ++undecodable_;
        std::print(out_, "undecodable frame: {}\n", es::serreg::to_string(text.error()));
    }

    options o_;
    std::FILE* out_ = nullptr;
    es::serreg::frame_router router_;
    es::serreg::dyn::dictionary dict_;
    std::size_t dict_bytes_ = 0;
    std::uint64_t frames_ = 0, rendered_ = 0, unknown_ = 0, undecodable_ = 0, expected_frames_ = 0;
    bool ended_ = false;
};

// --name N, --out FILE, --timeout-ms N
bool parse(const std::span<char* const> args, options& o)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view a = args[i];
        if (i + 1 >= args.size())
            return false;
        const std::string_view v = args[++i];
        if (a == "--name")
            o.name = v;
        else if (a == "--out")
            o.out = v;
        else if (a != "--timeout-ms" ||
                 std::from_chars(v.data(), v.data() + v.size(), o.timeout_ms).ec != std::errc{})
            return false;
    }
    return true;
}
} // namespace ipc_log

int main(const int argc, char** const argv)
{
    ipc_log::options o;
    if (!ipc_log::parse(std::span<char* const>(argv + 1, static_cast<std::size_t>(argc - 1)), o))
    {
        std::println(stderr,
                     "usage: ipc_log_consumer [--name REGION] [--out FILE] [--timeout-ms MS]");
        return 2;
    }
    return ipc_log::consumer_app{std::move(o)}.run();
}
