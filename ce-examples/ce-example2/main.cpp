// ce-example2 — es::serreg dictionary blobs, call/literal ids and canonical images on Compiler Explorer
// (FR-H4 cross-check for the wire bytes, DECISIONS T-3).
//
// What it tests, against values frozen in the project (GCC 16.2, x86-64, format 1.3):
//   1. ENTRY blobs (detail::type_blob_bytes_v<T>) — the bytes that go into the ELF section, the RRI and every
//      capture dictionary: XXH3-64 of the blob + its size (tests/golden/blobs.cpp);
//   2. call-signature hashes and literal ids (tests/golden/hashes.cpp, format 1.1 locks);
//   3. the canonical image of a dynamic type, byte for byte (plan v2.0 §2; README "Dynamic members"), and a
//      decode round trip of three dynamic types (encode → decode → operator==).
// Needs 10 library headers (symlinks into include/): hash error format descriptor reflect literal arg blob
// detail/embed_elf detail/embed_none. No libserreg: nothing registers or links; the one library function the
// decoder may call (detail::report_warning, defined in src/decoder.cpp) is stubbed below.
//
// Compilers (Compiler Explorer names / ids):
//   x86-64 gcc 16                          -std=c++26 -freflection -O1 -I.
//   x86-64 clang (reflection - C++26)      -std=c++26 -freflection-latest -stdlib=libc++ -O1 -I.   (clang_bb_p2996)
// Add -DES_SERREG_EMBED_NONE to leave out the es_serreg_dict section (the "none" image backend).
//
// Exit status: 0 when every golden value matches; 1 otherwise (each mismatch is printed as DIFF).
#include <es/serreg/blob.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The decoder reports repaired bool bytes through this libserreg function; this example has no library.
namespace es::serreg::detail
{
void report_warning(warning, std::uint64_t, std::uint32_t) noexcept
{
}
} // namespace es::serreg::detail

// ---- corpus (verbatim from tests/golden/corpus.hpp) ----------------------------------------------------------
namespace corpus
{
struct Scalars
{
    std::int64_t i;
    double d;
    std::uint32_t u;
    std::int32_t s;
};
struct Padded
{
    std::uint8_t tag;
    std::uint64_t value;
    std::uint16_t code;
};
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
enum Venue : int
{
    nyse,
    nasdaq = 5,
    arca
};
struct Arrays
{
    std::int32_t c[4];
    std::array<double, 2> a;
    char sym[8];
    std::uint8_t grid[2][3];
};
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
    Side side;
    char sym[8];
    bool operator==(const Order&) const = default;
};
struct Cancel
{
    std::uint64_t id;
    std::uint32_t reason;
};
struct DynMsg
{
    std::uint32_t id;
    std::string name;
    std::vector<std::int16_t> xs;
    bool operator==(const DynMsg&) const = default;
};
struct DynNested
{
    std::vector<DynMsg> msgs;
    std::optional<Order> maybe;
    std::variant<std::int32_t, Order, std::string> v;
    bool operator==(const DynNested&) const = default;
};
struct DynArr
{
    std::string names[2];
    std::vector<std::string> tags;
    std::optional<Side> side;
    std::variant<std::monostate, DynMsg> maybe_msg;
    bool operator==(const DynArr&) const = default;
};
struct DynBase : Order
{
    std::string note;
    bool operator==(const DynBase&) const = default;
};
} // namespace corpus

namespace
{
struct[[= es::serreg::name("corpus::Local")]]
    Local // 2.1.14: no linkage-unique name, so named explicitly
{
    std::uint16_t a;
    std::uint16_t b;
};
} // namespace

// ---- the checks ---------------------------------------------------------------------------------------------
namespace
{
namespace sr = es::serreg;
int failures = 0;

void report(const bool ok, const std::string_view what, const std::string_view detail)
{
    if (!ok)
        ++failures;
    std::println("{:4} {:34} {}", ok ? "ok" : "DIFF", what, detail);
}

template<class T>
void blob(const std::string_view label, const std::uint64_t hash, const std::size_t size)
{
    const std::span<const std::byte> b(sr::detail::type_blob_bytes_v<T>);
    const std::uint64_t got = sr::fingerprint64(b);
    report(got == hash && b.size() == size, label,
           std::format("blob xxh3 0x{:016x} size {:4}   (golden 0x{:016x} {})", got, b.size(), hash,
                       size));
}

void id(const std::string_view label, const std::uint64_t got, const std::uint64_t want)
{
    report(got == want, label, std::format("0x{:016x}   (golden 0x{:016x})", got, want));
}

std::string hex(const std::span<const std::byte> b)
{
    std::string s;
    for (const std::byte x : b)
        s += std::format("{:02x} ", std::to_integer<unsigned>(x));
    return s;
}

template<class T> void round_trip(const std::string_view label, const T& v)
{
    std::vector<std::byte> img(sr::detail::measure_image(v));
    sr::detail::encode_image(v, img.data(), img.size());
    T back{};
    const auto st = sr::detail::decode_image(back, std::span<const std::byte>(img));
    report(st.has_value() && back == v, label,
           std::format("image {} bytes, decode {}", img.size(),
                       st ? "ok" : sr::to_string(st.error())));
}
} // namespace

int main()
{
    using namespace sr::literals;

    // 1. ENTRY blobs — tests/golden/blobs.cpp
    blob<corpus::Scalars>("blob Scalars", 0x5b676741452a6b58, 192);
    blob<corpus::Padded>("blob Padded", 0x9c624beb5c8b5b77, 168);
    blob<corpus::Side>("blob Side (enum)", 0x48f8e7e66398d256, 88); // 2.2.0: enum_flag_fixed
    blob<corpus::Venue>("blob Venue (enum)", 0x32332c620c674179, 104);
    blob<corpus::Arrays>("blob Arrays", 0x0f1c70994e73fec3, 360);
    blob<corpus::Order>("blob Order", 0x8bb3bcf5a04fdc0c, 264);
    blob<corpus::Cancel>("blob Cancel", 0x160d8de017e45e0f, 128);
    blob<Local>("blob Local (anon ns, named)", 0x6c69ffbf01e9159d, 128);
    blob<corpus::DynMsg>("blob DynMsg (string, vector)", 0x49d43c2ec5419e62, 192);
    blob<corpus::DynNested>("blob DynNested (vec/opt/variant)", 0x584f85e6e0c71876, 328);
    blob<corpus::DynArr>("blob DynArr", 0xc3060b35d803ae72, 368);
    blob<corpus::DynBase>("blob DynBase (flat base)", 0x26ba4a1abc9df924, 152);

    // 2. call signatures and literals — tests/golden/hashes.cpp (format 1.1 locks)
    id("call(2){string,i32}", sr::call_hash_v<const char*, int>, 0xef7e3878972feec1);
    id("print(2){lit:\"x {}\",i32}", sr::call_hash_v<sr::print_tag, decltype("x {}"_lit), int>,
       0x062ff32816c78e7d);
    id("println(2){lit:\"x {}\",i32}", sr::call_hash_v<sr::println_tag, decltype("x {}"_lit), int>,
       0x90a1eb79c6e2eda5);
    id("print(4) plan §3 frame",
       sr::call_hash_v<sr::print_tag, decltype("hello {} {} {}"_lit), const char*, int,
                       decltype("worlds"_lit)>,
       0xe97f49911b00565e);

    // 3. canonical image, byte-exact: DynMsg{7, "ab", {1, -2, 3}} (README, plan v2.0 §2)
    const corpus::DynMsg m{7, "ab", {1, -2, 3}};
    std::vector<std::byte> img(sr::detail::measure_image(m));
    sr::detail::encode_image(m, img.data(), img.size());
    const std::array<unsigned char, 28> want{0x07, 0,    0,    0, 0x14, 0,    0,    0, 0x02, 0,
                                             0,    0,    0x16, 0, 0,    0,    0x03, 0, 0,    0,
                                             0x61, 0x62, 0x01, 0, 0xfe, 0xff, 0x03, 0};
    bool same = img.size() == want.size();
    for (std::size_t i = 0; same && i < want.size(); ++i)
        same = std::to_integer<unsigned char>(img[i]) == want[i];
    report(same, "image DynMsg{7,\"ab\",{1,-2,3}}", hex(img));

    round_trip("round trip DynMsg", m);
    corpus::Order o{42, 1050000, 3, corpus::Side::sell, "AAPL"};
    round_trip("round trip DynNested",
               corpus::DynNested{{m, corpus::DynMsg{8, "", {}}}, o, std::string("var")});
    round_trip("round trip DynArr", corpus::DynArr{{"n0", "n1"}, {"x", "yy"}, std::nullopt, m});

    std::println("{} golden mismatches", failures);
    return failures == 0 ? 0 : 1;
}
