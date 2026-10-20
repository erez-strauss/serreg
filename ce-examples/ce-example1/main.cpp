// ce-example1 — es::serreg fingerprint layer on Compiler Explorer (FR-H4 cross-check, DECISIONS T-3).
//
// What it tests: canonical type names (§6.2b) and the XXH3-64 schema/layout hashes (FR-H1/H4) computed by
// reflect.hpp, against the values frozen in tests/golden/hashes.cpp (GCC 16.2, x86-64, format 1.3).
// Needs only 5 library headers (es/serreg/{hash,error,format,descriptor,reflect}.hpp, symlinks into include/)
// and no libserreg: nothing here registers, serializes or links.
//
// Compilers (Compiler Explorer names / ids):
//   x86-64 gcc 16                          -std=c++26 -freflection -O1 -I.
//   x86-64 clang (reflection - C++26)      -std=c++26 -freflection-latest -stdlib=libc++ -O1 -I.   (clang_bb_p2996)
// The corpus types below are copied verbatim from tests/golden/corpus.hpp (same namespaces, names, members): a
// schema hash covers names and offsets, so any edit here changes the expected values. The "probe" section has no
// golden values: it prints what review findings C-03/C-04 are about, for comparing the two compilers' output.
//
// Exit status: 0 when every golden value matches; 1 otherwise (each mismatch is printed as DIFF).
#include <es/serreg/reflect.hpp>

#include <array>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
struct Nested
{
    Scalars head;
    Padded body;
    std::int32_t n;
};
struct Base
{
    std::uint32_t id;
};
struct Tag2
{
    std::uint16_t t;
};
struct Derived : Base
{
    std::int32_t x;
};
struct MultiDerived : Base, Tag2
{
    std::uint8_t k;
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
struct Empty
{
};
struct WithEmpty
{
    [[no_unique_address]] Empty e;
    std::int32_t v;
};
class Account
{
public:
    Account() = default;

private:
    std::uint64_t id_ = 0;
    std::int64_t balance_ = 0;
};
struct[[= es::serreg::since(1)]] Versioned
{
    std::uint32_t id;
    [[= es::serreg::since(3)]] std::uint32_t venue;
    [[= es::serreg::deprecated(5)]] std::uint32_t legacy;
};
struct Unversioned
{
    std::uint32_t id;
    std::uint32_t venue;
    std::uint32_t legacy;
};
struct Skipped
{
    std::uint64_t key;
    [[= es::serreg::skip]] std::uint64_t cache;
    std::uint32_t n;
};
template<class T, std::size_t N> struct Vec
{
    T v[N];
};
using Vec3i = Vec<std::int32_t, 3>;
using Vec3 = Vec<Side, 3>;
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
struct Point2i
{
    std::int32_t x;
    std::int32_t y;
};
struct Pair2i
{
    std::int32_t a;
    std::int32_t b;
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

namespace t
{
enum class Mode : std::uint8_t
{
    fast = 7
};
template<class T, auto V> struct W
{
    T v;
};
using WBool = W<std::int32_t, true>;
using WChar = W<std::int32_t, 'x'>;
using WEnum = W<std::int32_t, Mode::fast>;
using WNeg = W<std::int32_t, -5>;
} // namespace t

// ---- probes (review C-03, C-04): no golden values; compare the printed lines between compilers --------------
namespace probe
{
template<class T> struct Outer
{
    struct Inner
    {
        T v;
    };
};
template<class T> struct Box
{
    T v;
};
template<class T>
struct Tagged // T appears only in the name: shows how a std type is spelled (C-04)
{
    std::int32_t v;
};
} // namespace probe

// ---- the check ----------------------------------------------------------------------------------------------
namespace
{
int failures = 0;

template<class T>
void golden(const std::string_view label, const std::string_view name, const std::uint64_t schema,
            const std::uint64_t layout)
{
    const std::string_view got_name = es::serreg::canonical_name_v<T>;
    const std::uint64_t got_schema = es::serreg::schema_hash_v<T>;
    const std::uint64_t got_layout = es::serreg::layout_hash_v<T>;
    const bool ok =
        (name.empty() || got_name == name) && got_schema == schema && got_layout == layout;
    if (!ok)
        ++failures;
    std::println("{:4} {:22} {:34} schema 0x{:016x} layout 0x{:016x}", ok ? "ok" : "DIFF", label,
                 got_name, got_schema, got_layout);
    if (!ok)
        std::println("     expected {:34} schema 0x{:016x} layout 0x{:016x}", name, schema, layout);
}

template<class T> void probe_line(const std::string_view label)
{
    std::println("PROBE {:26} {}  schema 0x{:016x}", label, es::serreg::canonical_name_v<T>,
                 es::serreg::schema_hash_v<T>);
}
} // namespace

int main()
{
    using es::serreg::fingerprint64;
    // XXH3-64 known answer (tests/unit/test_hash.cpp)
    const bool xxh3_ok = fingerprint64("string") == 0xd8ad194648e774f1ull;
    failures += xxh3_ok ? 0 : 1;
    std::println("{:4} fingerprint64(\"string\") = 0x{:016x}", xxh3_ok ? "ok" : "DIFF",
                 fingerprint64("string"));

    // golden values: tests/golden/hashes.cpp (FR-H1/FR-H4, §6.2b); "" = name not frozen there
    golden<corpus::Scalars>("Scalars", "", 0x0773476d2708557f, 0xb5bbf863d02230ef);
    golden<corpus::Padded>("Padded", "", 0x45d0212145051ee5, 0xefa34e33c5394e5a);
    golden<corpus::Nested>("Nested", "", 0x6aefae2ed0329a88, 0xf2451b660cc97379);
    golden<corpus::Base>("Base", "", 0xc83d6ca60fea9cd8, 0x077aac8f71faa349);
    golden<corpus::Tag2>("Tag2", "", 0x1bb7a3b344a42c01, 0x8bcafb69a4d4c365);
    golden<corpus::Derived>("Derived", "", 0xa8dc6824ca907ee1, 0x1f656e4e6820dd87);
    golden<corpus::MultiDerived>("MultiDerived", "corpus::MultiDerived", 0x12ff4ce6dc11b75d,
                                 0xe4c99473004f2508);
    golden<corpus::Side>("Side", "corpus::Side", 0x074e267cca5188e1, 0xfdccdf0b5609277b);
    golden<corpus::Venue>("Venue", "corpus::Venue", 0xe513f0b42c4e8f45, 0xcd97d07ac5e40b4a);
    golden<corpus::Arrays>("Arrays", "", 0xaca08ad8860aa982, 0x41cbe109607026f5);
    golden<corpus::WithEmpty>("WithEmpty", "", 0x98e7b2625c6dcadc, 0x4e0a9e986f2d5b5e);
    golden<corpus::Account>("Account (private)", "corpus::Account", 0xdde516232a3664d8,
                            0x27d87c311fb508f8);
    golden<corpus::Versioned>("Versioned", "", 0x899e36549dfabd4c, 0x52b37c2c9b2f54f2);
    golden<corpus::Unversioned>("Unversioned", "", 0x23599b0163a6b6a7, 0x52b37c2c9b2f54f2);
    golden<corpus::Skipped>("Skipped", "", 0x45d0763cd035e303, 0xc5187469d517b843);
    golden<corpus::Vec3>("Vec3", "corpus::Vec<corpus::Side,3>", 0x7fdb040866c570e1,
                         0xad3ceb2af17cbb33);
    golden<corpus::Vec3i>("Vec3i", "corpus::Vec<i32,3>", 0xa0c0848b2bfff86e, 0x31820848c4e244b6);
    golden<corpus::Empty>("Empty", "corpus::Empty", 0x2a2c52405ef9708b, 0x3ba1f9a330b68a73);
    golden<corpus::Order>("Order", "corpus::Order", 0x367f8cd8c6441dd2, 0x0661bb3e388c01fe);
    golden<corpus::Cancel>("Cancel", "", 0x35af6843b770c519, 0x99c125fd4fe8673c);
    golden<corpus::Point2i>("Point2i", "", 0x78baef33198c1282, 0x50e920f7a4899338);
    golden<corpus::Pair2i>("Pair2i", "", 0xe33c653e918e7443, 0x50e920f7a4899338);
    golden<Local>("Local (anon ns, named)", "corpus::Local", 0x4e2c01f6b569c2ed,
                  0x6333a3cc649863d7);
    golden<t::WBool>("W<i32,true>", "t::W<i32,true>", 0x3739a1409b708988, 0x26dc44c15cd38ea0);
    golden<t::WChar>("W<i32,'x'>", "t::W<i32,120>", 0xf8fac147067be6fd, 0x26dc44c15cd38ea0);
    golden<t::WEnum>("W<i32,Mode::fast>", "t::W<i32,enum(7)>", 0x202ee12828f6423c,
                     0x26dc44c15cd38ea0);
    golden<t::WNeg>("W<i32,-5>", "t::W<i32,-5>", 0x1bb677b7ffc98ec4, 0x26dc44c15cd38ea0);
    golden<t::Mode>("Mode", "", 0x2365a1001785cb81, 0xfdccdf0b5609277b);

    // probes (C-03, C-04; fixed in 2.1.14): the printed lines must now be equal on every lane, and the two
    // Outer<…>::Inner lines must differ
    probe_line<probe::Outer<int>::Inner>("Outer<int>::Inner");
    probe_line<probe::Outer<long>::Inner>("Outer<long>::Inner");
    probe_line<probe::Box<std::string>>("Box<std::string>");
    probe_line<probe::Tagged<std::string>>("Tagged<std::string>");
    probe_line<probe::Tagged<std::pair<int, int>>>("Tagged<std::pair<int,int>>");
    probe_line<probe::Tagged<std::array<int, 2>>>("Tagged<std::array<int,2>>");
    probe_line<probe::Tagged<std::vector<int>>>("Tagged<std::vector<int>>");

    std::println("{} golden mismatches", failures);
    return failures == 0 ? 0 : 1;
}
