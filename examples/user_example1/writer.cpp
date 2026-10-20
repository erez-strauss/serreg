// examples/user_example1/writer.cpp — writes a capture file with a complex (dynamic) data structure, per-order
// frames with a literal tag, and binary log lines (println).
//
//   writer [file]            default: user_example1.ser
//
// Every type used here is registered before main() just because it is serialized somewhere in this program; the
// capture file starts with that dictionary, so reader2 can decode the file without these headers.
#include "types.hpp"

#include <es/serreg/serreg.hpp>

#include <print>
#include <string>

namespace ue1
{
using namespace es::serreg;
using namespace es::serreg::literals; // "text"_ : a literal, interned by the registry

// Writes sample_book() to one capture file: a log line, the whole book, each order with its fills, a closing line.
class writer
{
public:
    explicit writer(const std::string_view path) : path_{path}, book_{demo::sample_book()}
    {
    }

    int run() const
    {
        capture_file out(path_); // FILE header + the registry's dictionary (RRI) are written here
        if (!out.is_open())
        {
            std::println(stderr, "cannot open {}: {}", path_, to_string(out.state().error()));
            return 1;
        }
        if (!write_frames(out))
            return 1;
        // close() back-patches the dictionary if the registry grew meanwhile (a dlopen'ed plugin); [[nodiscard]].
        if (const auto c = out.close(); !c)
        {
            std::println(stderr, "close failed: {}", to_string(c.error()));
            return 1;
        }
        std::println("wrote {}", path_);
        return 0;
    }

private:
    std::string path_;
    demo::Book book_;

    bool write_frames(capture_file& out) const
    {
        // 1. A log line. The format string is checked against the argument types at compile time; on the wire it
        //    is a 16-byte header, the string's bytes and 8 bytes of count. The reader runs std::format.
        if (!println(out, "session {} {} opened with {} orders"_, book_.venue, book_.trade_date,
                     book_.orders.size()))
            return false;

        // 2. The whole book in one frame: one record, a canonical image of Book, orders/fills/strings included.
        if (!serialize(out, book_))
            return false;

        // 3. Each order again as its own frame, tagged with a literal (0 bytes on the wire: the tag is part of the
        //    call signature), followed by one log line per fill.
        for (const demo::Order& o : book_.orders)
        {
            if (!serialize(out, "order"_, o))
                return false;
            for (const demo::Fill& f : o.fills)
                if (!println(out, "  fill #{} of order {}: {} @ {:.2f}"_, f.exec_id, o.id, f.qty,
                             f.px))
                    return false;
        }

        if (!println(out, "session {} closed"_, book_.venue))
            return false;
        return true;
    }
};
} // namespace ue1

int main(const int argc, char** const argv)
{
    return ue1::writer{argc > 1 ? argv[1] : "user_example1.ser"}.run();
}
