// tests/golden/corpus.hpp — the shared test corpus (spec §10.2 #1, #4). Every shape the v1 flat model supports,
// one type per feature, so golden hashes, round-trips, registry counts and dlopen tests all speak about the
// same types. Header-only; anonymous-namespace and template types are intentionally included.
#pragma once
#include <es/serreg/serreg.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace corpus
{

// 1. plain struct of scalars — no padding (24 bytes, align 8)
struct Scalars
{
    std::int64_t i;
    double d;
    std::uint32_t u;
    std::int32_t s;
};

// 2. struct with padding: gaps at [1,7] and [18,6]
struct Padded
{
    std::uint8_t tag;
    std::uint64_t value;
    std::uint16_t code;
};

// 3. nested struct
struct Nested
{
    Scalars head;
    Padded body;
    std::int32_t n;
};

// 4. bases: one base, and two bases
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

// 5. enums: scoped u8, unscoped int
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
}; // fixed underlying: an unfixed enum is u32 on GCC (implementation-defined)

// 6. arrays: C array, std::array, char[8], 2-D array
struct Arrays
{
    std::int32_t c[4];
    std::array<double, 2> a;
    char sym[8];
    std::uint8_t grid[2][3];
};

// 7. [[no_unique_address]] empty member
struct Empty
{
};
struct WithEmpty
{
    [[no_unique_address]] Empty e;
    std::int32_t v;
};

// 8. private members, no friend declarations (FR-S9)
class Account
{
public:
    Account() = default;
    Account(const std::uint64_t id, const std::int64_t balance) noexcept
        : id_(id), balance_(balance)
    {
    }
    std::uint64_t id() const noexcept
    {
        return id_;
    }
    std::int64_t balance() const noexcept
    {
        return balance_;
    }

private:
    std::uint64_t id_ = 0;
    std::int64_t balance_ = 0;
};

// 9. annotations on members and on the type (FR-S11)
struct[[= es::serreg::since(1)]] Versioned
{
    std::uint32_t id;
    [[= es::serreg::since(3)]] std::uint32_t venue;
    [[= es::serreg::deprecated(5)]] std::uint32_t legacy;
};
// same layout as Versioned, no annotations: layout hash equal, schema hash different
struct Unversioned
{
    std::uint32_t id;
    std::uint32_t venue;
    std::uint32_t legacy;
};

// 10. [[=skip]] member — scrubbed like padding, absent from the descriptor
struct Skipped
{
    std::uint64_t key;
    [[= es::serreg::skip]] std::uint64_t cache;
    std::uint32_t n;
};

// 11. class template specializations → "corpus::Vec<i32,3>" (scalar argument renders as a kind name) and
//     "corpus::Vec<corpus::Side,3>" (enum argument renders as a canonical name)
template<class T, std::size_t N> struct Vec
{
    T v[N];
};
using Vec3i = Vec<std::int32_t, 3>;
using Vec3 = Vec<Side, 3>;

// 12. as_flat mapping (FR-S8): Stamp is serialised as StampFlat
struct Stamp
{
    std::chrono::nanoseconds t;
};
struct StampFlat
{
    std::int64_t ns;
};

// 13. the "message" types used throughout the unit tests and examples
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

// 14. structurally identical pair with different member names → equal layout hash, different schema hash
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

inline bool operator==(const Order& l, const Order& r) noexcept
{
    return l.id == r.id && l.px == r.px && l.qty == r.qty && l.side == r.side &&
           std::memcmp(l.sym, r.sym, sizeof l.sym) == 0;
}
inline bool operator==(const Cancel& l, const Cancel& r) noexcept
{
    return l.id == r.id && l.reason == r.reason;
}
inline bool operator==(const Scalars& l, const Scalars& r) noexcept
{
    return l.i == r.i && l.d == r.d && l.u == r.u && l.s == r.s;
}
inline bool operator==(const Padded& l, const Padded& r) noexcept
{
    return l.tag == r.tag && l.value == r.value && l.code == r.code;
}
inline bool operator==(const Nested& l, const Nested& r) noexcept
{
    return l.head == r.head && l.body == r.body && l.n == r.n;
}
inline bool operator==(const Arrays& l, const Arrays& r) noexcept
{ // field-wise: the type has 2 bytes of tail padding
    return std::memcmp(l.c, r.c, sizeof l.c) == 0 && l.a == r.a &&
           std::memcmp(l.sym, r.sym, sizeof l.sym) == 0 &&
           std::memcmp(l.grid, r.grid, sizeof l.grid) == 0;
}
inline bool operator==(const Derived& l, const Derived& r) noexcept
{
    return l.id == r.id && l.x == r.x;
}
inline bool operator==(const MultiDerived& l, const MultiDerived& r) noexcept
{
    return l.id == r.id && l.t == r.t && l.k == r.k;
}
inline bool operator==(const Account& l, const Account& r) noexcept
{
    return l.id() == r.id() && l.balance() == r.balance();
}
inline bool operator==(const Vec3& l, const Vec3& r) noexcept
{
    return std::memcmp(l.v, r.v, sizeof l.v) == 0;
}
inline bool operator==(const Vec3i& l, const Vec3i& r) noexcept
{
    return std::memcmp(l.v, r.v, sizeof l.v) == 0;
}
inline bool operator==(const Stamp& l, const Stamp& r) noexcept
{
    return l.t == r.t;
}
inline bool operator==(const WithEmpty& l, const WithEmpty& r) noexcept
{
    return l.v == r.v;
}

// Sample values (deterministic) used by several tests.
inline Order sample_order() noexcept
{
    return Order{42, 1050000, 100, Side::buy, "AAPL"};
}
inline Cancel sample_cancel() noexcept
{
    return Cancel{42, 7};
}

// 16. v2 dynamic types (docs/plans-for-v2.0.md): canonical images, frozen by hashes.cpp / blobs.cpp / dynamic.*
struct DynMsg
{ // the plan §2 byte-exact example: fixed part 20 bytes, align 4
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
{ // a flat base inside a dynamic class: the base image is the flat Order bytes at canonical offset 0
    std::string note;
    bool operator==(const DynBase&) const = default;
};
inline DynMsg sample_dyn_msg()
{
    return DynMsg{7, "ab", {1, -2, 3}};
}
inline DynNested sample_dyn_nested()
{
    return DynNested{{sample_dyn_msg(), DynMsg{8, "", {}}}, sample_order(), std::string("var")};
}
inline DynArr sample_dyn_arr()
{
    return DynArr{{"n0", "n1"}, {"x", "yy"}, std::nullopt, sample_dyn_msg()};
}
inline DynBase sample_dyn_base()
{
    DynBase b{};
    static_cast<Order&>(b) = sample_order();
    b.note = "note";
    return b;
}
// 2.2.0 (format 1.6): maps and sets as members — an ordered map with string keys, a set, an unordered map whose
// entries are written in canonical (key) order whatever its iteration order, and a map of a dynamic value.
struct DynMap
{
    std::uint32_t id;
    std::map<std::string, std::int32_t> by_name;
    std::set<std::uint16_t> ids;
    std::unordered_map<std::int32_t, std::string> names;
    std::map<std::uint8_t, DynMsg> msgs;
    bool operator==(const DynMap&) const = default;
};
inline DynMap sample_dyn_map()
{
    DynMap m;
    m.id = 9;
    m.by_name = {{"b", 2}, {"a", 1}, {"c", 3}};
    m.ids = {30, 10, 20};
    m.names = {{5, "five"}, {4, "four"}, {-1, "minus one"}};
    m.msgs = {{2, DynMsg{2, "two", {2}}}, {1, DynMsg{1, "one", {1, 1}}}};
    return m;
}

} // namespace corpus

// 15. anonymous-namespace type: no linkage-unique name, so it must be named explicitly (2.1.14, review C-03/C-05,
//     maintainer 2026-10-02). The name is the same in every TU that includes this header: one registry entry per
//     process (FR-R4).
namespace
{
struct[[= es::serreg::name("corpus::Local")]] Local
{
    std::uint16_t a;
    std::uint16_t b;
};
} // namespace

// 16. value template arguments (cc F3): bool, char, enum and negative-int NTTPs render canonically as
//     "t::W<i32,true>", "t::W<i32,120>", "t::W<i32,enum(7)>", "t::W<i32,-5>" — never a compiler spelling.
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
using WChar = W<std::int32_t, 'x'>;        // 120
using WEnum = W<std::int32_t, Mode::fast>; // enum(7)
using WNeg = W<std::int32_t, -5>;
} // namespace t

// 17. (2.1.14, review C-03) a class nested in a class-template specialisation is named through its parent's
//     canonical arguments: Keyed<Order>::Key and Keyed<Cancel>::Key are distinct (they were both "{unnamed}::Key"
//     with one hash, their layouts being equal).
namespace corpus
{
template<class Tag> struct Keyed
{
    struct Key
    {
        std::uint64_t id;
    };
};
using KeyOrder = Keyed<Order>::Key;
using KeyCancel = Keyed<Cancel>::Key;

// 18. (2.1.14, review C-04) standard-library template arguments use the frozen std vocabulary, never a library's
//     spelling: Box<std::string> is "corpus::Box<std::string>" on libstdc++ (old and new ABI) and libc++ alike,
//     and a std::pmr::string argument names the same type (the allocator is not data).
template<class T> struct Box
{
    T v;
};
using BoxString = Box<std::string>;
using BoxPmrString = Box<std::pmr::string>;
using BoxVector = Box<std::vector<std::int32_t>>;

// 19. (2.1.14) a user inline namespace stays in the name: corpus::v1::InV1 and a later corpus::v2::InV1 are two
//     types (name it explicitly to keep one name across versions).
inline namespace v1
{
struct InV1
{
    std::int32_t x;
};
} // namespace v1

// 20. (2.1.14) explicit names: es::serreg::type_properties<T> (any type, also std and third-party ones) and the
//     [[=es::serreg::name("…")]] annotation; a class nested in a named class is named through it.
struct Renamed
{
    std::int32_t x;
};
struct[[= es::serreg::name("corpus::Alias")]] Annotated
{
    struct Part
    {
        std::int16_t p;
    };
    Part part;
};
} // namespace corpus
template<> struct es::serreg::type_properties<corpus::Renamed>
{
    static consteval std::string name()
    {
        return "corpus::Legacy";
    }
};

template<> struct es::serreg::as_flat<corpus::Stamp>
{
    using type = corpus::StampFlat;
    static corpus::StampFlat to(const corpus::Stamp& s) noexcept
    {
        return {s.t.count()};
    }
    static corpus::Stamp from(const corpus::StampFlat& f) noexcept
    {
        return {std::chrono::nanoseconds{f.ns}};
    }
};
