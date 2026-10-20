// examples/print_sinks.cpp — print / println into two sinks: an archive whose dictionary is the whole registry,
// and a log whose dictionary holds only the calls made into it; and how the names resolve — `sr::println(...)`
// by qualification, `println(...)` by ADL on the sink argument, with std::println in scope the whole time.
//   ./print_sinks [dir]        (default: the current directory; writes print_sinks.ser and print_sinks.log.ser)
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace sr = es::serreg;    // the short spelling this example is about
using namespace sr::literals; // the _ literal operator (an alias does not bring it along)

namespace app
{
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    Side side;
};
struct Fill
{
    std::uint64_t order_id;
    std::int64_t px;
    std::uint32_t qty;
};
struct Book
{
    std::uint32_t depth;
    std::vector<std::int64_t> bids, asks; // a dynamic type: a canonical image on the wire
};

// A dictionary scope: the log file's dictionary is the closure of the calls made into sinks of this type —
// the format strings below and nothing else of the program (compare the two dictionaries in `run`).
struct log_scope
{
};
using log_file = sr::scoped_capture_file<log_scope>;

// A user sink in the user's namespace: `println(mine, ...)` is still found by ADL, because the associated
// namespaces of a class include those of its bases (es::serreg::vector_sink) and, independently, the
// literal argument lit<F> lives in es::serreg.
class counting_sink : public sr::vector_sink
{
public:
    explicit counting_sink(std::vector<std::byte>& out) : sr::vector_sink{out}
    {
    }
};

class program
{
public:
    static int run(const std::string& dir)
    {
        sr::capture_file archive(dir + "/print_sinks.ser"); // scope void: the whole registry
        log_file log(dir + "/print_sinks.log.ser",
                     sr::capture_options{.crc = true}); // this scope only
        if (!archive.is_open() || !log.is_open())
            return 1;

        // --- the archive: typed messages ---------------------------------------------------------------
        if (!sr::serialize(archive, "new order"_, Order{42, 1050000, 100, Side::sell}))
            return 1;
        if (!sr::serialize(archive, "fill"_, Fill{42, 1050000, 100}))
            return 1;
        if (!sr::serialize(archive, Book{3, {1049900, 1049800}, {1050100}}))
            return 1;

        // --- the log: print frames, three spellings of the same call ----------------------------------
        // 1. qualified: the reading most code should use
        if (!sr::println(log, "started with {} workers"_, 4))
            return 1;
        // 2. unqualified: found by argument-dependent lookup — `log` is an es::serreg type, so the candidate
        //    set is es::serreg::println (and std::println, which is not viable: a sink is not a FILE* or a
        //    format string). No using-directive is in scope here.
        if (!println(log, "book {} depth {}"_, "AAPL", 3))
            return 1;
        // 3. the format string as a template argument
        if (!sr::println<"order {} rejected: {}">(log, std::uint64_t{43}, "price band"))
            return 1;
        // 4. print (no newline), and std::println in the same scope, unambiguous
        if (!sr::print(log, "px={:.2f} "_, 105.0))
            return 1;
        std::println(
            "wrote 3 archive frames and 4 log frames"); // std::println: a format string, not a sink

        // 5. a user-defined sink type: ADL still finds es::serreg::println, because a base class's
        //    namespace is an associated namespace of the derived type, and so is the literal's
        std::vector<std::byte> bytes;
        counting_sink mine{bytes};
        if (!println(mine, "user sink {}"_, 1))
            return 1;

        if (!archive.close() || !log.close())
            return 1;

        // --- what each dictionary holds ---------------------------------------------------------------
        for (const char* const name : {"/print_sinks.ser", "/print_sinks.log.ser"})
        {
            const auto info = sr::tools::info(dir + name);
            if (!info)
                return 1;
            std::println("{:<22} dictionary: {} types, {} literals, {} calls", name + 1,
                         info->dict.types.size(), info->dict.literals.size(),
                         info->dict.calls.size());
        }
        return 0;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    return app::program::run(argc > 1 ? argv[1] : ".");
}
