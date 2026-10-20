// ce-example6 — the "thirty seconds" of the Core C++ 2026 talk (slide 9), on Compiler Explorer (GCC 16,
// -std=c++26 -freflection). The program of the slide, as written: a capture_file, one serialize, close. Then the
// part the slide cannot show — the file is read back and taken apart with nothing but the dictionary it carries:
// the FILE header, the dictionary (RRI header + one ENTRY per type / literal / call signature), the frames, and
// a decode of the Order record driven by the dictionary alone. Finally the typed read.
//
// One translation unit: the library headers plus ce_registry_stub.hpp standing in for libserreg (Compiler
// Explorer links nothing else). The registry is still filled before main() by the same registrar guards. If the
// executor's sandbox refuses to create the file, the same frames go to an in-memory stream (write_dictionary_frame
// + serialize into a vector_sink) and the dump says so.
#include "ce_registry_stub.hpp"

#include <es/serreg/capture.hpp>
#include <es/serreg/deserialize.hpp>
#include <es/serreg/serialize.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace sr = es::serreg;
using namespace sr::literals;

// ---- the slide --------------------------------------------------------------------------------------------
namespace corpus
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
} // namespace corpus

int write_orders(const char* const path)
{
    sr::capture_file file(path); // header + dictionary written now
    if (!file.is_open())
        return 1;
    corpus::Order o{42, 1050000, 100, corpus::Side::buy, "AAPL"};
    if (!sr::serialize(file, "new order"_, o)) // one frame: 16-byte header + 32 bytes
        return 1;
    return file.close() ? 0 : 1;
}

// ---- what the file holds ------------------------------------------------------------------------------------
namespace
{
using bytes = std::span<const std::byte>;

std::string hex(const bytes b, const std::size_t from, const std::size_t n)
{
    std::string s;
    for (std::size_t i = from; i < from + n && i < b.size(); ++i)
        s += std::format("{:02x} ", std::to_integer<unsigned>(b[i]));
    return s;
}
template<class T> T load(const bytes b, const std::size_t at)
{
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
std::vector<std::byte> slurp(const char* const path)
{
    std::vector<std::byte> v;
    if (std::FILE* const f = std::fopen(path, "rb"))
    {
        std::byte buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

// The dictionary-only reader: walks type_desc::fields (name, kind, offset, size) over the record bytes — what
// srdump, the Python reader and the Wireshark dissector do, with no `Order` in this code path.
void decode_with_dictionary(const sr::type_desc& d, const bytes rec)
{
    std::println("  {} ({} bytes, schema {:#018x}):", d.name, d.size, d.schema_hash);
    for (const sr::field_desc& f : d.fields)
    {
        std::string v;
        switch (f.k)
        {
        case sr::kind::u64: v = std::to_string(load<std::uint64_t>(rec, f.offset)); break;
        case sr::kind::i64: v = std::to_string(load<std::int64_t>(rec, f.offset)); break;
        case sr::kind::u32: v = std::to_string(load<std::uint32_t>(rec, f.offset)); break;
        case sr::kind::enum_: v = std::to_string(std::to_integer<unsigned>(rec[f.offset])); break;
        case sr::kind::array_: // char[8]: the text up to the first NUL
            v = '"' + std::string(reinterpret_cast<const char*>(rec.data() + f.offset),
                                  strnlen(reinterpret_cast<const char*>(rec.data() + f.offset), f.size)) + '"';
            break;
        default: v = "(" + std::string(sr::to_string(f.k)) + ")";
        }
        std::println("    {:<5} {:<9} @{:<2} [{}]  = {}", f.name, sr::to_string(f.k), f.offset, f.size, v);
    }
}

const char* tag_name(const std::uint8_t tag)
{
    switch (tag)
    {
    case 1: return "struct type";
    case 2: return "enum type";
    case 3: return "literal";
    case 4: return "call signature";
    default: return "?";
    }
}

// Takes the file apart from its fixed layouts (format.hpp): FILE header | RRI header | ENTRY × n | CHAIN | frames.
int explain(const bytes f, const bool is_file)
{
    std::println("{} bytes\n", f.size());
    std::size_t at = 0;
    if (is_file)
    {
        std::println("FILE header, {} bytes:", sr::fmt::file_header_size);
        std::println("  {}  magic \"{}{}{}{}\" major {} minor {} flags {:#x} hash_algo {}", hex(f, 0, 16),
                     static_cast<char>(f[0]), static_cast<char>(f[1]), static_cast<char>(f[2]),
                     static_cast<char>(f[3]), load<std::uint16_t>(f, 4), load<std::uint16_t>(f, 6),
                     load<std::uint32_t>(f, 8), load<std::uint32_t>(f, 12));
        std::println("  {}  program_hash {:#018x} dict_offset {}", hex(f, 16, 16), load<std::uint64_t>(f, 16),
                     load<std::uint64_t>(f, 24));
        std::println("  {}  dict_len {} abi_tag {:#x}", hex(f, 32, 16), load<std::uint64_t>(f, 32),
                     load<std::uint64_t>(f, 40));
        at = load<std::uint64_t>(f, 24);
    }
    else
    { // a stream: the dictionary is a frame; its RRI starts after the 16-byte frame header
        std::println("dictionary frame header: {}  frame_len {} flags {:#x} (bit1 = dictionary) call {:#x}",
                     hex(f, 0, 16), load<std::uint32_t>(f, 0), load<std::uint16_t>(f, 4),
                     load<std::uint64_t>(f, 8));
        at = sr::fmt::frame_header_size;
    }

    const std::size_t rri = at;
    const std::uint32_t n_entries = load<std::uint32_t>(f, rri + 16);
    std::println("\nRRI header at {}, {} bytes: magic \"ESRG\" {}.{} n_entries {} program_hash {:#018x}", rri,
                 sr::fmt::rri_header_size, load<std::uint16_t>(f, rri + 4), load<std::uint16_t>(f, rri + 6),
                 n_entries, load<std::uint64_t>(f, rri + 20));
    at = rri + sr::fmt::rri_header_size;
    std::println("ENTRY × {} (u32 total_len, u8 tag, u8 version, u16 rsv, body):", n_entries);
    const sr::type_desc* order = nullptr;
    for (std::uint32_t i = 0; i < n_entries; ++i)
    {
        const std::uint32_t len = load<std::uint32_t>(f, at);
        const std::uint8_t tag = std::to_integer<std::uint8_t>(f[at + 4]);
        std::println("  {:>5}  {} …  {:<14} {} bytes", at, hex(f, at, 16), tag_name(tag), len);
        if (tag == 1 || tag == 2) // the hash of a type ENTRY is its schema hash, right after the 8-byte header
            if (const sr::type_desc* const d = sr::registry::instance().find_type(load<std::uint64_t>(f, at + 8));
                d && d->name == std::string_view{"corpus::Order"})
                order = d;
        at += len;
    }
    if (is_file)
    {
        at = (at + 7) & ~std::size_t{7};
        std::println("CHAIN at {}: next_dictionary {} first_checkpoint {}", at, load<std::uint64_t>(f, at),
                     load<std::uint64_t>(f, at + 8));
        at += sr::fmt::chain_size;
    }
    else
        at = load<std::uint32_t>(f, 0); // the frame_len of the dictionary frame

    std::println("\nframes:");
    bytes order_rec;
    while (at + sr::fmt::frame_header_size <= f.size())
    {
        const std::uint32_t len = load<std::uint32_t>(f, at);
        const std::uint16_t flags = load<std::uint16_t>(f, at + 4);
        const std::uint16_t n_rec = load<std::uint16_t>(f, at + 6);
        const std::uint64_t call = load<std::uint64_t>(f, at + 8);
        const char* what = call == sr::fmt::info_sentinel         ? "writer-info frame (versions, options)"
                           : call == sr::fmt::dict_frame_sentinel ? "dictionary frame"
                                                                  : "call frame";
        std::println("  {:>5}  {}  {}: frame_len {} flags {:#x} n_records {} call {:#018x}", at,
                     hex(f, at, 16), what, len, flags, n_rec, call);
        if (call != sr::fmt::info_sentinel && call != sr::fmt::dict_frame_sentinel)
        {
            std::println("  {:>5}  {}  record 0: the literal, 0 bytes; record 1: Order, id px", at + 16,
                         hex(f, at + 16, 16));
            std::println("  {:>5}  {}  qty side (pad) sym", at + 32, hex(f, at + 32, 16));
            std::println("         call hash == call_hash_v<lit<\"new order\">, Order>: {}",
                         call == sr::call_hash_v<sr::lit<"new order">, corpus::Order>);
            order_rec = f.subspan(at + 16, 32);
        }
        if (len < sr::fmt::frame_header_size)
            break;
        at += len;
    }
    if (!order || order_rec.empty())
        return 1;
    std::println("\ndecoded from the dictionary record alone (no Order in this code path):");
    decode_with_dictionary(*order, order_rec);

    corpus::Order back{};
    const bytes frame = f.subspan(order_rec.data() - f.data() - 16, 48);
    if (!sr::deserialize(frame, "new order"_, back))
        return 1;
    const bool same = back.id == 42 && back.px == 1050000 && back.qty == 100 && back.side == corpus::Side::buy &&
                      std::strncmp(back.sym, "AAPL", 8) == 0;
    std::println("\ntyped read: id {} px {} qty {} side {} sym {}  ({})", back.id, back.px, back.qty,
                 static_cast<int>(back.side), back.sym, same ? "identical" : "DIFF");
    return same ? 0 : 1;
}
} // namespace

int main()
{
    std::println("registry before main(): {} entries (struct, enum, literal, call signature)\n",
                 sr::ce_stub::count);
    const char* const path = "orders.ser";
    if (write_orders(path) == 0)
    {
        const std::vector<std::byte> f = slurp(path);
        std::remove(path); // read into memory; leave nothing behind (srdump orders.ser works on it too)
        std::println("orders.ser written by capture_file — the slide's program — and read back: ");
        return explain(f, true);
    }
    // no writable directory in this sandbox: the stream form, same frames, dictionary as a frame
    std::vector<std::byte> stream;
    sr::vector_sink out{stream};
    if (!sr::write_dictionary_frame(out))
        return 1;
    corpus::Order o{42, 1050000, 100, corpus::Side::buy, "AAPL"};
    if (!sr::serialize(out, "new order"_, o))
        return 1;
    std::println("(no writable file here) in-memory stream: dictionary frame + call frame, ");
    return explain(stream, false);
}
