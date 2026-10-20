// tests/python/log_capture.cpp — writes a small 1.7 capture for tests/python/parity.py: log frames of both
// envelope policies (plain; per-message thread id + per-sink sequence), a logger call without a call site, a
// println frame and one plain record frame, all under a CRC32C trailer and a deterministic clock, so the
// pure-Python reader (python/serreg) can be compared with srdump --json / --verify / --info on it.
//
//   python_log_capture <out.ser>
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string>

namespace py_log
{
using namespace es::serreg;
using namespace es::serreg::literals;

struct capture_scope
{
};
struct Position
{
    std::int32_t id;
    double qty;
};
enum class Venue : std::uint8_t
{
    nyse = 1,
    nasdaq = 2,
};

namespace
{
std::uint64_t ticks = 1'700'000'000'000'000'000ull;
std::uint64_t fixed() noexcept
{
    return ticks += 1'000; // strictly increasing, deterministic
}
} // namespace

using file_t = scoped_capture_file<capture_scope>;
constexpr log_options plain{.clock = clock_policy::user};
constexpr log_options traced{.thread = thread_policy::per_message,
                             .sequence = sequence_policy::per_sink,
                             .clock = clock_policy::user};

class writer
{
public:
    explicit writer(std::string path) : path_{std::move(path)}
    {
    }

    int run() const
    {
        capture_options co{.crc = true, .info_host = false, .clock = &fixed};
        co.log_clock = "user";
        file_t f(path_, co);
        if (!f.is_open())
        {
            std::println(stderr, "cannot open {}", path_);
            return 1;
        }
        logger<plain, file_t> a{f};
        logger<traced, file_t> b{f};
        a.set_clock(&fixed);
        b.set_clock(&fixed);
        const std::string sym = "AAPL";
        const bool ok =
            ES_SERREG_LOG(a, info, "started with {} workers"_, 4) &&
            ES_SERREG_LOG(a, warn, "book {} depth {:>4}"_, sym, 7) &&
            ES_SERREG_LOG(b, error, "px={:.2f} order={} venue={}"_, 1.25, 42, Venue::nasdaq) &&
            ES_SERREG_LOG(b, debug, "pos {} flag={} big={}"_, Position{1, 2.5}, true,
                          std::int64_t{1} << 60) &&
            a.trace("no call site {:#x} {}"_, 255u, "lit"_) &&
            println(f, "plain print {} {}"_, "txt"_, 3) &&
            serialize(f, "evt"_lit, Position{2, -0.5});
        return ok && f.close() ? 0 : 1;
    }

private:
    std::string path_;
};
} // namespace py_log

int main(const int argc, char** const argv)
{
    return py_log::writer{argc > 1 ? argv[1] : "log.ser"}.run();
}
