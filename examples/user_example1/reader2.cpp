// examples/user_example1/reader2.cpp — walks every value of any es::serreg capture using only the dictionary
// embedded in the file. It includes no application header: types.hpp is unknown here, and so is every type the
// writer used.
//
//   reader2 [--blocks] [file]    default: user_example1.ser
//
// tools::walk_values(path, visitor) walks each frame's records through the file's dictionary — members, nested
// structs, arrays, strings, vectors, optionals, variants, enums — and reports every value to a
// tools::decoded_value_visitor (tree_printer.hpp). The file is mapped, or read in 2 MiB blocks with --blocks.
#include "args.hpp"
#include "tree_printer.hpp"

#include <es/serreg/serreg.hpp>

#include <print>

namespace ue1
{
// Prints every frame header and every value of the capture from the file's own dictionary (no compiled types).
class generic_reader
{
public:
    explicit generic_reader(const reader_options& opts) : opts_{opts}
    {
    }

    int run()
    {
        const auto walk = tools::walk_values(
            opts_.path, out_,
            [this](const dyn::dictionary& d, const tools::frame_ref& f) { return frame(d, f); },
            opts_.file);
        if (!walk)
        {
            std::println(stderr, "{}: {}", opts_.path, to_string(walk.error()));
            return 1;
        }
        for (const auto& p : walk->problems)
            std::println("problem: {}", p);
        std::println("reader2: {} frames, {} records, {} values, {} types in the dictionary, {} "
                     "errors",
                     walk->frames, records_, out_.values, walk->dict.types.size(),
                     walk->problems.size());
        return walk->problems.empty() ? 0 : 1;
    }

private:
    const reader_options opts_;
    tree_printer out_;
    std::uint64_t records_ = 0;

    // Called before each frame's records: print the frame header, and for a log line the rendered message.
    bool frame(const dyn::dictionary& d, const tools::frame_ref& f)
    {
        std::println("frame {} @{}: {} record{}, call {:016x}{}", f.index, f.offset, f.n_records,
                     f.n_records == 1 ? "" : "s", f.call_hash, f.is_print() ? " (println)" : "");
        if (f.is_print())
            if (const auto text = tools::render_frame(f.bytes, d))
                std::print("  => {}", *text);
        records_ += f.n_records;
        return true;
    }
};
} // namespace ue1

int main(const int argc, char** const argv)
{
    return ue1::generic_reader{ue1::reader_options::parse(argc, argv)}.run();
}
