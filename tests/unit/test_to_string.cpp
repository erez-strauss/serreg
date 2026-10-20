// tests/unit/test_to_string.cpp — 2.2.1 (DECISIONS A-10): to_string / to_json / render for any serializable
// value, rendered from the process dictionary; calling them registers the type; the opt-in std::formatter.
#include "corpus.hpp"
#include "test_support.hpp"

#include <es/serreg/to_string.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <map>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace tostr
{
struct NeverSerialized
{
    std::int32_t a;
    std::uint8_t b;
    double c;
};
enum class Colour : std::uint8_t
{
    red = 1,
    green = 2
};
struct Nested
{
    NeverSerialized inner;
    Colour colour;
    std::vector<std::int16_t> ids;
    std::string name;
    std::map<std::string, std::int32_t> counts;
};
struct Printable
{
    std::int32_t x;
    std::int32_t y;
};
} // namespace tostr

template<> struct es::serreg::formattable<tostr::Printable> : std::true_type
{
};

TEST_CASE(
    "A to_string registers the type and renders the one-line text form; to_json the JSON value",
    "[to_string]")
{
    // no serialize() names NeverSerialized anywhere in this binary: the to_string<T> instantiation is what
    // registers it (statically, like a serialize call — so it is in the process dictionary before main)
    CHECK(tools::process_dictionary()->find_type(schema_hash_v<tostr::NeverSerialized>) != nullptr);
    const std::string s = to_string(tostr::NeverSerialized{7, 2, 1.5});
    CHECK(s == "tostr::NeverSerialized{a: 7, b: 2, c: 1.5}");
    CHECK(tools::process_dictionary()->find_type(schema_hash_v<tostr::NeverSerialized>) != nullptr);
    CHECK(to_json(tostr::NeverSerialized{7, 2, 1.5}) == "{\"a\":7,\"b\":2,\"c\":1.5}");

    CHECK(to_string(tostr::Colour::green) == "green");
    CHECK(to_json(tostr::Colour::green) ==
          "{\"name\":\"green\",\"value\":2}"); // srdump's enum object
    CHECK(to_string(std::int32_t{-5}) == "-5");
    CHECK(to_json(std::uint64_t{18446744073709551615ull}) ==
          "\"18446744073709551615\""); // > 2^53: a string
    CHECK(to_string(2.5) == "2.5");
    CHECK(to_string(true) == "true");
    CHECK(to_string("hi there") == "\"hi there\"");
    CHECK(to_json(std::string("q\"uote")) == "\"q\\\"uote\"");
}

TEST_CASE("A to_string: dynamic members, containers as values, the corpus types",
          "[to_string][dynamic]")
{
    tostr::Nested n{{1, 2, 3.0}, tostr::Colour::red, {10, 20}, "nm", {{"a", 1}, {"b", 2}}};
    const std::string s = to_string(n);
    // the dump's forms: a nested enum as `Type::name (value)`, a map as {key: value}
    CHECK(s == "tostr::Nested{inner: tostr::NeverSerialized{a: 1, b: 2, c: 3}, colour: "
               "tostr::Colour::red (1), "
               "ids: [10, 20], name: \"nm\", counts: {\"a\": 1, \"b\": 2}}");
    const std::string j = to_json(n);
    CHECK(j ==
          "{\"inner\":{\"a\":1,\"b\":2,\"c\":3},\"colour\":{\"name\":\"red\",\"value\":1},\"ids\":["
          "10,20],"
          "\"name\":\"nm\",\"counts\":[{\"key\":\"a\",\"value\":1},{\"key\":\"b\",\"value\":2}]}");
    // a container passed as the value: the transparent type renders as the value
    const std::vector<std::int32_t> v{1, 2, 3};
    CHECK(to_json(v) == "[1,2,3]");
    CHECK(to_string(v).find("[1, 2, 3]") != std::string::npos);
    // the corpus's Order, as print renders it
    CHECK(to_string(corpus::sample_order()).starts_with("corpus::Order{"));
    CHECK(to_json(corpus::sample_order())
              .starts_with("{\"id\":\"42\"")); // 64-bit integers are JSON strings (gk 1.12)
    // render() carries the error instead of embedding it
    const auto r = render(corpus::sample_order(), tools::format::json);
    REQUIRE(r.has_value());
    CHECK(*r == to_json(corpus::sample_order()));
}

TEST_CASE(
    "A formattable<T>: std::format and std::print render through to_string; {:j} is the JSON form",
    "[to_string][format]")
{
    STATIC_REQUIRE(formattable<tostr::Printable>::value);
    STATIC_REQUIRE(!formattable<tostr::Nested>::value);
    const tostr::Printable p{3, 4};
    CHECK(std::format("{}", p) == "tostr::Printable{x: 3, y: 4}");
    CHECK(std::format("{:j}", p) == "{\"x\":3,\"y\":4}");
    CHECK(std::format("at {} end", p) == "at tostr::Printable{x: 3, y: 4} end");
}
