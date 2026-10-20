// examples/file_store.cpp — write a capture file (header + dictionary + frames), read it back, verify and dump.
//   ./file_store [path]         (default: file_store.ser in the current directory)
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string>

namespace ns
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
    char sym[8];
};
struct Cancel
{
    std::uint64_t id;
    std::uint32_t reason;
};
} // namespace ns

namespace file_store
{
using namespace es::serreg;
using namespace es::serreg::literals;

// Writes a capture file, then reads it back: verify against the embedded dictionary and print the text dump.
class app
{
public:
    explicit app(const std::string_view path) : path_{path}
    {
    }

    int run() const
    {
        if (const int rc = write())
            return rc;
        return read_back();
    }

private:
    std::string path_;

    // The dictionary snapshot goes out at open(), one frame per serialize() call.
    int write() const
    {
        capture_file out(path_);
        if (!out.is_open())
        {
            std::println(stderr, "cannot open {}", path_);
            return 1;
        }
        for (std::uint64_t i = 1; i <= 3; ++i)
        {
            const ns::Order o{i, 1050000 + static_cast<std::int64_t>(i),
                              100u * static_cast<std::uint32_t>(i), ns::Side::buy, "AAPL"};
            if (!serialize(out, "new order"_lit, o))
                return 1;
        }
        if (!serialize(out, "cancel"_lit, ns::Cancel{2, 7}, "user"_lit))
            return 1;
        if (!serialize(out, "eod"_lit))
            return 1;
        // close() appends a fresh dictionary and back-patches the header if the registry grew meanwhile (dlopen);
        // it is [[nodiscard]]: an I/O failure here means the dictionary may be incomplete.
        if (const auto c = out.close(); !c)
        {
            std::println(stderr, "close failed: {}", to_string(c.error()));
            return 1;
        }
        return 0;
    }

    // Verify the structure against the embedded dictionary, then print the text dump.
    int read_back() const
    {
        const auto bytes = tools::read_file(path_);
        if (!bytes)
        {
            std::println(stderr, "read failed: {}", to_string(bytes.error()));
            return 1;
        }
        const auto rep = tools::verify(*bytes);
        if (!rep)
        {
            std::println(stderr, "verify failed: {}", to_string(rep.error()));
            return 1;
        }
        std::println("{}: {} frames, {} records, {} bytes, {}", path_, rep->frames, rep->records,
                     rep->bytes, rep->ok() ? "ok" : "PROBLEMS");
        for (const auto& p : rep->problems)
            std::println("  problem: {}", p);

        std::string text;
        if (const auto r = tools::dump(*bytes, nullptr, tools::dump_options{}, text); !r)
        {
            std::println(stderr, "dump failed");
            return 1;
        }
        std::print("{}", text);
        return rep->ok() ? 0 : 1;
    }
};
} // namespace file_store

int main(const int argc, char** const argv)
{
    return file_store::app{argc > 1 ? argv[1] : "file_store.ser"}.run();
}
