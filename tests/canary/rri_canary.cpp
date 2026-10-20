// tests/canary/rri_canary.cpp — format-drift canary (cc F1 / plan §1.6 (c)).
//
// Decodes tests/fixtures/golden/orders.rri from LITERAL byte offsets taken from the layout comment at the top of
// include/es/serreg/format.hpp — this file deliberately includes NO es/serreg header and links nothing from the
// library, so a change to the encoder and decoder that keeps them consistent with each other but moves the format
// still fails here. Expected values are the frozen golden hashes (tests/golden/hashes.cpp) and the known geometry of
// corpus::Cancel { u64 id; u32 reason; } (size 16), found among the struct entries by its name.
//
// Layout used (all little-endian, from format.hpp):
//   RRI header (40 bytes): "ESRG" | u16 major | u16 minor | u32 flags | u32 hash_algo | u32 n_entries | u64 program_hash | u32 rsv
//                          | u64 abi_tag (byte 0 version 1, 1 endian, 2 ptr size, 3 long, 4 wchar, 5 char signed, 6 max align, 7 ldbl)
//   ENTRY header (8):      u32 total_len (multiple of 8) | u8 tag (1 struct, 2 enum, 3 literal, 4 call) | u8 version (1) | u16 rsv
//   struct body:           u64 schema_hash | u64 layout_hash | u32 size | u32 align | u8 flags | u8 rsv | u16 since | u16 deprecated
//                          | u16 name_len | u16 n_fields | name bytes | FIELD × n_fields
//   FIELD:                 u16 name_len | u8 kind | u8 rsv | u32 offset | u32 size | u16 align | u16 since | u16 deprecated
//                          | u16 n_elem | u64 type_hash | u32 extent | name bytes | FIELD × n_elem
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>

namespace
{

int failures = 0;
#define EXPECT(cond, what)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::println(stderr, "canary: FAIL: {} ({})", what, #cond);                            \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

template<class T> T rd(const std::vector<unsigned char>& b, const std::size_t off)
{
    T v{};
    if (off + sizeof(T) > b.size())
    {
        std::println(stderr, "canary: read past end at {}", off);
        std::exit(1);
    }
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

} // namespace

namespace rri_canary
{
// rri_canary <orders.rri> [minor n_entries n_literals n_calls]   (defaults: the current 1.7 image)
struct options
{
    const char* path = nullptr;
    unsigned want_minor = 7;
    unsigned want_entries = 20;
    unsigned want_lits = 8;
    unsigned want_calls = 9;

    static options parse(const int argc, char** const argv)
    {
        options o;
        if (argc > 1)
            o.path = argv[1];
        if (argc > 2)
            o.want_minor = static_cast<unsigned>(std::atoi(argv[2]));
        if (argc > 3)
            o.want_entries = static_cast<unsigned>(std::atoi(argv[3]));
        if (argc > 4)
            o.want_lits = static_cast<unsigned>(std::atoi(argv[4]));
        if (argc > 5)
            o.want_calls = static_cast<unsigned>(std::atoi(argv[5]));
        return o;
    }
};

// Reads the file, then checks the header, the entry stream and the corpus::Cancel entry at literal offsets.
class app
{
public:
    explicit app(const options& opts) : opts_{opts}
    {
    }

    int run()
    {
        if (!opts_.path)
        {
            std::println(stderr, "usage: rri_canary <orders.rri>");
            return 2;
        }
        if (const int rc = read_file())
            return rc;
        check_header();
        if (const int rc = walk_entries())
            return rc;
        check_cancel_entry();
        if (failures)
        {
            std::println(stderr, "canary: {} failure(s)", failures);
            return 1;
        }
        std::println("canary: ok ({} entries, corpus::Cancel schema 0x{:016x})", n_entries_,
                     schema_);
        return 0;
    }

private:
    options opts_;
    std::vector<unsigned char> b_;
    std::uint32_t n_entries_ = 0;
    std::size_t first_struct_ = 0; // the corpus::Cancel struct entry
    std::uint64_t schema_ = 0;

    int read_file()
    {
        std::FILE* const f = std::fopen(opts_.path, "rb");
        if (!f)
        {
            std::println(stderr, "canary: cannot open {}", opts_.path);
            return 1;
        }
        unsigned char tmp[4096];
        for (std::size_t n; (n = std::fread(tmp, 1, sizeof tmp, f)) > 0;)
            b_.insert(b_.end(), tmp, tmp + n);
        std::fclose(f);
        return 0;
    }

    // ---- RRI header ----
    void check_header()
    {
        const std::vector<unsigned char>& b = b_;
        const unsigned want_minor = opts_.want_minor, want_entries = opts_.want_entries;
        EXPECT(b.size() >= 40, "file holds an RRI header");
        EXPECT(std::memcmp(b.data(), "ESRG", 4) == 0, "magic is ESRG");
        EXPECT(rd<std::uint16_t>(b, 4) == 1, "major == 1");
        EXPECT(rd<std::uint16_t>(b, 6) == want_minor, "minor == expected");
        EXPECT(rd<std::uint32_t>(b, 8) == 0, "flags == 0 (not a delta)");
        EXPECT(rd<std::uint32_t>(b, 12) == 2, "hash_algo == 2 (XXH3-64)");
        const std::uint32_t n_entries = rd<std::uint32_t>(b, 16);
        n_entries_ = n_entries;
        EXPECT(n_entries == want_entries, "n_entries == expected (3 types + literals + calls)");
        const std::uint64_t program_hash = rd<std::uint64_t>(b, 20);
        EXPECT(program_hash != 0, "program_hash present");
        EXPECT(rd<std::uint32_t>(b, 28) == 0, "reserved == 0");
        EXPECT(rd<std::uint8_t>(b, 32) == 1, "abi tag version == 1");
        EXPECT(rd<std::uint8_t>(b, 33) == 0, "abi tag: little-endian writer");
        EXPECT(rd<std::uint8_t>(b, 34) == 8, "abi tag: 8-byte pointers (x86-64 fixture)");
    }

    // ---- walk the ENTRY stream ----
    int walk_entries()
    {
        const std::vector<unsigned char>& b = b_;
        const std::uint32_t n_entries = n_entries_;
        const unsigned want_lits = opts_.want_lits, want_calls = opts_.want_calls;
        std::size_t first_struct = 0; // the corpus::Cancel struct entry
        std::size_t off = 40, count = 0, tags[5] = {0, 0, 0, 0, 0};
        unsigned const_calls = 0; // 1.5
        while (off < b.size())
        {
            const std::uint32_t total = rd<std::uint32_t>(b, off);
            const std::uint8_t tag = rd<std::uint8_t>(b, off + 4);
            EXPECT(total >= 8 && total % 8 == 0, "entry total_len is a positive multiple of 8");
            EXPECT(rd<std::uint8_t>(b, off + 5) == 1, "entry version == 1");
            EXPECT(rd<std::uint16_t>(b, off + 6) == 0, "entry reserved == 0");
            EXPECT(tag >= 1 && tag <= 4, "entry tag in 1..4");
            if (tag >= 1 && tag <= 4)
                ++tags[tag];
            // struct body: name_len at +8+30, name at +8+34
            if (tag == 1 && first_struct == 0 && off + 8 + 34 + 14 <= b.size() &&
                rd<std::uint16_t>(b, off + 8 + 30) == 14 &&
                std::memcmp(b.data() + off + 8 + 34, "corpus::Cancel", 14) == 0)
                first_struct = off;
            // format 1.5: the call ("fill", 42_, Order, k<Side::sell>, -1.5_) — the only call with five arguments.
            // call body: u64 hash | u16 n_args | u16 flags | u32 rsv | ARG × n_args, ARG = u8 arg_kind | u8 kind |
            // 6 rsv | u64 hash; a constant's ARG (arg_kind 4) is followed by u64 value.
            if (tag == 4 && off + 24 <= b.size() && rd<std::uint16_t>(b, off + 16) == 5)
            {
                ++const_calls;
                EXPECT(total == 8 + 16 + 16 + 24 + 16 + 24 + 24,
                       "const call: 2 plain + 3 constant ARGs");
                if (total == 128 && off + total <= b.size())
                {
                    EXPECT(rd<std::uint8_t>(b, off + 24) == 1, "arg 0 is a literal");
                    EXPECT(rd<std::uint8_t>(b, off + 40) == 4, "arg 1 is a constant (arg_kind 4)");
                    EXPECT(rd<std::uint8_t>(b, off + 41) == 3, "arg 1 kind == i32 (3)");
                    EXPECT(rd<std::uint64_t>(b, off + 56) == 42, "arg 1 value == 42");
                    EXPECT(rd<std::uint8_t>(b, off + 64) == 0, "arg 2 is a type");
                    EXPECT(rd<std::uint8_t>(b, off + 80) == 4, "arg 3 is a constant");
                    EXPECT(rd<std::uint8_t>(b, off + 81) == 0, "arg 3 kind == 0 (an enum)");
                    EXPECT(rd<std::uint64_t>(b, off + 96) == 2, "arg 3 value == Side::sell (2)");
                    EXPECT(rd<std::uint8_t>(b, off + 104) == 4, "arg 4 is a constant");
                    EXPECT(rd<std::uint8_t>(b, off + 105) == 10, "arg 4 kind == f64 (10)");
                    EXPECT(rd<std::uint64_t>(b, off + 120) == 0xbff8000000000000ull,
                           "arg 4 value == the bits of -1.5");
                }
            }
            if (total < 8 || off + total > b.size())
            {
                std::println(stderr, "canary: bad entry length at {}", off);
                return 1;
            }
            off += total;
            ++count;
        }
        EXPECT(off == b.size(), "entries end exactly at end of file");
        EXPECT(count == n_entries, "walked entry count equals header n_entries");
        EXPECT(tags[1] == 2, "two struct entries (Order, Cancel)");
        EXPECT(tags[2] == 1, "one enum entry (Side)");
        EXPECT(tags[3] == want_lits, "literal entry count");
        EXPECT(tags[4] == want_calls, "call entry count");
        EXPECT(opts_.want_minor < 5 || const_calls == 1, "the call with constants exists (1.5)");
        EXPECT(first_struct != 0, "the corpus::Cancel struct entry exists");
        if (first_struct == 0)
            return 1;
        first_struct_ = first_struct;
        return 0;
    }

    // ---- the corpus::Cancel struct entry ----
    void check_cancel_entry()
    {
        const std::vector<unsigned char>& b = b_;
        const std::size_t first_struct = first_struct_;
        const std::size_t p = first_struct + 8;
        const std::uint64_t schema = rd<std::uint64_t>(b, p);
        schema_ = schema;
        const std::uint64_t layout = rd<std::uint64_t>(b, p + 8);
        EXPECT(schema == 0x35af6843b770c519ull, "schema hash == golden corpus::Cancel");
        EXPECT(layout == 0x99c125fd4fe8673cull, "layout hash == golden corpus::Cancel");
        EXPECT(rd<std::uint32_t>(b, p + 16) == 16, "size == 16");
        EXPECT(rd<std::uint32_t>(b, p + 20) == 8, "align == 8");
        EXPECT(rd<std::uint8_t>(b, p + 24) == 1, "flags bit0 (is_flat) set");
        EXPECT(rd<std::uint8_t>(b, p + 25) == 0, "struct reserved byte == 0");
        EXPECT(rd<std::uint16_t>(b, p + 26) == 0, "since == 0");
        EXPECT(rd<std::uint16_t>(b, p + 28) == 0, "deprecated == 0");
        const std::uint16_t name_len = rd<std::uint16_t>(b, p + 30);
        const std::uint16_t n_fields = rd<std::uint16_t>(b, p + 32);
        EXPECT(name_len == 14, "name_len == strlen(\"corpus::Cancel\")");
        EXPECT(n_fields == 2, "n_fields == 2");
        const std::string name(reinterpret_cast<const char*>(b.data() + p + 34), name_len);
        EXPECT(name == "corpus::Cancel", "name == corpus::Cancel");
        // FIELD 0: id  (u64 at 0)   FIELD 1: reason (u32 at 8)
        std::size_t fp = p + 34 + name_len;
        const char* const field_names[2] = {"id", "reason"};
        const std::uint32_t field_off[2] = {0, 8}, field_size[2] = {8, 4};
        const std::uint16_t field_align[2] = {8, 4};
        const std::uint8_t field_kind[2] = {
            8 /*u64*/, 7 /*u32*/}; // kind enum: i8=1 i16 i32 i64 u8=5 u16 u32 u64=8
        for (int i = 0; i < 2; ++i)
        {
            const std::uint16_t fnl = rd<std::uint16_t>(b, fp);
            EXPECT(fnl == std::strlen(field_names[i]), "field name_len");
            EXPECT(rd<std::uint8_t>(b, fp + 2) == field_kind[i], "field kind");
            EXPECT(rd<std::uint8_t>(b, fp + 3) == 0, "field reserved == 0");
            EXPECT(rd<std::uint32_t>(b, fp + 4) == field_off[i], "field offset");
            EXPECT(rd<std::uint32_t>(b, fp + 8) == field_size[i], "field size");
            EXPECT(rd<std::uint16_t>(b, fp + 12) == field_align[i], "field align");
            EXPECT(rd<std::uint16_t>(b, fp + 14) == 0, "field since == 0");
            EXPECT(rd<std::uint16_t>(b, fp + 16) == 0, "field deprecated == 0");
            EXPECT(rd<std::uint16_t>(b, fp + 18) == 0, "field n_elem == 0 (scalar)");
            EXPECT(rd<std::uint64_t>(b, fp + 20) == 0, "field type_hash == 0 (scalar)");
            EXPECT(rd<std::uint32_t>(b, fp + 28) == 0, "field extent == 0 (scalar)");
            const std::string fname(reinterpret_cast<const char*>(b.data() + fp + 32), fnl);
            EXPECT(fname == field_names[i], "field name");
            fp += 32 + fnl;
        }
        // entry ends at the 8-byte padded position
        const std::uint32_t total = rd<std::uint32_t>(b, first_struct);
        EXPECT(((fp - first_struct) + 7) / 8 * 8 == total,
               "struct entry total_len == padded body length");
        EXPECT(total == 128,
               "corpus::Cancel ENTRY is 128 bytes"); // frozen in tests/golden/blobs.cpp
    }
};
} // namespace rri_canary

int main(const int argc, char** const argv)
{
    return rri_canary::app{rri_canary::options::parse(argc, argv)}.run();
}
