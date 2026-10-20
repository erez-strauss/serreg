// tests/unit/test_register_namespace.cpp — A-22: register every serialisable type of a namespace before main()
// (register_namespace.hpp): the walk, the selectors, the serialisability filter, the registration itself.
#include <es/serreg/register_namespace.hpp>
#include <es/serreg/serreg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sr = es::serreg;

namespace rns
{
struct Order
{
    std::uint64_t id;
    std::int64_t px;
};
struct[[= sr::message]] Fill
{
    std::uint64_t id;
    std::uint32_t qty;
};
struct MsgHeartbeat
{
    std::uint32_t seq;
};
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Helper // a pointer: not serialisable
{
    void* p;
};
struct Poly // polymorphic: not serialisable
{
    virtual ~Poly() = default;
    int x;
};
struct[[= sr::skip]] Scratch // opted out
{
    int x;
};
template<class T> struct Box // a template, not a type
{
    T v;
};
using Alias = Order; // an alias, not a type of its own
struct Incomplete;   // never completed
namespace inner
{
struct MsgOrderV2
{
    std::uint64_t id;
    std::string sym;
    std::vector<std::int32_t> legs;
};
} // namespace inner
int helper();
} // namespace rns

namespace rns_excluded // nothing registers this namespace: its type must stay unknown to the registry
{
struct Lonely
{
    std::uint32_t a;
};
} // namespace rns_excluded

ES_SERREG_REGISTER_NAMESPACE(rns);

namespace
{
consteval bool has(const std::span<const char* const> names, const std::string_view n)
{
    for (const char* const x : names)
        if (std::string_view{x} == n)
            return true;
    return false;
}
} // namespace

TEST_CASE("A-22 register_namespace: the walk lists complete class and enum types, nested "
          "namespaces included",
          "[registry][namespace]")
{
    constexpr auto all = sr::selected_type_names<^^rns>();
    STATIC_REQUIRE(
        all.size() ==
        7); // Order, Fill, MsgHeartbeat, Side, Helper, Poly, inner::MsgOrderV2 (not Scratch)
    STATIC_REQUIRE(has(all, "rns::Order"));
    STATIC_REQUIRE(has(all, "rns::Side"));
    STATIC_REQUIRE(has(all, "rns::inner::MsgOrderV2"));
    STATIC_REQUIRE(has(all, "rns::Helper"));   // selected (it is a type) …
    STATIC_REQUIRE(!has(all, "rns::Scratch")); // [[=skip]] is never selected
    STATIC_REQUIRE(!has(all, "rns::Alias"));
    STATIC_REQUIRE(!has(all, "rns::Incomplete"));
    constexpr auto flat = sr::selected_type_names<^^rns,
                                                  sr::select_all{
                                                  },
                                                  false>(); // no recursion
    STATIC_REQUIRE(!has(flat, "rns::inner::MsgOrderV2"));
    STATIC_REQUIRE(has(flat, "rns::Order"));
}

TEST_CASE("A-22 register_namespace: selectors by prefix, suffix, glob, qualified glob, annotation "
          "and combinators",
          "[registry][namespace]")
{
    constexpr auto pre = sr::selected_type_names<^^rns, sr::name_prefix<"Msg">{
                                                        }>();
    STATIC_REQUIRE(pre.size() == 2 && has(pre, "rns::MsgHeartbeat") &&
                   has(pre, "rns::inner::MsgOrderV2"));
    constexpr auto suf = sr::selected_type_names<^^rns, sr::name_suffix<"V2">{
                                                        }>();
    STATIC_REQUIRE(suf.size() == 1 && has(suf, "rns::inner::MsgOrderV2"));
    constexpr auto glob = sr::selected_type_names<^^rns, sr::name_glob<"*Order*">{
                                                         }>();
    STATIC_REQUIRE(glob.size() == 2 && has(glob, "rns::Order") &&
                   has(glob, "rns::inner::MsgOrderV2"));
    constexpr auto q = sr::selected_type_names<^^rns, sr::qualified_glob<"rns::inner::*">{
                                                      }>();
    STATIC_REQUIRE(q.size() == 1);
    constexpr auto ann = sr::selected_type_names<^^rns, sr::annotated<sr::message_t>{
                                                        }>();
    STATIC_REQUIRE(ann.size() == 1 && has(ann, "rns::Fill"));
    constexpr auto combo = sr::selected_type_names<
        ^^rns, sr::all_of<sr::name_glob<"*Order*">{}, sr::not_<sr::name_prefix<"Msg">{}>{}>{
               }>();
    STATIC_REQUIRE(combo.size() == 1 && has(combo, "rns::Order"));
    constexpr auto either = sr::selected_type_names<
        ^^rns, sr::any_of<sr::annotated<sr::message_t>{}, sr::name_suffix<"beat">{}>{
               }>();
    STATIC_REQUIRE(either.size() == 2);
    // the glob itself
    STATIC_REQUIRE(sr::detail::glob_match("*", "anything"));
    STATIC_REQUIRE(sr::detail::glob_match("Msg?", "MsgA") &&
                   !sr::detail::glob_match("Msg?", "MsgAB"));
    STATIC_REQUIRE(sr::detail::glob_match("a*b*c", "a--b---c") &&
                   !sr::detail::glob_match("a*b*c", "a--c"));
    STATIC_REQUIRE(!sr::detail::glob_match("", "x") && sr::detail::glob_match("", ""));
}

TEST_CASE("A-22 register_namespace: only serialisable types register; the rest of the selection is "
          "skipped",
          "[registry][namespace]")
{
    STATIC_REQUIRE(sr::detail::supported_type<rns::Order>());
    STATIC_REQUIRE(sr::detail::supported_type<rns::inner::MsgOrderV2>());
    STATIC_REQUIRE(sr::detail::supported_type<rns::Side>());
    STATIC_REQUIRE(!sr::detail::supported_type<rns::Helper>());
    STATIC_REQUIRE(!sr::detail::supported_type<rns::Poly>());
    STATIC_REQUIRE(sr::registered_type_count<^^rns> == 5);
    STATIC_REQUIRE(sr::registered_type_count<^^rns, sr::name_prefix<"Msg">{
                                                    }> == 2);
}

TEST_CASE("A-22 register_namespace: the types are in the registry before main() without a "
          "serialize() call site",
          "[registry][namespace]")
{
    const sr::registry& r = sr::registry::instance();
    CHECK(r.find_type(sr::schema_hash_v<rns::Order>) != nullptr);
    CHECK(r.find_type(sr::schema_hash_v<rns::Fill>) != nullptr);
    CHECK(r.find_type(sr::schema_hash_v<rns::MsgHeartbeat>) != nullptr);
    CHECK(r.find_type(sr::schema_hash_v<rns::Side>) != nullptr);
    CHECK(r.find_type(sr::schema_hash_v<rns::inner::MsgOrderV2>) != nullptr);
    CHECK(r.find_type(sr::schema_hash_v<rns_excluded::Lonely>) == nullptr); // nobody registered it
    // the registered type is usable by a dictionary-driven reader: its ENTRY is in the exported image
    const auto d = sr::tools::load_dictionary_rri(r.export_image());
    REQUIRE(d.has_value());
    CHECK(d->find_type(sr::schema_hash_v<rns::inner::MsgOrderV2>) != nullptr);
    CHECK(d->find_type(sr::schema_hash_v<rns_excluded::Lonely>) == nullptr);
}
