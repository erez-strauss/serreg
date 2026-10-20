// examples/user_example1/reader1.cpp — reads writer.cpp's capture back into the same C++ types.
//
//   reader1 [--blocks] [file]    default: user_example1.ser
//
// One frame_router holds a handler per call signature the writer used; each frame goes to the handler of its call
// hash (one table lookup) and is decoded into the handler's parameter types. tools::for_each_frame(path) maps the
// file, or reads it in blocks with --blocks.
#include "args.hpp"
#include "types.hpp"

#include <es/serreg/serreg.hpp>

#include <print>
#include <string_view>

namespace ue1
{
using namespace es::serreg;
using namespace es::serreg::literals;

// Reads the capture into the writer's own types and compares the book with sample_book().
class typed_reader
{
public:
    explicit typed_reader(const reader_options& opts) : opts_{opts}
    {
        router_
            .on([this](const demo::Book& b) { on_book(b); }) // serialize(out, book)
            .on("order"_,
                [this](const demo::Order& o) { // serialize(out, "order"_, order)
                    ++orders_;
                    print_order(o);
                })
            .on_println("  fill #{} of order {}: {} @ {:.2f}"_,
                        [this](const std::uint64_t exec_id, const std::uint64_t order_id,
                               const std::int32_t qty, const double px) {
                            ++fills_;
                            std::println("    fill {}: order {}, {} @ {:.2f}", exec_id, order_id,
                                         qty, px);
                        })
            .otherwise([this](const dyn::dictionary& d, const tools::frame_ref& f) {
                other_frame(d, f); // the other log lines
            });
    }

    int run()
    {
        const auto walk = tools::for_each_frame(opts_.path, router_.callback(), opts_.file);
        if (!walk)
        {
            std::println(stderr, "{}: {}", opts_.path, to_string(walk.error()));
            return 1;
        }
        for (const auto& p : walk->problems)
            std::println("problem: {}", p);
        const bool ok = walk->problems.empty() && router_.failed() == 0 && book_matches_;
        std::println("reader1: {} book, {} orders, {} fills, {} other log lines, {}", books_,
                     orders_, fills_, lines_, ok ? "matches the writer" : "MISMATCH");
        return ok && books_ == 1 && orders_ == 3 && fills_ == 3 ? 0 : 1;
    }

private:
    const reader_options opts_;
    frame_router router_;
    int books_ = 0, orders_ = 0, fills_ = 0, lines_ = 0;
    bool book_matches_ = false;

    void on_book(const demo::Book& book)
    {
        ++books_;
        book_matches_ = book == demo::sample_book();
        std::println("book {} {}, fees {:.2f}/{:.2f}/{:.2f} bps, {} orders", book.venue,
                     book.trade_date, book.fee_bps[0], book.fee_bps[1], book.fee_bps[2],
                     book.orders.size());
        for (const auto& o : book.orders)
            print_order(o);
    }
    void other_frame(const dyn::dictionary& d, const tools::frame_ref& f)
    {
        if (!f.is_print())
        {
            std::println("frame {}: a signature this reader does not know", f.index);
            return;
        }
        ++lines_; // any other log line: let the library run std::format on it
        const auto text = tools::render_frame(f.bytes, d);
        std::print("log: {}", text ? std::string_view(*text) : "<cannot render>\n");
    }
    static void print_order(const demo::Order& o)
    {
        std::print("  order {} {} {} {}, ", o.id, o.side == demo::Side::buy ? "buy" : "sell",
                   o.symbol, o.qty);
        if (const auto* l = std::get_if<demo::Limit>(&o.type))
            std::print("limit {:.2f} tif {}s", l->px, l->tif_seconds);
        else if (const auto* m = std::get_if<demo::Market>(&o.type))
            std::print("market max slippage {} bp", m->max_slippage_bp);
        std::println(", {} fills{}{}", o.fills.size(), o.note ? ", note: " : "",
                     o.note.value_or(""));
    }
};
} // namespace ue1

int main(const int argc, char** const argv)
{
    return ue1::typed_reader{ue1::reader_options::parse(argc, argv)}.run();
}
