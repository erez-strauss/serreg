// examples/user_example1/reader3.cpp — the hybrid reader: compiled with demo::Order only (not Book, not Fill as
// a handler), it gets every Order as a C++ object — the ones inside the Book frame included — and everything else
// walked field by field from the file's dictionary.
//
//   reader3 [--blocks] [file]    default: user_example1.ser
//
// tools::walk_values<demo::Order>: for each record and then each nested value, outermost first, a value whose
// schema hash equals schema_hash_v<demo::Order> is decoded into a demo::Order and handed to the lambda (its
// interior is not walked); all other values go to the tree printer. The Book record does not match, so the
// printer sees "demo::Book {", venue, trade_date, fee_bps, "orders: vector[3]" — and then each element is an
// Order, which is taken over and delivered typed.
#include "args.hpp"
#include "tree_printer.hpp"
#include "types.hpp"

#include <es/serreg/serreg.hpp>

#include <print>
#include <string_view>

namespace ue1
{
// Walks the capture with demo::Order compiled in: every Order arrives as a C++ object, everything else is
// printed generically by the tree printer.
class hybrid_reader
{
public:
    explicit hybrid_reader(const reader_options& opts) : opts_{opts}
    {
    }

    int run()
    {
        const auto walk = tools::walk_values<demo::Order>(
            opts_.path,
            [this](const std::string_view field, const demo::Order& o) { typed_order(field, o); },
            rest_,
            [](const dyn::dictionary&, const tools::frame_ref& f) {
                std::println("frame {}{}", f.index, f.is_print() ? " (println)" : "");
                return true;
            },
            opts_.file);
        if (!walk)
        {
            std::println(stderr, "{}: {}", opts_.path, to_string(walk.error()));
            return 1;
        }
        for (const auto& p : walk->problems)
            std::println("problem: {}", p);
        // 3 orders inside the book + 3 order frames = 6 typed Orders
        std::println(
            "reader3: {} typed orders ({} fills inside them), {} other values walked generically",
            typed_orders_, fills_seen_typed_, rest_.values);
        return walk->problems.empty() && typed_orders_ == 6 ? 0 : 1;
    }

private:
    const reader_options opts_;
    tree_printer rest_;
    int typed_orders_ = 0;
    std::uint64_t fills_seen_typed_ = 0;

    // An Order, delivered typed — the ones inside the Book record as well as each "order"_ frame.
    void typed_order(const std::string_view field, const demo::Order& o)
    {
        ++typed_orders_;
        fills_seen_typed_ += o.fills.size();
        std::println("{:{}}{}: <typed demo::Order> id {} {} qty {}, {} fills{}{}", "",
                     2 * rest_.depth, field.empty() ? "element" : field, o.id, o.symbol, o.qty,
                     o.fills.size(), o.note ? ", note: " : "", o.note.value_or(""));
    }
};
} // namespace ue1

int main(const int argc, char** const argv)
{
    return ue1::hybrid_reader{ue1::reader_options::parse(argc, argv)}.run();
}
