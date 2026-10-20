// tests/unit/runtime_selftest.cpp — framework-free self-test of the non-reflection runtime + tools layer
// (dictionary parsing/round-trip, parser bounds, conflicts, capture walking, verify incl. the decoder, dump
// (string and streaming), diff, JSON 64-bit rendering, ELF reader, cli). Spec §6.5, §6.7, §6.9; FR-W2, FR-W3,
// FR-W5, FR-D1..D3, FR-D5, FR-D6, FR-L3; review items cc F2/F15/F16/F17/F18/F19/F24, gk 1.4/1.9/1.10/1.11/1.12/1.15.
// Plain asserts; exits non-zero on the first failure.
//
//   runtime_selftest [out.ser [fixtures-dir]]
//     out.ser       also writes the capture (and out.ser.rri) for manual srdump runs
//     fixtures-dir  regenerates tests/fixtures/{corpus/<target>/*, negative/*} (seed corpora and the review bombs)
#include <es/serreg/dictionary.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/tools.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <vector>

using namespace es::serreg;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace
{

constexpr std::uint64_t h_point = 0x1111, h_color = 0x2222, h_pixel = 0x3333, lit_hello = 0x4444,
                        h_call = 0x5555;
constexpr std::uint64_t h_bomb = 0x6666, h_bomb_call = 0x6667, h_big = 0x7777, h_big64 = 0x8888,
                        h_big_call = 0x9999;

// Small growable byte builder on top of fmt::writer (writes into a fixed scratch area, then appends).
struct builder
{
    std::vector<std::byte> out;
    template<class T> void put(const T v)
    {
        std::array<std::byte, sizeof(T)> m;
        fmt::writer w{m.data()};
        w.put(v);
        out.insert(out.end(), m.begin(), m.end());
    }
    void str16(const std::string_view s)
    {
        put(static_cast<std::uint16_t>(s.size()));
        for (const char c : s)
            out.push_back(static_cast<std::byte>(c));
    }
    void raw(const std::string_view s)
    {
        for (const char c : s)
            out.push_back(static_cast<std::byte>(c));
    }
    void bytes(const std::vector<std::byte>& b)
    {
        out.insert(out.end(), b.begin(), b.end());
    }
    void pad8()
    {
        while (out.size() % 8)
            out.push_back(std::byte{0});
    }
};

// FIELD: u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated, u16 n_elem,
//        u64 type_hash, u32 extent, name bytes, FIELD × n_elem
void field(builder& b, const std::string_view name, const kind k, const std::uint32_t off,
           const std::uint32_t size, const std::uint16_t align, const std::uint64_t type_hash = 0,
           const std::uint32_t extent = 0, const std::uint16_t n_elem = 0,
           const std::uint16_t since = 0)
{
    b.put(static_cast<std::uint16_t>(name.size()));
    b.put(static_cast<std::uint8_t>(k));
    b.put(std::uint8_t{0});
    b.put(off);
    b.put(size);
    b.put(align);
    b.put(since);
    b.put(std::uint16_t{0});
    b.put(n_elem);
    b.put(type_hash);
    b.put(extent);
    b.raw(name);
}

void entry(builder& out, const std::uint8_t tag, const builder& body)
{
    const std::uint32_t total =
        static_cast<std::uint32_t>(fmt::align8(fmt::entry_header_size + body.out.size()));
    out.put(total);
    out.put(tag);
    out.put(std::uint8_t{1});
    out.put(std::uint16_t{0});
    out.out.insert(out.out.end(), body.out.begin(), body.out.end());
    out.pad8();
}

void type_head(builder& b, const std::uint64_t schema, const std::uint64_t layout,
               const std::uint32_t size, const std::uint32_t align,
               const std::uint8_t flags_or_underlying, const std::string_view name,
               const std::uint16_t n)
{
    b.put(schema);
    b.put(layout);
    b.put(size);
    b.put(align);
    b.put(flags_or_underlying);
    b.put(std::uint8_t{0});
    b.put(std::uint16_t{0});
    b.put(std::uint16_t{0});
    b.put(static_cast<std::uint16_t>(name.size()));
    b.put(n);
    b.raw(name);
}

void call_entry(builder& e, const std::uint64_t hash,
                const std::vector<std::pair<bool, std::uint64_t>>& args)
{
    builder b;
    b.put(hash);
    b.put(static_cast<std::uint16_t>(args.size()));
    b.put(std::uint16_t{0});
    b.put(std::uint32_t{0});
    for (const auto& [lit, h] : args)
    {
        b.put(std::uint8_t{lit ? fmt::arg_literal : fmt::arg_type});
        for (int i = 0; i < 7; ++i)
            b.put(std::uint8_t{0});
        b.put(h);
    }
    entry(e, fmt::tag_call, b);
}

std::vector<std::byte> make_entries()
{
    builder e;
    {
        builder b;
        type_head(b, h_point, 0xA111, 8, 4, fmt::struct_flag_flat, "t::Point", 2);
        field(b, "x", kind::i32, 0, 4, 4);
        field(b, "y", kind::i32, 4, 4, 4);
        entry(e, fmt::tag_struct, b);
    }
    {
        builder b;
        type_head(b, h_color, 0xA222, 1, 1, static_cast<std::uint8_t>(kind::u8), "t::Color", 2);
        b.put(std::int64_t{0});
        b.str16("red");
        b.put(std::int64_t{1});
        b.str16("green");
        entry(e, fmt::tag_enum, b);
    }
    {
        builder b;
        type_head(b, h_pixel, 0xA333, 16, 4, fmt::struct_flag_flat, "t::Pixel", 3);
        field(b, "<base:t::Point>", kind::struct_, 0, 8, 4, h_point);
        field(b, "c", kind::enum_, 8, 1, 1, h_color);
        field(b, "name", kind::array_, 9, 4, 1, 0, 4, 1);
        field(b, "", kind::char_, 0, 1, 1);
        entry(e, fmt::tag_struct, b);
    }
    {
        builder b;
        b.put(lit_hello);
        b.put(std::uint32_t{5});
        b.put(std::uint32_t{0});
        b.raw("hello");
        entry(e, fmt::tag_literal, b);
    }
    call_entry(e, h_call, {{false, h_pixel}, {true, lit_hello}});
    return e.out;
}

std::vector<std::byte> rri_header(const std::uint32_t n_entries, const std::uint64_t program_hash)
{
    builder r;
    r.put(fmt::rri_magic);
    r.put(fmt::format_major);
    r.put(fmt::format_minor);
    r.put(std::uint32_t{0});
    r.put(fmt::hash_algo_xxh3_64);
    r.put(n_entries);
    r.put(program_hash);
    r.put(std::uint32_t{0});
    r.put(fmt::host_abi_tag); // 1.3
    CHECK(r.out.size() == fmt::rri_header_size);
    return r.out;
}

std::vector<std::byte> file_header(const std::uint64_t rri_len,
                                   const std::uint64_t program_hash = 0x77)
{
    builder c;
    c.put(fmt::file_magic);
    c.put(fmt::format_major);
    c.put(fmt::format_minor);
    c.put(fmt::file_flag_le | fmt::file_flag_dict);
    c.put(fmt::hash_algo_xxh3_64);
    c.put(program_hash);
    c.put(std::uint64_t{fmt::file_header_size});
    c.put(rri_len);
    c.put(fmt::host_abi_tag); // 1.3
    CHECK(c.out.size() == fmt::file_header_size);
    return c.out;
}

std::vector<std::byte> make_capture(const std::vector<std::byte>& rri)
{
    builder c;
    c.bytes(file_header(rri.size()));
    c.bytes(rri);
    c.put(std::uint64_t{
        0}); // 1.7: the CHAIN after the (8-aligned) opening dictionary — next_dictionary,
    c.put(std::uint64_t{0}); // first_checkpoint; both 0 (never patched: nothing follows)
    const auto pixel = [&] {
        builder p;
        p.put(std::int32_t{3});
        p.put(std::int32_t{4});
        p.put(std::uint8_t{1});
        p.raw("AB");
        p.put(std::uint8_t{0});
        p.put(std::uint8_t{0});
        p.pad8();
        CHECK(p.out.size() == 16);
        return p.out;
    }();
    // compact frame (format 1.1): 16 header + 16 Pixel + 0 for the literal (FR-L6: named by the call)
    c.put(std::uint32_t{32});
    c.put(std::uint16_t{0});
    c.put(std::uint16_t{2});
    c.put(h_call);
    c.bytes(pixel);
    // full frame: 16 header + (16 + 16) + (16 + 8)
    c.put(std::uint32_t{72});
    c.put(fmt::frame_flag_full);
    c.put(std::uint16_t{2});
    c.put(h_call);
    c.put(h_pixel);
    c.put(std::uint32_t{16});
    c.put(std::uint16_t{0});
    c.put(std::uint16_t{0});
    c.bytes(pixel);
    c.put(lit_hello);
    c.put(std::uint32_t{8});
    c.put(std::uint16_t{0});
    c.put(std::uint16_t{0});
    c.put(lit_hello);
    return c.out;
}

// cc F2: a FILE whose dictionary declares array<u8, 0xFFFFFFFF> with a zero-size element, plus one 8-byte record.
std::vector<std::byte> make_bomb()
{
    builder e;
    {
        builder b;
        type_head(b, h_bomb, 0xA666, 8, 1, fmt::struct_flag_flat, "t::Bomb", 1);
        field(b, "a", kind::array_, 0, 8, 1, 0, 0xFFFFFFFFu, 1);
        field(b, "", kind::u8, 0, 0, 1);
        entry(e, fmt::tag_struct, b);
    }
    call_entry(e, h_bomb_call, {{false, h_bomb}});
    builder rri;
    rri.bytes(rri_header(2, 0));
    rri.bytes(e.out);
    builder c;
    c.bytes(file_header(rri.out.size(), 0));
    c.bytes(rri.out);
    c.put(std::uint32_t{24});
    c.put(std::uint16_t{0});
    c.put(std::uint16_t{1});
    c.put(h_bomb_call);
    c.put(std::uint64_t{0x4142434445464748ull});
    return c.out;
}

// cc F17: an 80-byte struct ENTRY whose single field announces n_elem = 65535 (previously a 6 MiB resize).
std::vector<std::byte> make_f17_entry()
{
    builder e;
    builder b;
    type_head(b, 0xF17, 0xAF17, 8, 8, fmt::struct_flag_flat, "t::A", 1);
    field(b, "a", kind::u8, 0, 1, 1, 0, 0, 65535);
    entry(e, fmt::tag_struct, b);
    CHECK(e.out.size() == 80);
    return e.out;
}

// gk 1.12: a type with u64 / i64 / 64-bit-enum members and one compact frame of it (bare stream).
struct big_fixture
{
    dyn::dictionary dict;
    std::vector<std::byte> frames;
};
big_fixture make_big()
{
    builder e;
    {
        builder b;
        type_head(b, h_big64, 0xA888, 8, 8, static_cast<std::uint8_t>(kind::i64), "t::Big64", 2);
        b.put(std::int64_t{1} << 60);
        b.str16("huge");
        b.put(std::int64_t{-1});
        b.str16("minus");
        entry(e, fmt::tag_enum, b);
    }
    {
        builder b;
        type_head(b, h_big, 0xA777, 24, 8, fmt::struct_flag_flat, "t::Big", 3);
        field(b, "a", kind::u64, 0, 8, 8);
        field(b, "b", kind::i64, 8, 8, 8);
        field(b, "e", kind::enum_, 16, 8, 8, h_big64);
        entry(e, fmt::tag_struct, b);
    }
    call_entry(e, h_big_call, {{false, h_big}});
    big_fixture f;
    CHECK(f.dict.add_entries(e.out).has_value());
    builder c;
    c.put(std::uint32_t{40});
    c.put(std::uint16_t{0});
    c.put(std::uint16_t{1});
    c.put(h_big_call);
    c.put(std::uint64_t{1} << 60);
    c.put(std::int64_t{-3});
    c.put(std::int64_t{1} << 60);
    f.frames = c.out;
    return f;
}

// A minimal ELF64 LSB relocatable image with .shstrtab and an es_serreg_dict section holding `dict`.
std::vector<std::byte> make_elf(const std::vector<std::byte>& dict)
{
    const std::string names = std::string("\0.shstrtab\0es_serreg_dict\0", 26);
    const std::uint64_t names_off = 64, dict_off = fmt::align8(names_off + names.size());
    const std::uint64_t shoff = fmt::align8(dict_off + dict.size());
    builder b;
    b.raw("\x7f"
          "ELF");
    b.put(std::uint8_t{2});
    b.put(std::uint8_t{1});
    b.put(std::uint8_t{1});
    b.put(std::uint8_t{0});
    b.put(std::uint64_t{0});
    b.put(std::uint16_t{1});
    b.put(std::uint16_t{0x3e});
    b.put(std::uint32_t{1});
    b.put(std::uint64_t{0});
    b.put(std::uint64_t{0});
    b.put(shoff);
    b.put(std::uint32_t{0});
    b.put(std::uint16_t{64});
    b.put(std::uint16_t{0});
    b.put(std::uint16_t{0});
    b.put(std::uint16_t{64});
    b.put(std::uint16_t{3});
    b.put(std::uint16_t{1});
    CHECK(b.out.size() == 64);
    b.raw(names);
    b.pad8();
    CHECK(b.out.size() == dict_off);
    b.bytes(dict);
    b.pad8();
    CHECK(b.out.size() == shoff);
    const auto shdr = [&](const std::uint32_t name, const std::uint32_t type,
                          const std::uint64_t off, const std::uint64_t size) {
        b.put(name);
        b.put(type);
        b.put(std::uint64_t{0});
        b.put(std::uint64_t{0});
        b.put(off);
        b.put(size);
        b.put(std::uint32_t{0});
        b.put(std::uint32_t{0});
        b.put(std::uint64_t{1});
        b.put(std::uint64_t{0});
    };
    shdr(0, 0, 0, 0);
    shdr(1, 3, names_off, names.size());
    shdr(11, 1, dict_off, dict.size());
    return b.out;
}

// 2.1.12 (cc review 2.1.10, D-04): an ELF whose string table holds one long name without its end near the start
// and `fillers` section headers pointing at it, then the real es_serreg_dict. Scanning every name to its NUL made
// the lookup quadratic (fillers × name length).
std::vector<std::byte> make_wide_elf(const std::vector<std::byte>& dict,
                                     const std::size_t long_name, const std::uint32_t fillers)
{
    std::string names("\0", 1);
    names += std::string(long_name, 'a');
    names.push_back('\0');
    const std::uint32_t shstr_name = static_cast<std::uint32_t>(names.size());
    names += std::string(".shstrtab\0", 10);
    const std::uint32_t dict_name = static_cast<std::uint32_t>(names.size());
    names += std::string("es_serreg_dict\0", 15);
    const std::uint64_t names_off = 64, dict_off = fmt::align8(names_off + names.size());
    const std::uint64_t shoff = fmt::align8(dict_off + dict.size());
    const std::uint32_t n_sections = 3 + fillers;
    builder b;
    b.raw("\x7f"
          "ELF");
    b.put(std::uint8_t{2});
    b.put(std::uint8_t{1});
    b.put(std::uint8_t{1});
    b.put(std::uint8_t{0});
    b.put(std::uint64_t{0});
    b.put(std::uint16_t{1});
    b.put(std::uint16_t{0x3e});
    b.put(std::uint32_t{1});
    b.put(std::uint64_t{0});
    b.put(std::uint64_t{0});
    b.put(shoff);
    b.put(std::uint32_t{0});
    b.put(std::uint16_t{64});
    b.put(std::uint16_t{0});
    b.put(std::uint16_t{0});
    b.put(std::uint16_t{64});
    b.put(static_cast<std::uint16_t>(
        0)); // e_shnum 0: the count is in section 0's size (extended numbering)
    b.put(std::uint16_t{1});
    b.raw(names);
    b.pad8();
    b.bytes(dict);
    b.pad8();
    const auto shdr = [&](const std::uint32_t name, const std::uint32_t type,
                          const std::uint64_t off, const std::uint64_t size) {
        b.put(name);
        b.put(type);
        b.put(std::uint64_t{0});
        b.put(std::uint64_t{0});
        b.put(off);
        b.put(size);
        b.put(std::uint32_t{0});
        b.put(std::uint32_t{0});
        b.put(std::uint64_t{1});
        b.put(std::uint64_t{0});
    };
    shdr(0, 0, 0, n_sections);
    shdr(shstr_name, 3, names_off, names.size());
    for (std::uint32_t i = 0; i < fillers; ++i)
        shdr(1, 1, 0, 0); // named by the long string
    shdr(dict_name, 1, dict_off, dict.size());
    return b.out;
}

bool contains(const std::string& s, const std::string_view needle)
{
    return s.find(needle) != std::string::npos;
}

std::string source_dir()
{
    const std::string f = __FILE__;
    return f.substr(0, f.rfind('/'));
}

bool file_exists(const std::string& p)
{
    struct stat st = {};
    return ::stat(p.c_str(), &st) == 0;
}

// Runs cli::run with stdout captured into `out` (the CLI writes with fwrite/fflush on stdout).
int run_cli(const std::vector<std::string>& args, std::string& out, const std::string& tmp)
{
    std::vector<std::string> storage = args;
    storage.insert(storage.begin(), "srdump");
    std::vector<char*> argv;
    for (auto& s : storage)
        argv.push_back(s.data());
    std::fflush(stdout);
    const int saved = ::dup(STDOUT_FILENO);
    CHECK(saved >= 0);
    std::FILE* const f = std::fopen(tmp.c_str(), "w+");
    CHECK(f != nullptr);
    CHECK(::dup2(::fileno(f), STDOUT_FILENO) == STDOUT_FILENO);
    const int rc = cli::run(static_cast<int>(argv.size()), argv.data());
    std::fflush(stdout);
    CHECK(::dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(saved);
    std::fclose(f);
    const auto bytes = tools::read_file(tmp);
    CHECK(bytes.has_value());
    out.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    return rc;
}

// cli::run with standard output sent to `path` (e.g. /dev/full); returns the exit code.
int run_cli_into(const std::vector<std::string>& args, const char* const path)
{
    std::vector<std::string> storage = args;
    storage.insert(storage.begin(), "srdump");
    std::vector<char*> argv;
    for (auto& s : storage)
        argv.push_back(s.data());
    std::fflush(stdout);
    const int saved = ::dup(STDOUT_FILENO);
    CHECK(saved >= 0);
    const int fd = ::open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(::dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(fd);
    const int rc = cli::run(static_cast<int>(argv.size()), argv.data());
    std::fflush(stdout);
    std::clearerr(stdout);
    CHECK(::dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    ::close(saved);
    return rc;
}

void write_fixture(const std::string& path, const std::vector<std::byte>& bytes)
{
    CHECK(tools::write_file(path, bytes).has_value());
    std::println("wrote {} ({} bytes)", path, bytes.size());
}

std::vector<std::byte> to_bytes(const std::string_view s)
{
    std::vector<std::byte> v;
    for (const char c : s)
        v.push_back(static_cast<std::byte>(c));
    return v;
}

} // namespace

namespace runtime_selftest
{
// runtime_selftest [out.ser [fixtures-dir]]
struct options
{
    const char* out_ser = nullptr;      // also write the capture (and out.ser.rri) here
    const char* fixtures_dir = nullptr; // regenerate the seed corpora and negative fixtures here

    static options parse(const int argc, char** const argv)
    {
        options o;
        if (argc > 1)
            o.out_ser = argv[1];
        if (argc > 2)
            o.fixtures_dir = argv[2];
        return o;
    }
};

// The checks run in order, each section building on the fixtures of the earlier ones; CHECK exits on failure.
class app
{
public:
    explicit app(const options& opts)
        : opts_{opts},
          tmp_out_{(opts.out_ser ? std::string(opts.out_ser) : std::string("runtime_selftest")) +
                   ".cli.txt"}
    {
    }

    int run()
    {
        dictionary_parsing();
        rri_round_trip();
        parser_bounds();
        dictionary_conflicts();
        capture_verify();
        dump_text_json();
        json_64bit_integers();
        diff_reports();
        elf_reader();
        write_all_errors();
        cli_surface();
        review_2_1_10_decoders();
        negative_fixtures();
        generate_fixtures();
        std::println("runtime_selftest: all checks passed");
        return 0;
    }

private:
    options opts_;
    std::string tmp_out_;
    // built by the sections below and used by the later ones
    std::vector<std::byte> entries_;
    dyn::dictionary d_;
    std::vector<std::byte> rri_;
    std::vector<std::byte> bomb_;
    std::vector<std::byte> f17_;
    dyn::dictionary conflicting_;
    std::vector<std::byte> capture_;
    std::span<const std::byte> frames_;
    std::vector<std::byte> lit_full_;
    dyn::dictionary nested_bad_;
    std::vector<std::byte> capture_decode_err_;
    std::string text_;
    std::string filtered_;
    big_fixture big_;
    std::vector<std::byte> elf_;

    // ---- dictionary parsing and lookups (FR-W3) ----
    void dictionary_parsing()
    {
        entries_ = make_entries();
        CHECK(d_.add_entries(entries_).has_value());
        CHECK(d_.types.size() == 3 && d_.literals.size() == 1 && d_.calls.size() == 1 &&
              d_.conflicts.empty());
        const dyn::type* const pixel = d_.find_type(h_pixel);
        CHECK(pixel && pixel->name == "t::Pixel" && pixel->fields.size() == 3 && pixel->is_flat &&
              pixel->size == 16);
        CHECK(pixel->fields[2].k == kind::array_ && pixel->fields[2].elem.size() == 1 &&
              pixel->fields[2].elem[0].k == kind::char_);
        CHECK(d_.find_type_by_name("t::Color") && d_.find_type_by_name("t::Color")->is_enum &&
              d_.find_type_by_name("t::Color")->enumerators.size() == 2);
        CHECK(d_.find_type_by_name("nope") == nullptr);
        CHECK(d_.find_literal(lit_hello) && d_.find_literal(lit_hello)->text == "hello");
        const dyn::call* const call = d_.find_call(h_call);
        CHECK(call && call->args.size() == 2 && !call->args[0].is_literal &&
              call->args[1].is_literal);
        CHECK(d_.record_size(call->args[0]).value() == 16 &&
              d_.record_size(call->args[1]).value() ==
                  0); // 1.1 FR-L6: a compact literal has no bytes
        CHECK(d_.record_size(dyn::call_arg{false, 0xdead}).error() == error::unknown_type);

        // Garbage in must not crash (FR-D5): truncated stream and a bogus length.
        {
            dyn::dictionary g;
            CHECK(g.add_entries(std::span(entries_).first(entries_.size() - 3)).error() ==
                  error::corrupt_dictionary);
        }
        {
            std::vector<std::byte> bad(entries_);
            bad[0] = std::byte{0xff};
            bad[1] = std::byte{0xff};
            dyn::dictionary g;
            CHECK(!g.add_entries(bad).has_value());
        }
        {
            std::vector<std::byte> bad(entries_);
            bad[fmt::entry_header_size + 32] = std::byte{0xff}; /* name_len of t::Point */
            dyn::dictionary g;
            CHECK(!g.add_entries(bad).has_value());
        }
    }

    // ---- RRI round trip (FR-W5 determinism, §6.7) ----
    void rri_round_trip()
    {
        d_.program_hash = 0x77;
        rri_ = dyn::to_rri(d_);
        CHECK(rri_.size() % 8 == 0);
        const auto d2 = tools::load_dictionary_rri(rri_);
        CHECK(d2.has_value());
        CHECK(d2->to_json() == d_.to_json());
        CHECK(dyn::to_rri(*d2) == rri_);
        CHECK(d2->to_toml() == d_.to_toml());
        CHECK(contains(d_.to_json(), "\"name\": \"<base:t::Point>\""));
        CHECK(contains(d_.to_toml(), "[[types.fields.elem]]"));
        {
            std::vector<std::byte> bad(rri_);
            bad[0] = std::byte{'X'};
            const auto e = tools::load_dictionary_rri(bad);
            CHECK(!e.has_value() && e.error() == error::bad_magic);
        }
    }

    // ---- parser bounds (cc F2 / F17, gk 1.15) ----
    void parser_bounds()
    {
        bomb_ = make_bomb();
        f17_ = make_f17_entry();
        {
            dyn::dictionary g;
            CHECK(g.add_entries(f17_).error() == error::corrupt_dictionary && g.types.empty());
        }
        {
            const auto r = tools::verify(bomb_);
            CHECK(!r.has_value() &&
                  r.error() == error::corrupt_dictionary); // rejected before any record walk
            std::string out;
            CHECK(tools::dump(bomb_, nullptr, {}, out).error() == error::corrupt_dictionary);
            tools::dump_options o;
            o.dict_only = true;
            CHECK(!tools::dump(bomb_, nullptr, o, out).has_value());
        }
        {
            dyn::dictionary g = d_;
            g.types[h_pixel].fields[2].extent = 5; // extent × elem.size (5) > field size (4)
            CHECK(tools::load_dictionary_rri(dyn::to_rri(g)).error() == error::corrupt_dictionary);
        }
        {
            dyn::dictionary g = d_;
            g.types[h_pixel].fields[1].offset = 16; // offset + size (17) > type size (16)
            CHECK(tools::load_dictionary_rri(dyn::to_rri(g)).error() == error::corrupt_dictionary);
        }
        {
            dyn::dictionary g = d_;
            g.types[h_pixel].fields[2].elem[0].size = 0; // zero-size element with extent 4
            CHECK(tools::load_dictionary_rri(dyn::to_rri(g)).error() == error::corrupt_dictionary);
        }
        {
            dyn::dictionary g = d_;
            g.types[h_pixel].fields[2].elem.clear(); // array without an element descriptor
            CHECK(tools::load_dictionary_rri(dyn::to_rri(g)).error() == error::corrupt_dictionary);
        }
        {
            builder e;
            builder b;
            type_head(b, 0xE1, 0xAE1, 1, 1, static_cast<std::uint8_t>(kind::u8), "t::E", 65535);
            entry(e, fmt::tag_enum, b);
            dyn::dictionary g;
            CHECK(g.add_entries(e.out).error() == error::corrupt_dictionary);
        } // enumerator count vs bytes
        {
            builder e;
            builder b;
            b.put(std::uint64_t{0xC1});
            b.put(std::uint16_t{65535});
            b.put(std::uint16_t{0});
            b.put(std::uint32_t{0});
            entry(e, fmt::tag_call, b);
            dyn::dictionary g;
            CHECK(g.add_entries(e.out).error() == error::corrupt_dictionary);
        } // arg count vs bytes
        {
            std::vector<std::byte> bad = rri_header(0xFFFFFFFFu, 0);
            dyn::dictionary g;
            CHECK(g.add_rri(bad).error() == error::corrupt_dictionary);
        }
        { // decoder guard, bypassing the parser: zero-size element with a non-zero extent never iterates
            dyn::type t;
            t.name = "t::Z";
            t.size = 8;
            t.align = 1;
            t.is_flat = true;
            dyn::field a;
            a.name = "a";
            a.k = kind::array_;
            a.size = 8;
            a.align = 1;
            a.extent = 5;
            a.elem.emplace_back();
            a.elem[0].k = kind::u8;
            t.fields.push_back(a);
            struct null_visitor final : tools::decoded_value_visitor
            {
                int arrays{}, scalars{};
                void begin_struct(std::string_view, std::string_view) override
                {
                }
                void end_struct() override
                {
                }
                void begin_array(std::string_view, std::uint32_t) override
                {
                    ++arrays;
                }
                void end_array() override
                {
                }
                void scalar(std::string_view, kind, std::span<const std::byte>) override
                {
                    ++scalars;
                }
                void enumerator(std::string_view, std::string_view, std::string_view,
                                std::int64_t) override
                {
                }
                void literal(std::string_view, std::uint64_t, std::string_view) override
                {
                }
            } v;
            const std::array<std::byte, 8> payload{};
            CHECK(tools::visit_record(payload, t, d_, v).error() == error::length_mismatch &&
                  v.scalars == 0);
            t.fields[0].extent = 0;
            CHECK(tools::visit_record(payload, t, d_, v).has_value() && v.arrays == 1 &&
                  v.scalars == 0);
            t.fields[0].extent = 9;
            t.fields[0].elem[0].size = 1; // 9 × 1 > 8 bytes
            CHECK(tools::visit_record(payload, t, d_, v).error() == error::length_mismatch);
        }
    }

    // ---- cc review 2.1.10 (2.1.12): the decoder findings D-01..D-04 and D-08 ----
    void review_2_1_10_decoders()
    {
        struct counting_visitor final : tools::decoded_value_visitor
        {
            std::uint64_t events{};
            void begin_struct(std::string_view, std::string_view) override
            {
                ++events;
            }
            void end_struct() override
            {
            }
            void begin_array(std::string_view, std::uint32_t) override
            {
                ++events;
            }
            void end_array() override
            {
            }
            void scalar(std::string_view, kind, std::span<const std::byte> b) override
            {
                CHECK(b.size() <= 8); // never a forged 4 GiB scalar
                ++events;
            }
            void enumerator(std::string_view, std::string_view, std::string_view,
                            std::int64_t) override
            {
            }
            void literal(std::string_view, std::uint64_t, std::string_view) override
            {
            }
        };
        { // D-01: n × stride wraps 64 bits. take() accepts the wrapped product, take_n() refuses the block.
            constexpr std::uint64_t stride = fmt::slot_stride(0xFFFFFFFFu, 0xFFFEu);
            constexpr std::uint32_t n = 4294901767u;
            const std::uint64_t wrapped = std::uint64_t{n} * stride; // 851926
            std::vector<std::byte> image(8 + wrapped + 8);
            fmt::image_budget b1(image, 8), b2(image, 8);
            CHECK(stride == 0x10000FFFAull && wrapped < image.size());
            CHECK(b1.take({8, n}, wrapped));   // what 2.1.11 charged
            CHECK(!b2.take_n({8, n}, stride)); // 2.1.12
            // and through the walker, with a descriptor that bypasses the parser
            dyn::type t;
            t.name = "t::Wrap";
            t.size = 8;
            t.align = 4;
            dyn::field v;
            v.name = "v";
            v.k = kind::vector_;
            v.size = 8;
            v.align = 4;
            dyn::field e;
            e.k = kind::u8;
            e.size = 0xFFFFFFFFu;
            e.align = 0xFFFE;
            v.elem.push_back(e);
            t.fields.push_back(v);
            fmt::writer w{image.data()};
            w.put(std::uint32_t{8});
            w.put(n);
            counting_visitor cv;
            CHECK(tools::visit_record(image, t, d_, cv).error() == error::length_mismatch);
            // the parser refuses such a FIELD in the first place: a u8 of 0xFFFFFFFF bytes, an alignment of 0xFFFE
            builder en, body;
            type_head(body, 0xD01, 0xAD01, 8, 4, 0, "t::Wrap", 1);
            field(body, "v", kind::vector_, 0, 8, 4, 0, 0, 1);
            field(body, "", kind::u8, 0, 0xFFFFFFFFu, 0xFFFE);
            entry(en, fmt::tag_struct, body);
            dyn::dictionary g;
            CHECK(g.add_entries(en.out).error() == error::corrupt_dictionary);
            builder en2, body2;
            type_head(body2, 0xD02, 0xAD02, 8, 8, fmt::struct_flag_flat, "t::Align", 1);
            field(body2, "x", kind::u64, 0, 8, 12); // alignment 12 is not a power of two
            entry(en2, fmt::tag_struct, body2);
            dyn::dictionary g2;
            CHECK(g2.add_entries(en2.out).error() == error::corrupt_dictionary);
        }
        { // D-02: fan-out — 30 levels of 1-byte types whose 8 struct fields all sit at offset 0 would be 8^30
            // visits; the per-record step budget stops the walk
            dyn::dictionary d;
            std::uint64_t prev = 0xF000;
            dyn::type leaf;
            leaf.name = "t::F0";
            leaf.schema_hash = prev;
            leaf.size = 1;
            leaf.align = 1;
            leaf.is_flat = true;
            dyn::field u;
            u.name = "u";
            u.k = kind::u8;
            u.size = 1;
            u.align = 1;
            leaf.fields.push_back(u);
            d.types[prev] = leaf;
            for (int level = 1; level <= 30; ++level)
            {
                dyn::type t;
                t.name = "t::F" + std::to_string(level);
                t.schema_hash = 0xF000 + static_cast<std::uint64_t>(level);
                t.size = 1;
                t.align = 1;
                t.is_flat = true;
                for (int k = 0; k < 8; ++k)
                {
                    dyn::field f;
                    f.name = "f" + std::to_string(k);
                    f.k = kind::struct_;
                    f.size = 1;
                    f.align = 1;
                    f.type_hash = prev;
                    t.fields.push_back(f);
                }
                d.types[t.schema_hash] = t;
                prev = t.schema_hash;
            }
            const std::array<std::byte, 1> payload{};
            counting_visitor cv;
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(tools::visit_record(payload, *d.find_type(prev), d, cv).error() ==
                  error::length_mismatch);
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
            CHECK(cv.events < 200000);
        }
        { // D-03: 200 conflicting redefinitions keep 64 summaries (no fields) and count the rest
            dyn::dictionary m;
            for (std::uint32_t i = 0; i < 201; ++i)
            {
                builder en, body;
                type_head(body, 0xD03, 0xA000 + i, 8, 8, fmt::struct_flag_flat, "t::Redefined", 1);
                field(body, "x", kind::u64, 0, 8, 8);
                entry(en, fmt::tag_struct, body);
                CHECK(m.add_entries(en.out).has_value());
            }
            CHECK(m.conflicts.size() == dyn::dictionary::max_conflicts &&
                  m.conflicts_omitted == 200 - dyn::dictionary::max_conflicts);
            CHECK(m.conflicts[0].kept.name == "t::Redefined" &&
                  m.conflicts[0].kept.fields.empty() &&
                  m.conflicts[0].dropped.layout_hash == 0xA001);
            CHECK(m.find_type(0xD03)->fields.size() == 1); // the kept definition itself is whole
        }
        { // D-04: 20000 section headers named by a 1 MiB string: the lookup stays linear (2.1.11: 2 × 20 G compares)
            const auto wide = make_wide_elf(entries_, 1024 * 1024, 20000);
            const auto t0 = std::chrono::steady_clock::now();
            const auto sec = tools::extract_section_from_image(wide, "es_serreg_dict");
            CHECK(sec.has_value() && *sec == entries_);
            CHECK(tools::extract_section_from_image(wide, "nope").error() == error::not_found);
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
        }
        { // T-13: members whose names map to one filter segment get their own fields
            dyn::dictionary d;
            dyn::type t;
            t.name = "t::Y";
            t.schema_hash = 0x7013;
            t.size = 2;
            t.align = 1;
            t.is_flat = true;
            for (const char* const n : {"y", "y_"})
            {
                dyn::field f;
                f.name = n;
                f.k = kind::u8;
                f.offset = static_cast<std::uint32_t>(t.fields.size());
                f.size = 1;
                f.align = 1;
                t.fields.push_back(f);
            }
            d.types[t.schema_hash] = t;
            const std::string lua = tools::wireshark_dissector(d);
            CHECK(contains(lua, "\"serreg.t_Y.y\"") && contains(lua, "\"serreg.t_Y.y_1\""));
        }
        { // D-08: JSON is valid UTF-8 — well-formed sequences pass, stray bytes become \u00XX
            dyn::dictionary d;
            d.literals[1] = dyn::literal{1, "caf\xc3\xa9 \xff\xfe d\xc3\x28"};
            const std::string j = d.to_json();
            CHECK(contains(j, "caf\xc3\xa9 \\u00ff\\u00fe d\\u00c3("));
        }
    }

    // ---- dictionary conflicts (cc F15/F16, gk 1.10) ----
    void dictionary_conflicts()
    {
        conflicting_ = d_;
        conflicting_.types[h_pixel].layout_hash = 0xB333;
        {
            dyn::dictionary m;
            CHECK(m.add_rri(rri_).has_value() && m.add_rri(rri_).has_value() &&
                  m.conflicts.empty()); // same body twice: no conflict
            CHECK(m.add_rri(dyn::to_rri(conflicting_)).has_value());
            CHECK(m.conflicts.size() == 1 && m.conflicts[0].kept.layout_hash == 0xA333 &&
                  m.conflicts[0].dropped.layout_hash == 0xB333);
            CHECK(m.find_type(h_pixel)->layout_hash == 0xA333 &&
                  m.types.size() == 3); // first definition wins
            dyn::dictionary via_merge = d_;
            via_merge.merge(conflicting_);
            CHECK(via_merge.conflicts.size() == 1 &&
                  via_merge.find_type(h_pixel)->layout_hash == 0xA333);
        }
    }

    // ---- capture: verify (FR-W2 both modes; the decoder runs on every record, cc F2 / gk 1.4) ----
    void capture_verify()
    {
        capture_ = make_capture(rri_);
        if (opts_.out_ser)
        { // optional: write the capture and its RRI for manual srdump runs
            CHECK(tools::write_file(opts_.out_ser, capture_).has_value());
            CHECK(tools::write_file(std::string(opts_.out_ser) + ".rri", rri_).has_value());
            CHECK(tools::read_file(opts_.out_ser).value() == capture_);
        }
        const auto rep = tools::verify(capture_);
        CHECK(rep.has_value());
        for (const auto& p : rep->problems)
            std::println(stderr, "verify problem: {}", p);
        CHECK(rep->ok() && rep->frames == 2 && rep->records == 4 && rep->bytes == 104);
        const std::size_t frames_off = fmt::file_header_size + rri_.size() + fmt::chain_size;
        {
            std::vector<std::byte> bad(capture_);
            bad[frames_off] = std::byte{41}; // frame_len not a multiple of 8
            const auto r = tools::verify(bad);
            CHECK(r.has_value() && !r->ok());
        }
        {
            std::vector<std::byte> bad(capture_);
            std::memset(bad.data() + frames_off + 8, 0, 8); // unknown call hash
            const auto r = tools::verify(bad);
            CHECK(r.has_value() && r->problems.size() == 1 &&
                  contains(r->problems[0], "unknown call"));
        }
        // Bare frame stream with the dictionary supplied out of band.
        frames_ = std::span<const std::byte>(capture_).subspan(frames_off);
        {
            const auto r = tools::verify(frames_, &d_);
            CHECK(r.has_value() && r->ok() && r->frames == 2);
            const auto no_dict = tools::verify(frames_);
            CHECK(no_dict.has_value() && !no_dict->ok());
        }
        // Conflicting extra dictionary → a problem.
        {
            const auto r = tools::verify(capture_, &conflicting_);
            CHECK(r.has_value() && r->problems.size() == 1 &&
                  contains(r->problems[0], "schema conflict: t::Pixel 0x0000000000003333"));
        }
        // gk 1.4: a compact literal has no bytes (its call hash is the only place it lives), so the literal id check
        // applies to full records — the header slot and the payload id.
        lit_full_ = capture_;
        lit_full_[frames_off + 32 + 48] = std::byte{0x99};
        {
            const auto r = tools::verify(lit_full_);
            CHECK(r.has_value() && r->problems.size() == 1 &&
                  contains(r->problems[0], "frame 1 @") &&
                  contains(r->problems[0], "literal id mismatch"));
        }
        // Nested type larger than the field that holds it: only the decoder can see it, and verify must run it.
        nested_bad_ = d_;
        nested_bad_.types[h_point].size = 16;
        capture_decode_err_ = make_capture(dyn::to_rri(nested_bad_));
        {
            const auto r = tools::verify(capture_decode_err_);
            CHECK(r.has_value() && !r->ok() && r->frames == 2 && r->records == 4 &&
                  r->problems.size() == 2 &&
                  contains(r->problems[0], "decode error: length_mismatch"));
        }
        // "ESSR" as a frame length (gk §2): a small file with the magic but another major stays a FILE (bad_version).
        {
            std::vector<std::byte> v2(capture_);
            v2[4] = std::byte{2};
            CHECK(tools::verify(v2).error() == error::bad_version);
        }
    }

    // ---- dump text / json (FR-D1, FR-L3) ----
    void dump_text_json()
    {
        tools::dump_options opt;
        CHECK(tools::dump(capture_, nullptr, opt, text_).has_value());
        std::println("{}", text_);
        CHECK(contains(text_, "t::Pixel {") && contains(text_, "x: 3") &&
              contains(text_, "c: t::Color::green (1)") && contains(text_, "name: \"AB\\0\\0\"") &&
              contains(text_, "[1] \"hello\"\nframe 1") &&
              contains(text_, "frame 1  call 0x0000000000005555  (2 records, full)"));
        std::string flat;
        opt.flatten = true;
        opt.hex = true;
        CHECK(tools::dump(capture_, nullptr, opt, flat).has_value());
        CHECK(contains(flat, "        x: 3\n") && !contains(flat, "<base:t::Point>: t::Point {") &&
              contains(flat, "hex 0: 03 00 00 00"));
        std::string json;
        opt = {};
        opt.fmt = tools::format::json;
        CHECK(tools::dump(capture_, nullptr, opt, json).has_value());
        std::println("{}", json);
        CHECK(contains(json, "\"x\":3") && contains(json, "\"y\":4") &&
              contains(json, "\"c\":{\"name\":\"green\",\"value\":1}") &&
              contains(json, "\"name\":\"AB\\u0000\\u0000\"") &&
              contains(json, "{\"lit\":\"hello\",\"id\":\"0x0000000000004444\"}") &&
              contains(json, "{\"frame\":1,\"record\":0,\"type\":\"t::Pixel\""));
        opt = {};
        opt.type_name_filter = "t::Pixel";
        opt.first_frame = 1;
        opt.last_frame = 1;
        CHECK(tools::dump(capture_, nullptr, opt, filtered_).has_value());
        CHECK(!contains(filtered_, "frame 0 ") && contains(filtered_, "frame 1 ") &&
              !contains(filtered_, "[1] \"hello\""));
        std::string dict_only;
        opt = {};
        opt.dict_only = true;
        opt.fmt = tools::format::toml;
        CHECK(tools::dump(capture_, nullptr, opt, dict_only).has_value() &&
              dict_only == d_.to_toml());
        {
            std::string unknown_lit;
            dyn::dictionary no_lit = d_;
            no_lit.literals.clear(); // id not in the table → lit#…
            CHECK(tools::dump(frames_, &no_lit, {}, unknown_lit).has_value() &&
                  contains(unknown_lit, "lit#0000000000004444"));
        }
        // A literal id mismatch (full record, 1.1) is a problem: printed and returned (exit 1 in srdump).
        {
            std::string out;
            const auto r = tools::dump(lit_full_, nullptr, {}, out);
            CHECK(!r.has_value() && r.error() == error::hash_mismatch &&
                  contains(out, "problem: frame 1 @") && contains(out, "literal id mismatch"));
        }
        // Walker error → problem + error, and the walk continues to the next frame (both records fail here).
        {
            std::string out;
            const auto r = tools::dump(capture_decode_err_, nullptr, {}, out);
            CHECK(!r.has_value() && r.error() == error::length_mismatch &&
                  contains(out, "<decode error: length_mismatch>") &&
                  contains(out, "frame 1  call") && contains(out, "problem: frame 1 @"));
            std::string js;
            tools::dump_options jo;
            jo.fmt = tools::format::json;
            CHECK(!tools::dump(capture_decode_err_, nullptr, jo, js).has_value() &&
                  contains(js, "\"error\":\"length_mismatch\"") &&
                  contains(js, "{\"problem\":\"frame 0 @"));
            // cc F18: --dict-only never decodes a record, so the same capture dumps cleanly.
            std::string dict;
            tools::dump_options o;
            o.dict_only = true;
            std::string expect;
            CHECK(tools::dump_dictionary(nested_bad_, tools::format::text, expect).has_value());
            CHECK(tools::dump(capture_decode_err_, nullptr, o, dict).has_value() && dict == expect);
        }
        // cc F18: --frames A..B stops after B (frame 1 carries the flipped literal id).
        {
            std::string out;
            tools::dump_options o;
            o.last_frame = 0;
            CHECK(tools::dump(lit_full_, nullptr, o, out).has_value() &&
                  contains(out, "frame 0 ") && !contains(out, "frame 1 "));
            o.last_frame = 1;
            CHECK(!tools::dump(lit_full_, nullptr, o, out).has_value());
        }
        // Conflicts show up in the dictionary text and make dump fail.
        {
            std::string out;
            const auto r = tools::dump(capture_, &conflicting_, {}, out);
            CHECK(
                !r.has_value() && r.error() == error::schema_conflict &&
                contains(out,
                         "conflict 0x0000000000003333  kept t::Pixel (layout 0x000000000000a333") &&
                contains(out, "problem: schema conflict"));
            tools::dump_options o;
            o.dict_only = true;
            CHECK(tools::dump(capture_, &conflicting_, o, out).error() == error::schema_conflict);
        }
        // cc F19: the streaming overload produces the same bytes as the string overload, one chunk per frame.
        {
            struct sink
            {
                std::string s;
                int chunks{};
            } sk;
            const auto w = [](const std::string_view chunk, void* const ctx) {
                auto* const s = static_cast<sink*>(ctx);
                s->s.append(chunk);
                ++s->chunks;
                return true;
            };
            CHECK(tools::dump(capture_, nullptr, {}, +w, &sk).has_value() && sk.s == text_ &&
                  sk.chunks >= 4);
            const auto refuse = [](std::string_view, void*) { return false; };
            CHECK(tools::dump(capture_, nullptr, {}, +refuse, nullptr).error() == error::io_error);
        }
    }

    // ---- gk 1.12: 64-bit integers are quoted decimal strings in JSON, plain in text ----
    void json_64bit_integers()
    {
        big_ = make_big();
        {
            const auto r = tools::verify(big_.frames, &big_.dict);
            CHECK(r.has_value() && r->ok() && r->records == 1);
            std::string js;
            tools::dump_options jo;
            jo.fmt = tools::format::json;
            CHECK(tools::dump(big_.frames, &big_.dict, jo, js).has_value());
            CHECK(contains(js, "\"a\":\"1152921504606846976\"") && contains(js, "\"b\":\"-3\"") &&
                  contains(js, "\"e\":{\"name\":\"huge\",\"value\":\"1152921504606846976\"}"));
            std::string tx;
            CHECK(tools::dump(big_.frames, &big_.dict, {}, tx).has_value());
            CHECK(contains(tx, "a: 1152921504606846976") && contains(tx, "b: -3") &&
                  contains(tx, "e: t::Big64::huge (1152921504606846976)"));
        }
    }

    // ---- diff (FR-D3, gk 1.11) ----
    void diff_reports()
    {
        dyn::dictionary m = d_;
        m.types[h_pixel].fields[1].since = 3;
        m.types[h_pixel].schema_hash = 0x3334; // metadata-only
        m.types[h_point].schema_hash = 0x1112; // names-only
        m.types[h_color].layout_hash = 0xB222;
        m.types[h_color].schema_hash = 0x2223; // breaking
        {
            dyn::type n;
            n.name = "t::New";
            n.schema_hash = n.layout_hash = 0x9999;
            n.size = n.align = 1;
            n.is_flat = true;
            m.types[0x9999] = n;
        }
        const tools::diff_report dr = tools::diff(d_, m);
        std::println("{}", dr.to_text());
        CHECK(dr.breaking && dr.entries.size() == 4);
        for (const auto& e : dr.entries)
        {
            if (e.name == "t::Pixel")
                CHECK(e.verdict == "metadata-only");
            else if (e.name == "t::Point")
                CHECK(e.verdict == "names-only");
            else if (e.name == "t::Color")
                CHECK(e.verdict == "breaking");
            else
                CHECK(e.name == "t::New" && e.verdict == "added");
        }
        CHECK(!tools::diff(d_, d_).breaking &&
              tools::diff(d_, d_).entries[0].verdict == "identical");
        for (const auto& e : tools::diff(m, d_).entries)
            if (e.name == "t::New")
                CHECK(e.verdict == "removed" && e.schema_b == 0);
        const auto verdict_of = [](const tools::diff_report& r, const std::string_view name) {
            for (const auto& e : r.entries)
                if (e.name == name)
                    return e.verdict;
            return std::string("<none>");
        };
        {
            dyn::dictionary e2 = d_;
            e2.types[h_color].schema_hash = 0x2224;
            e2.types[h_color].enumerators[1].second = 2; // green: 1 → 2
            const auto r = tools::diff(d_, e2);
            CHECK(r.breaking && verdict_of(r, "t::Color") == "breaking");
        }
        {
            dyn::dictionary e2 = d_;
            e2.types[h_color].schema_hash = 0x2224;
            e2.types[h_color].enumerators.pop_back(); // green removed
            const auto r = tools::diff(d_, e2);
            CHECK(r.breaking && verdict_of(r, "t::Color") == "breaking");
        }
        {
            dyn::dictionary e2 = d_;
            e2.types[h_color].schema_hash = 0x2224;
            e2.types[h_color].enumerators[1].first = "lime"; // renamed, value kept
            const auto r = tools::diff(d_, e2);
            CHECK(!r.breaking && verdict_of(r, "t::Color") == "names-only");
        }
        {
            dyn::dictionary e2 = d_;
            e2.types[h_color].schema_hash = 0x2224;
            e2.types[h_color].enumerators.emplace_back("blue", 2); // added
            const auto r = tools::diff(d_, e2);
            CHECK(!r.breaking && verdict_of(r, "t::Color") == "metadata-only");
        }
        {
            dyn::dictionary dup = d_;
            dyn::type p2 = d_.types[h_point];
            p2.schema_hash = 0x1113;
            dup.types[0x1113] = p2; // duplicate canonical name
            const auto r = tools::diff(d_, dup);
            std::size_t dups = 0, points = 0;
            for (const auto& e : r.entries)
            {
                if (e.verdict == "duplicate-name")
                {
                    ++dups;
                    CHECK(e.name == "t::Point" && e.schema_b == 0x1113);
                }
                if (e.name == "t::Point" && e.verdict == "identical")
                    ++points;
            }
            CHECK(dups == 1 && points == 1 && !r.breaking &&
                  contains(r.to_text(), "1 duplicate-name"));
        }
        { // cc review 2.1.10, T-09 (2.1.12): versions of one name pair up by hash first
            dyn::type p2 = d_.types[h_point];
            p2.schema_hash = 0x1113;
            p2.layout_hash = 0xA112;
            dyn::type p3 = p2;
            p3.schema_hash = 0x1114;
            p3.layout_hash = 0xA113;
            dyn::dictionary two = d_, other = d_;
            two.types[0x1113] = p2; // a: {0x1111, 0x1113}
            other.types.erase(h_point);
            other.types[0x1113] = p2; // b: {0x1113} — the same version, still there
            const auto r1 = tools::diff(two, other);
            CHECK(!r1.breaking); // 2.1.11 compared 0x1111 with 0x1113 and said BREAKING
            other.types[h_point] = d_.types[h_point];
            other.types.erase(0x1113);
            other.types[0x1114] =
                p3; // b: {0x1111, 0x1114}: the left-over versions differ in layout
            const auto r2 = tools::diff(two, other);
            CHECK(r2.breaking); // 2.1.11 compared 0x1111 with 0x1111 only
            std::size_t ident = 0, broke = 0;
            for (const auto& e : r2.entries)
                if (e.name == "t::Point")
                {
                    ident += e.verdict == "identical" && e.schema_a == h_point;
                    broke +=
                        e.verdict == "breaking" && e.schema_a == 0x1113 && e.schema_b == 0x1114;
                }
            CHECK(ident == 1 && broke == 1);
        }
        {
            dyn::dictionary c2 = d_;
            c2.calls.erase(h_call);
            c2.calls[0x5556] = dyn::call{0x5556, 0, {{false, h_point}}};
            c2.literals[lit_hello].text = "hallo";
            c2.literals[0x4445] = dyn::literal{0x4445, "bye"};
            const auto r = tools::diff(d_, c2);
            CHECK(r.breaking && verdict_of(r, "call 0x0000000000005555") == "call removed" &&
                  verdict_of(r, "call 0x0000000000005556") == "call added" &&
                  verdict_of(r, "literal \"hello\" -> literal \"hallo\"") == "literal changed" &&
                  verdict_of(r, "literal \"bye\"") == "literal added");
            std::println("{}", r.to_text());
            CHECK(contains(r.to_text(),
                           "2 calls: 1 added, 1 removed, 0 changed; 2 literals: 1 added, "
                           "0 removed, 1 changed  BREAKING"));
            dyn::dictionary c3 = d_;
            c3.calls[h_call].args[0].hash = h_point;
            CHECK(verdict_of(tools::diff(d_, c3), "call 0x0000000000005555") == "call changed" &&
                  tools::diff(d_, c3).breaking);
        }
    }

    // ---- ELF reader (FR-D6): in-memory image, then our own executable ----
    void elf_reader()
    {
        elf_ = make_elf(entries_);
        {
            const auto sec = tools::extract_section_from_image(elf_, "es_serreg_dict");
            CHECK(sec.has_value() && *sec == entries_);
            CHECK(tools::extract_section_from_image(elf_, "nope").error() == error::not_found);
            std::vector<std::byte> huge(elf_);
            huge[60] = std::byte{0xff};
            huge[61] = std::byte{0xff}; // e_shnum way past the image
            CHECK(tools::extract_section_from_image(huge, "es_serreg_dict").error() ==
                  error::truncated);
            std::vector<std::byte> wrap(elf_);
            std::memset(wrap.data() + 40, 0xff, 8); // e_shoff near UINT64_MAX (would wrap)
            CHECK(tools::extract_section_from_image(wrap, "es_serreg_dict").error() ==
                  error::truncated);
            CHECK(tools::extract_section_from_image(std::span(elf_).first(63), ".text").error() ==
                  error::truncated);
            // A4 (2.1.8): extract_embedded_dictionary recognises the format by its magic
            const auto emb = tools::extract_embedded_dictionary(elf_);
            CHECK(emb.has_value() && emb->types.size() == 3 && emb->calls.size() == 1);
            const std::array<std::byte, 8> pe{std::byte{'M'}, std::byte{'Z'}};
            CHECK(tools::extract_embedded_dictionary(pe).error() == error::unsupported);
            CHECK(tools::extract_embedded_dictionary(std::span(elf_).first(3)).error() ==
                  error::truncated);
        }
        if (const auto self = tools::read_file("/proc/self/exe"); self.has_value())
        {
            const auto text_sec = tools::extract_section_from_elf("/proc/self/exe", ".text");
            CHECK(text_sec.has_value() && !text_sec->empty());
            CHECK(tools::extract_section_from_elf("/proc/self/exe", "no_such_section").error() ==
                  error::not_found);
            CHECK(tools::extract_registry_from_elf("/proc/self/exe").error() == error::not_found);
            CHECK(tools::extract_section_from_elf("/definitely/missing", ".text").error() ==
                  error::not_found);
        }
        else
        {
            std::println("note: /proc/self/exe not readable; ELF reader not exercised");
        }
    }

    // ---- write_all (gk 1.9): whole buffer written; a closed descriptor is io_error, not a spin ----
    void write_all_errors()
    {
        {
            const std::string p = tmp_out_ + ".bin";
            CHECK(tools::write_file(p, capture_).has_value() &&
                  tools::read_file(p).value() == capture_);
            CHECK(tools::write_all(-1, capture_).error() == error::io_error);
            ::unlink(p.c_str());
        }
    }

    // ---- cli (cc F24, gk §2): --version, --program-hash, option-looking operands, conflicting --elf, --config ----
    void cli_surface()
    {
        {
            std::string out;
            CHECK(run_cli({"--version"}, out, tmp_out_) == 0 &&
                  out == "srdump 2.2.6 (format 1.7, hash_algo 2)\n");
            const std::string cap = tmp_out_ + ".ser", rri_path = tmp_out_ + ".rri",
                              elf_a = tmp_out_ + ".a.o", elf_b = tmp_out_ + ".b.o",
                              cfg = tmp_out_ + ".toml";
            CHECK(tools::write_file(cap, capture_).has_value() &&
                  tools::write_file(rri_path, rri_).has_value());
            const auto conflicting_rri = dyn::to_rri(conflicting_);
            const std::vector<std::byte> conflicting_entries(
                conflicting_rri.begin() + static_cast<std::ptrdiff_t>(fmt::rri_header_size),
                conflicting_rri.end());
            CHECK(tools::write_file(elf_a, elf_).has_value() &&
                  tools::write_file(elf_b, make_elf(conflicting_entries)).has_value());
            CHECK(run_cli({cap, "--program-hash"}, out, tmp_out_) == 0 &&
                  out == "0x0000000000000077\n");
            CHECK(run_cli({"--rri", rri_path, "--program-hash"}, out, tmp_out_) == 0 &&
                  out == "0x0000000000000077\n");
            // an ELF dictionary carries no header hash: FR-H3 is recomputed from its type table (v1.0.2)
            {
                dyn::dictionary own;
                CHECK(own.add_entries(entries_).has_value());
                const std::string hex = std::format("0x{:016x}\n", own.computed_program_hash());
                CHECK(run_cli({"--elf", elf_a, "--program-hash"}, out, tmp_out_) == 0 &&
                      out == hex);
            }
            CHECK(run_cli({cap, "--verify"}, out, tmp_out_) == 0 &&
                  out == "ok: 2 frames, 4 records, 104 bytes\n");
            CHECK(run_cli({cap}, out, tmp_out_) == 0 &&
                  out == text_); // streamed output == string dump
            CHECK(run_cli({cap, "--frames", "1..1", "--type", "t::Pixel"}, out, tmp_out_) == 0 &&
                  out == filtered_);
            CHECK(run_cli({"--config", "--verify", cap}, out, tmp_out_) ==
                  2); // gk §2: option-looking operand
            CHECK(run_cli({"--elf", "--json"}, out, tmp_out_) == 2 &&
                  run_cli({"--dict"}, out, tmp_out_) == 2);
            CHECK(run_cli({"--elf", elf_a}, out, tmp_out_) == 0 &&
                  contains(out, "struct t::Pixel"));
            CHECK(run_cli({"--elf", elf_a, "--elf", elf_b}, out, tmp_out_) == 1 &&
                  contains(out, "conflict 0x0000000000003333")); // cc F15/F16
            CHECK(run_cli({"--elf", elf_a, "--elf", elf_a}, out, tmp_out_) ==
                  0); // same image twice is fine
            const std::string tomls =
                "# srdump config\nverify = true\n[unused]\nframes = \"0..0\"\n";
            CHECK(tools::write_file(cfg, to_bytes(tomls)).has_value());
            CHECK(run_cli({"--config", cfg, cap}, out, tmp_out_) == 0 &&
                  contains(out, "ok: 2 frames"));
            std::string err;
            CHECK(cli::parse_config(tomls, err) && err.empty());
            CHECK(!cli::parse_config("verify = maybe\n", err) &&
                  err ==
                      "1: unsupported value (strings, integers, booleans, arrays of strings only)");
            CHECK(!cli::parse_config("a.b = 1\n", err) && err.starts_with("1: unsupported key"));
            CHECK(!cli::parse_config("nope = 1\n", err) && err == "1: unknown key \"nope\"");
            CHECK(!cli::parse_config("[broken\n", err) && err == "1: malformed table header");
            // cc review 2.1.10, D-09 (2.1.12): commas between array items; a comment after a table header
            CHECK(!cli::parse_config("diff = [\"a\" \"b\"]\n", err));
            CHECK(cli::parse_config("[srdump] # the tool\nverify = true\n", err));
            CHECK(!cli::parse_config("[x] y\n", err) && err == "1: malformed table header");
            CHECK(cli::parse_config("", err) && cli::parse_config("diff = [\"a\", \"b\"]\n", err));
            // cc review 2.1.10 (2.1.12)
            // T-07: output that cannot be written is exit 1, not 0
            if (::access("/dev/full", W_OK) == 0)
            {
                CHECK(run_cli_into({cap}, "/dev/full") == 1);
                CHECK(run_cli_into({cap, "--verify"}, "/dev/full") == 1);
                CHECK(run_cli_into({"--wireshark", "-", cap}, "/dev/full") == 1);
            }
            // T-03: a source path with newlines stays inside the Lua comment
            const std::string evil = tmp_out_ + ".x\nprint(\"INJECTED\")\n--.ser";
            CHECK(tools::write_file(evil, capture_).has_value());
            CHECK(run_cli({"--wireshark", "-", evil}, out, tmp_out_) == 0);
            CHECK(!contains(out, "\nprint(\"INJECTED\")\n") &&
                  contains(out, "?print(\"INJECTED\")?"));
            // T-08: a stream whose dictionary arrives in a dictionary frame after the data frames
            std::vector<std::byte> stream(frames_.begin(), frames_.end());
            const std::vector<std::byte> data_only = stream;
            {
                std::vector<std::byte> payload = rri_;
                payload.resize(fmt::align8(payload.size()));
                builder df;
                df.put(static_cast<std::uint32_t>(fmt::frame_header_size + payload.size()));
                df.put(static_cast<std::uint16_t>(fmt::frame_flag_dict));
                df.put(std::uint16_t{0});
                df.put(fmt::dict_frame_sentinel);
                df.bytes(payload);
                stream.insert(stream.end(), df.out.begin(), df.out.end());
            }
            const std::string late = tmp_out_ + ".late.ser", bare = tmp_out_ + ".bare.ser";
            CHECK(tools::write_file(late, stream).has_value() &&
                  tools::write_file(bare, data_only).has_value());
            CHECK(run_cli({"--wireshark", "-", late}, out, tmp_out_) == 0 &&
                  contains(out, "3 types, 1 call signatures"));
            CHECK(run_cli({"--wireshark", "-", bare}, out, tmp_out_) == 1); // nothing to describe
            for (const auto& p : {cap, rri_path, elf_a, elf_b, cfg, tmp_out_, evil, late, bare})
                ::unlink(p.c_str());
        }
    }

    // ---- checked-in negative fixtures equal the in-memory bombs (cc F2 / F17) ----
    void negative_fixtures()
    {
        const std::string negative = source_dir() + "/../fixtures/negative/";
        if (file_exists(negative + "bomb_array_extent.ser"))
        {
            CHECK(tools::read_file(negative + "bomb_array_extent.ser").value() == bomb_);
            CHECK(tools::read_file(negative + "entry_n_elem_65535.bin").value() == f17_);
        }
        else
        {
            std::println(
                "note: tests/fixtures/negative not found next to the source; run with a fixtures "
                "dir to (re)generate it");
        }
    }

    // ---- fixture generation: seed corpora for tests/fuzz and the negative fixtures ----
    void generate_fixtures()
    {
        if (opts_.fixtures_dir)
        {
            const std::string root = opts_.fixtures_dir;
            for (const char* const sub : {"", "/corpus", "/corpus/rri", "/corpus/capture",
                                          "/corpus/elf", "/corpus/config", "/negative"})
                ::mkdir((root + sub).c_str(), 0755);
            write_fixture(root + "/corpus/rri/selftest.rri", rri_);
            write_fixture(root + "/corpus/rri/big.rri", dyn::to_rri(big_.dict));
            write_fixture(root + "/corpus/capture/selftest.ser", capture_);
            write_fixture(root + "/corpus/capture/bare_frames.bin",
                          std::vector<std::byte>(frames_.begin(), frames_.end()));
            write_fixture(root + "/corpus/capture/big_bare.bin", big_.frames);
            {
                builder s;
                s.put(std::uint32_t{16 + static_cast<std::uint32_t>(rri_.size())});
                s.put(fmt::frame_flag_dict);
                s.put(std::uint16_t{0});
                s.put(fmt::dict_frame_sentinel);
                s.bytes(rri_);
                s.bytes(std::vector<std::byte>(frames_.begin(), frames_.end()));
                CHECK(tools::verify(s.out).value().ok());
                write_fixture(root + "/corpus/capture/dict_frame_stream.bin", s.out);
            }
            write_fixture(root + "/corpus/elf/minimal.o", elf_);
            write_fixture(root + "/corpus/config/selftest.toml",
                          to_bytes("# srdump config\nverify = true\njson = false\nformat = "
                                   "\"text\"\nframes = \"0..10\"\n"
                                   "type = \"t::Pixel\"\ndict = [\"a.rri\", \"b.rri\"]\nelf = "
                                   "\"app\"\n[section]\nhex = true\n"));
            write_fixture(root + "/negative/bomb_array_extent.ser", bomb_);
            write_fixture(root + "/negative/entry_n_elem_65535.bin", f17_);
        }
    }
};
} // namespace runtime_selftest

int main(const int argc, char** const argv)
{
    return runtime_selftest::app{runtime_selftest::options::parse(argc, argv)}.run();
}
