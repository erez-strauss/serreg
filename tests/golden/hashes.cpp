// tests/golden/hashes.cpp — golden lock (spec §10.2 #1): schema/layout hashes (FR-H1/H4), canonical names
// (§6.2b), descriptor field tables, padding gap tables (FR-S6) and frame sizes (FR-S3) are frozen as
// STATIC_REQUIREs. Changing any value here is a wire-format change and needs a format/major bump + CHANGELOG note.
// The [golden-print] test prints the current values so a deliberate refreeze is a copy-paste.
//
// 2.1.7 (review G2-18): -DES_SERREG_GOLDEN_STANDALONE compiles this file without Catch2, for a second compiler
// (clang-p2996 in CI, Compiler Explorer): every STATIC_REQUIRE becomes a static_assert and each TEST_CASE an unused
// function, so `-fsyntax-only` passing means the other compiler computes every frozen value identically (FR-H4).
// Needs only include/ and tests/golden/ on the include path.
#include "corpus.hpp"

#if defined(ES_SERREG_GOLDEN_STANDALONE)
#define ES_GOLDEN_CAT2(a, b) a##b
#define ES_GOLDEN_CAT(a, b) ES_GOLDEN_CAT2(a, b)
#define TEST_CASE(...) [[maybe_unused]] static void ES_GOLDEN_CAT(es_golden_case_, __LINE__)()
#define STATIC_REQUIRE(...) static_assert(__VA_ARGS__)
#define SUCCEED() static_cast<void>(0)
#else
#include <catch2/catch_test_macros.hpp>
#endif

#include <cstdint>
#include <deque>
#include <flat_map>
#include <forward_list>
#include <list>
#include <map>
#include <print>
#include <set>
#include <stack>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;
using namespace std::string_view_literals;

// ---- hash freeze (FR-H1: schema hash; FR-H4: layout hash) --------------------------------------------------
#define ES_GOLDEN(T, schema, layout)                                                               \
    STATIC_REQUIRE(schema_hash_v<T> == schema##ull);                                               \
    STATIC_REQUIRE(layout_hash_v<T> == layout##ull)

TEST_CASE("FR-H1/FR-H4 golden schema and layout hashes", "[golden]")
{
    // refrozen for format 1.3 / XXH3-64 (2.1.0) from GCC 16.2 -std=c++26 -freflection, x86-64
    ES_GOLDEN(corpus::Scalars, 0x0773476d2708557f, 0xb5bbf863d02230ef);
    ES_GOLDEN(corpus::Padded, 0x45d0212145051ee5, 0xefa34e33c5394e5a);
    ES_GOLDEN(corpus::Nested, 0x6aefae2ed0329a88, 0xf2451b660cc97379);
    ES_GOLDEN(corpus::Base, 0xc83d6ca60fea9cd8, 0x077aac8f71faa349);
    ES_GOLDEN(corpus::Tag2, 0x1bb7a3b344a42c01, 0x8bcafb69a4d4c365);
    ES_GOLDEN(corpus::Derived, 0xa8dc6824ca907ee1, 0x1f656e4e6820dd87);
    ES_GOLDEN(corpus::MultiDerived, 0x12ff4ce6dc11b75d, 0xe4c99473004f2508);
    ES_GOLDEN(corpus::Side, 0x074e267cca5188e1, 0xfdccdf0b5609277b);
    ES_GOLDEN(corpus::Venue, 0xe513f0b42c4e8f45, 0xcd97d07ac5e40b4a);
    ES_GOLDEN(corpus::Arrays, 0xaca08ad8860aa982, 0x41cbe109607026f5);
    ES_GOLDEN(corpus::WithEmpty, 0x98e7b2625c6dcadc, 0x4e0a9e986f2d5b5e);
    ES_GOLDEN(corpus::Account, 0xdde516232a3664d8, 0x27d87c311fb508f8);
    ES_GOLDEN(corpus::Versioned, 0x899e36549dfabd4c, 0x52b37c2c9b2f54f2);
    ES_GOLDEN(corpus::Unversioned, 0x23599b0163a6b6a7, 0x52b37c2c9b2f54f2);
    ES_GOLDEN(corpus::Skipped, 0x45d0763cd035e303, 0xc5187469d517b843);
    ES_GOLDEN(corpus::Vec3, 0x7fdb040866c570e1, 0xad3ceb2af17cbb33);
    ES_GOLDEN(corpus::Vec3i, 0xa0c0848b2bfff86e, 0x31820848c4e244b6);
    ES_GOLDEN(corpus::Empty, 0x2a2c52405ef9708b, 0x3ba1f9a330b68a73);
    ES_GOLDEN(corpus::Stamp, 0x04f563c942cb922f, 0x4dc4867df2341e86);
    ES_GOLDEN(corpus::StampFlat, 0x04f563c942cb922f, 0x4dc4867df2341e86);
    ES_GOLDEN(corpus::Order, 0x367f8cd8c6441dd2, 0x0661bb3e388c01fe);
    ES_GOLDEN(corpus::Cancel, 0x35af6843b770c519, 0x99c125fd4fe8673c);
    ES_GOLDEN(corpus::Point2i, 0x78baef33198c1282, 0x50e920f7a4899338);
    ES_GOLDEN(corpus::Pair2i, 0xe33c653e918e7443, 0x50e920f7a4899338);
    // 2.1.14 (format 1.4): the anonymous-namespace type is named explicitly, "corpus::Local" (was "{anon}::Local")
    ES_GOLDEN(Local, 0x4e2c01f6b569c2ed, 0x6333a3cc649863d7);
    // v1.0.2 (cc F3): value template arguments — added, frozen on first print; the layout hash is shared (same body)
    ES_GOLDEN(t::WBool, 0x3739a1409b708988, 0x26dc44c15cd38ea0);
    ES_GOLDEN(t::WChar, 0xf8fac147067be6fd, 0x26dc44c15cd38ea0);
    ES_GOLDEN(t::WEnum, 0x202ee12828f6423c, 0x26dc44c15cd38ea0);
    ES_GOLDEN(t::WNeg, 0x1bb677b7ffc98ec4, 0x26dc44c15cd38ea0);
    ES_GOLDEN(t::Mode, 0x2365a1001785cb81, 0xfdccdf0b5609277b);
}

// ---- canonical names (§6.2b) -------------------------------------------------------------------------------
TEST_CASE("6.2b canonical names", "[golden]")
{
    STATIC_REQUIRE(canonical_name_v<corpus::Order> == "corpus::Order"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Side> == "corpus::Side"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Venue> == "corpus::Venue"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::MultiDerived> == "corpus::MultiDerived"sv);
    STATIC_REQUIRE(canonical_name_v<Local> ==
                   "corpus::Local"sv); // 2.1.14: explicit name (no linkage)
    STATIC_REQUIRE(canonical_name_v<corpus::Vec3> == "corpus::Vec<corpus::Side,3>"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Vec3i> == "corpus::Vec<i32,3>"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Stamp> ==
                   "corpus::StampFlat"sv); // as_flat: the mapped type's name
    STATIC_REQUIRE(canonical_name_v<corpus::Account> == "corpus::Account"sv);
    // cc F3: template-argument *values* never use a compiler spelling (§6.2b tightened in v1.0.2)
    STATIC_REQUIRE(canonical_name_v<t::WBool> == "t::W<i32,true>"sv);
    STATIC_REQUIRE(canonical_name_v<t::WChar> == "t::W<i32,120>"sv);
    STATIC_REQUIRE(canonical_name_v<t::WEnum> == "t::W<i32,enum(7)>"sv);
    STATIC_REQUIRE(canonical_name_v<t::WNeg> == "t::W<i32,-5>"sv);
    STATIC_REQUIRE(fingerprint64("struct<t::W<i32,-5>>[4,4]{v:i32@0[4,4];}") ==
                   schema_hash_v<t::WNeg>);
    STATIC_REQUIRE(layout_hash_v<t::WBool> ==
                   layout_hash_v<t::WNeg>); // the argument enters the schema string only

    // 2.1.14 (review C-03): nested in a class-template specialisation, named through the parent's arguments
    STATIC_REQUIRE(canonical_name_v<corpus::KeyOrder> == "corpus::Keyed<corpus::Order>::Key"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::KeyCancel> == "corpus::Keyed<corpus::Cancel>::Key"sv);
    STATIC_REQUIRE(layout_hash_v<corpus::KeyOrder> == layout_hash_v<corpus::KeyCancel>);
    STATIC_REQUIRE(schema_hash_v<corpus::KeyOrder> != schema_hash_v<corpus::KeyCancel>);
    // 2.1.14 (review C-04): the std vocabulary — no inline namespace, no default argument, no allocator
    STATIC_REQUIRE(canonical_name_v<corpus::BoxString> == "corpus::Box<std::string>"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::BoxPmrString> == "corpus::Box<std::string>"sv);
    STATIC_REQUIRE(schema_hash_v<corpus::BoxString> == schema_hash_v<corpus::BoxPmrString>);
    STATIC_REQUIRE(canonical_name_v<corpus::BoxVector> == "corpus::Box<std::vector<i32>>"sv);
    STATIC_REQUIRE(canonical_name_v<std::string> == "std::string"sv);
    STATIC_REQUIRE(canonical_name_v<std::monostate> == "std::monostate"sv);
    STATIC_REQUIRE(canonical_name_v<std::optional<corpus::Side>> ==
                   "std::optional<corpus::Side>"sv);
    STATIC_REQUIRE(canonical_name_v<std::array<std::uint8_t, 4>> == "std::array<u8,4>"sv);
    STATIC_REQUIRE(canonical_name_v<std::variant<std::int32_t, std::string>> ==
                   "std::variant<i32,std::string>"sv);
    STATIC_REQUIRE(canonical_name_v<std::pair<std::int32_t, double>> == "std::pair<i32,f64>"sv);
    STATIC_REQUIRE(canonical_name_v<std::tuple<std::int8_t, corpus::Order>> ==
                   "std::tuple<i8,corpus::Order>"sv);
    STATIC_REQUIRE(canonical_name_v<std::chrono::milliseconds> ==
                   "std::chrono::duration<i64,std::ratio<1,1000>>"sv);
    // 2.1.14: a user inline namespace stays; explicit names (type_properties, annotation) win and nest
    STATIC_REQUIRE(canonical_name_v<corpus::InV1> == "corpus::v1::InV1"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Renamed> == "corpus::Legacy"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Annotated> == "corpus::Alias"sv);
    STATIC_REQUIRE(canonical_name_v<corpus::Annotated::Part> == "corpus::Alias::Part"sv);
}

// ---- descriptor tables: field counts, offsets, kinds --------------------------------------------------------
TEST_CASE("6.1 descriptor field tables", "[golden]")
{
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.fields.size() == 4);
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.size == 24 &&
                   descriptor_v<corpus::Scalars>.align == 8);
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.fields[0].name == "i"sv &&
                   descriptor_v<corpus::Scalars>.fields[0].k == kind::i64);
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.fields[1].offset == 8 &&
                   descriptor_v<corpus::Scalars>.fields[1].k == kind::f64);
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.fields[3].offset == 20 &&
                   descriptor_v<corpus::Scalars>.fields[3].k == kind::i32);

    STATIC_REQUIRE(descriptor_v<corpus::Padded>.fields.size() == 3);
    STATIC_REQUIRE(descriptor_v<corpus::Padded>.fields[1].offset == 8 &&
                   descriptor_v<corpus::Padded>.fields[2].offset == 16);

    STATIC_REQUIRE(descriptor_v<corpus::Nested>.fields.size() == 3);
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.fields[0].k == kind::struct_);
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.fields[0].type_hash ==
                   schema_hash_v<corpus::Scalars>);
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.fields[1].type_hash ==
                   schema_hash_v<corpus::Padded>);
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.fields[1].offset == 24 &&
                   descriptor_v<corpus::Nested>.fields[2].offset == 48);

    // bases come first, as "<base:Name>" pseudo-fields (§6.1b)
    STATIC_REQUIRE(descriptor_v<corpus::Derived>.fields.size() == 2);
    STATIC_REQUIRE(descriptor_v<corpus::Derived>.fields[0].name == "<base:corpus::Base>"sv);
    STATIC_REQUIRE(descriptor_v<corpus::Derived>.fields[0].type_hash ==
                   schema_hash_v<corpus::Base>);
    STATIC_REQUIRE(descriptor_v<corpus::MultiDerived>.fields.size() == 3);
    STATIC_REQUIRE(descriptor_v<corpus::MultiDerived>.fields[1].name == "<base:corpus::Tag2>"sv);
    STATIC_REQUIRE(descriptor_v<corpus::MultiDerived>.fields[1].offset == 4);
    STATIC_REQUIRE(descriptor_v<corpus::MultiDerived>.fields[2].name == "k"sv &&
                   descriptor_v<corpus::MultiDerived>.fields[2].offset == 6);

    // enums
    STATIC_REQUIRE(descriptor_v<corpus::Side>.k == kind::enum_ &&
                   descriptor_v<corpus::Side>.underlying == kind::u8);
    STATIC_REQUIRE(descriptor_v<corpus::Side>.enumerators.size() == 2);
    STATIC_REQUIRE(descriptor_v<corpus::Side>.enumerators[1].name == "sell"sv &&
                   descriptor_v<corpus::Side>.enumerators[1].value == 2);
    STATIC_REQUIRE(descriptor_v<corpus::Venue>.underlying == kind::i32 &&
                   descriptor_v<corpus::Venue>.size == 4);
    STATIC_REQUIRE(descriptor_v<corpus::Venue>.enumerators.size() == 3 &&
                   descriptor_v<corpus::Venue>.enumerators[2].value == 6);

    // arrays: C array, std::array, char[8], 2-D (element descriptor chain)
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields.size() == 4);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[0].k == kind::array_ &&
                   descriptor_v<corpus::Arrays>.fields[0].extent == 4);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[0].elem->k == kind::i32);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[1].extent == 2 &&
                   descriptor_v<corpus::Arrays>.fields[1].elem->k == kind::f64);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[2].name == "sym"sv &&
                   descriptor_v<corpus::Arrays>.fields[2].extent == 8);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[2].elem->k == kind::char_);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[3].extent == 2 &&
                   descriptor_v<corpus::Arrays>.fields[3].elem->k == kind::array_);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.fields[3].elem->extent == 3 &&
                   descriptor_v<corpus::Arrays>.fields[3].elem->elem->k == kind::u8);

    // [[no_unique_address]] empty member shares offset 0 with the next member
    STATIC_REQUIRE(descriptor_v<corpus::WithEmpty>.fields.size() == 2 &&
                   descriptor_v<corpus::WithEmpty>.size == 4);
    STATIC_REQUIRE(descriptor_v<corpus::WithEmpty>.fields[0].offset == 0 &&
                   descriptor_v<corpus::WithEmpty>.fields[1].offset == 0);
    STATIC_REQUIRE(descriptor_v<corpus::Empty>.fields.size() == 0 &&
                   descriptor_v<corpus::Empty>.size == 1);
    STATIC_REQUIRE(canonical_name_v<corpus::Empty> == "corpus::Empty"sv);
    STATIC_REQUIRE(descriptor_v<corpus::WithEmpty>.fields[0].type_hash ==
                   schema_hash_v<corpus::Empty>);

    // private members are described (FR-S9)
    STATIC_REQUIRE(descriptor_v<corpus::Account>.fields.size() == 2);
    STATIC_REQUIRE(descriptor_v<corpus::Account>.fields[0].name == "id_"sv &&
                   descriptor_v<corpus::Account>.fields[1].name == "balance_"sv);

    // annotations (FR-S11): on members and on the type
    STATIC_REQUIRE(descriptor_v<corpus::Versioned>.since == 1 &&
                   descriptor_v<corpus::Versioned>.deprecated == 0);
    STATIC_REQUIRE(descriptor_v<corpus::Versioned>.fields[1].since == 3 &&
                   descriptor_v<corpus::Versioned>.fields[1].deprecated == 0);
    STATIC_REQUIRE(descriptor_v<corpus::Versioned>.fields[2].since == 0 &&
                   descriptor_v<corpus::Versioned>.fields[2].deprecated == 5);
    STATIC_REQUIRE(descriptor_v<corpus::Unversioned>.since == 0);

    // skip: the member is absent from the table, the type keeps its native size
    STATIC_REQUIRE(descriptor_v<corpus::Skipped>.fields.size() == 2 &&
                   descriptor_v<corpus::Skipped>.size == 24);
    STATIC_REQUIRE(descriptor_v<corpus::Skipped>.fields[1].name == "n"sv &&
                   descriptor_v<corpus::Skipped>.fields[1].offset == 16);

    // template specialization and as_flat
    STATIC_REQUIRE(descriptor_v<corpus::Vec3>.fields.size() == 1 &&
                   descriptor_v<corpus::Vec3>.fields[0].extent == 3);
    STATIC_REQUIRE(descriptor_v<corpus::Vec3i>.fields[0].elem->k == kind::i32 &&
                   descriptor_v<corpus::Vec3i>.size == 12);
    STATIC_REQUIRE(descriptor_v<corpus::Vec3>.fields[0].elem->k == kind::enum_ &&
                   descriptor_v<corpus::Vec3>.fields[0].elem->type_hash ==
                       schema_hash_v<corpus::Side>);
    STATIC_REQUIRE(descriptor_v<corpus::Stamp>.fields.size() == 1 &&
                   descriptor_v<corpus::Stamp>.fields[0].k == kind::i64);
    STATIC_REQUIRE(schema_hash_v<corpus::Stamp> == schema_hash_v<corpus::StampFlat>);
}

// ---- padding gap tables (FR-S6) ----------------------------------------------------------------------------
TEST_CASE("FR-S6 padding gap tables", "[golden]")
{
    STATIC_REQUIRE(descriptor_v<corpus::Scalars>.padding_gaps.size() == 0);
    STATIC_REQUIRE(descriptor_v<corpus::Padded>.padding_gaps.size() == 2);
    STATIC_REQUIRE(descriptor_v<corpus::Padded>.padding_gaps[0].offset == 1 &&
                   descriptor_v<corpus::Padded>.padding_gaps[0].len == 7);
    STATIC_REQUIRE(descriptor_v<corpus::Padded>.padding_gaps[1].offset == 18 &&
                   descriptor_v<corpus::Padded>.padding_gaps[1].len == 6);
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.padding_gaps.size() ==
                   3); // nested gaps are flattened
    STATIC_REQUIRE(descriptor_v<corpus::Nested>.padding_gaps[0].offset == 25 &&
                   descriptor_v<corpus::Nested>.padding_gaps[2].offset == 52);
    STATIC_REQUIRE(descriptor_v<corpus::MultiDerived>.padding_gaps.size() == 1 &&
                   descriptor_v<corpus::MultiDerived>.padding_gaps[0].offset == 7);
    STATIC_REQUIRE(descriptor_v<corpus::Order>.padding_gaps.size() == 1);
    STATIC_REQUIRE(descriptor_v<corpus::Order>.padding_gaps[0].offset == 29 &&
                   descriptor_v<corpus::Order>.padding_gaps[0].len == 3);
    STATIC_REQUIRE(descriptor_v<corpus::Skipped>.padding_gaps.size() ==
                   2); // skipped member counts as a gap
    STATIC_REQUIRE(descriptor_v<corpus::Skipped>.padding_gaps[0].offset == 8 &&
                   descriptor_v<corpus::Skipped>.padding_gaps[0].len == 8);
    STATIC_REQUIRE(descriptor_v<corpus::Arrays>.padding_gaps.size() == 1 &&
                   descriptor_v<corpus::Arrays>.padding_gaps[0].offset == 46);
}

// ---- frame sizes (FR-S3): 16-byte header + 8-aligned records ----------------------------------------------
TEST_CASE("FR-S3 frame sizes", "[golden]")
{
    // format 1.1 (FR-L6): a literal record has no bytes in a compact frame (was 8 in 1.0)
    STATIC_REQUIRE(frame_size<decltype("evt"_lit), corpus::Order>() == 16 + 0 + 32);
    STATIC_REQUIRE(frame_size<corpus::Cancel, decltype("x"_lit), corpus::Side>() ==
                   16 + 16 + 0 + 8);
    STATIC_REQUIRE(frame_size<decltype("evt"_lit), corpus::Order>(
                       serialize_options{.full_records = true}) == 16 + (16 + 8) + (16 + 32));
    STATIC_REQUIRE(frame_size<corpus::Side>() == 16 + 8);
    STATIC_REQUIRE(frame_size<>() == 16);
    STATIC_REQUIRE(frame_size<decltype("heartbeat"_lit)>() == 16);
    // 1.1 scalar / string arguments (FR-A1/A2): scalars are their size padded to 8, a string is at least its
    // u32 length prefix (frame_size<>() is the minimum when a string is present)
    STATIC_REQUIRE(frame_size<int, double, bool>() == 16 + 8 + 8 + 8);
    STATIC_REQUIRE(frame_size<std::string_view>() == 16 + 8);
    STATIC_REQUIRE(frame_size<const char*, int>(serialize_options{.full_records = true}) ==
                   16 + (16 + 8) + (16 + 8));
}

// ---- 1.1 call strings and hashes (FR-A1/A2, FR-P1): frozen -------------------------------------------------
TEST_CASE("1.1 argument categories and print forms", "[golden]")
{
    // category names, not C++ type names: every string-like type is `string`, scalars use the kind names
    STATIC_REQUIRE(arg_hash_v<const char*> == fingerprint64("string"));
    STATIC_REQUIRE(arg_hash_v<std::string> == fingerprint64("string"));
    STATIC_REQUIRE(arg_hash_v<std::string_view> == fingerprint64("string"));
    STATIC_REQUIRE(arg_hash_v<char[4]> == fingerprint64("string"));
    STATIC_REQUIRE(arg_hash_v<int> == fingerprint64("i32"));
    STATIC_REQUIRE(arg_hash_v<std::uint64_t> == fingerprint64("u64"));
    STATIC_REQUIRE(arg_hash_v<double> == fingerprint64("f64"));
    STATIC_REQUIRE(arg_hash_v<bool> == fingerprint64("bool"));
    STATIC_REQUIRE(arg_hash_v<decltype("evt"_lit)> == fingerprint64("evt"));
    STATIC_REQUIRE(arg_hash_v<corpus::Order> == schema_hash_v<corpus::Order>);
    STATIC_REQUIRE(arg_kind_v<const char*> == fmt::arg_string);
    STATIC_REQUIRE(arg_kind_v<int> == fmt::arg_scalar);
    STATIC_REQUIRE(arg_kind_v<decltype("evt"_lit)> == fmt::arg_literal);
    STATIC_REQUIRE(arg_kind_v<corpus::Order> == fmt::arg_type);
    // 1.5: a compile-time constant is its own category; its ARG hash is that of the value it stands for
    STATIC_REQUIRE(arg_kind_v<decltype(42_)> == fmt::arg_const);
    STATIC_REQUIRE(arg_hash_v<decltype(42_)> == arg_hash_v<int>);
    STATIC_REQUIRE(arg_hash_v<constant<corpus::Side::buy>> == schema_hash_v<corpus::Side>);
    // the same call whatever the string/scalar spelling on either side
    STATIC_REQUIRE(call_hash_v<const char*, int> == call_hash_v<std::string, std::int32_t>);
    STATIC_REQUIRE(call_hash_v<const char*, int> == call_hash_v<std::string_view&, int&>);
    // print/println are distinct forms of the same argument list (call strings "print(" / "println(")
    STATIC_REQUIRE(
        print_call_hash_v<"hello {} {} {}", const char*, int, decltype("worlds"_lit)> !=
        call_hash_v<decltype("hello {} {} {}"_lit), const char*, int, decltype("worlds"_lit)>);
    STATIC_REQUIRE(print_call_hash_v<"x {}", int> != println_call_hash_v<"x {}", int>);
    // frozen values (format 1.1, GCC 16.2, x86-64) — these lock the call strings:
    //   call(2){string,i32}   print(2){lit:0x61127b53167a9c45,i32}   println(2){lit:0x61127b53167a9c45,i32}
    STATIC_REQUIRE(fingerprint64("string") == 0xd8ad194648e774f1ull);
    STATIC_REQUIRE(call_hash_v<const char*, int> == 0xef7e3878972feec1ull);
    STATIC_REQUIRE(print_call_hash_v<"x {}", int> == 0x062ff32816c78e7dull);
    STATIC_REQUIRE(println_call_hash_v<"x {}", int> == 0x90a1eb79c6e2eda5ull);
    //   print(4){lit:0xc2731e47aa1af9a5,string,i32,lit:0xe2776d7d7e4f5780}  — the plan §3 example frame
    STATIC_REQUIRE(print_call_hash_v<"hello {} {} {}", const char*, int, decltype("worlds"_lit)> ==
                   0xe97f49911b00565eull);
    STATIC_REQUIRE(frame_size<decltype("hello {} {} {}"_lit), const char*, int,
                              decltype("worlds"_lit)>() == 16 + 0 + 8 + 8 + 0); // 32 with "new"
}

// ---- 2.0 dynamic types (docs/plans-for-v2.0.md §2, §5): canonical images, frozen -----------------------------
TEST_CASE("2.0 dynamic types: canonical strings, hashes, fixed layouts", "[golden][dynamic]")
{
    STATIC_REQUIRE(dynamic_type<corpus::DynMsg>);
    STATIC_REQUIRE(!flat_type<corpus::DynMsg>);
    STATIC_REQUIRE(flat_type<corpus::Order>);
    // fixed parts (plan §2 rule): DynMsg id@0[4] name slot@4[8] xs slot@12[8] → 20, align 4
    STATIC_REQUIRE(descriptor_v<corpus::DynMsg>.size == 20 &&
                   descriptor_v<corpus::DynMsg>.align == 4);
    STATIC_REQUIRE(!descriptor_v<corpus::DynMsg>.is_flat);
    STATIC_REQUIRE(descriptor_v<corpus::DynMsg>.fields[1].offset == 4 &&
                   descriptor_v<corpus::DynMsg>.fields[1].size == 8 &&
                   descriptor_v<corpus::DynMsg>.fields[1].k == kind::string_);
    STATIC_REQUIRE(descriptor_v<corpus::DynMsg>.fields[2].k == kind::vector_ &&
                   descriptor_v<corpus::DynMsg>.fields[2].elem->k == kind::i16);
    STATIC_REQUIRE(descriptor_v<corpus::DynNested>.size == 24);
    STATIC_REQUIRE(descriptor_v<corpus::DynNested>.fields[2].k == kind::variant_ &&
                   descriptor_v<corpus::DynNested>.fields[2].extent == 3);
    STATIC_REQUIRE(descriptor_v<corpus::DynArr>.size == 40);
    STATIC_REQUIRE(descriptor_v<corpus::DynArr>.fields[0].k == kind::array_ &&
                   descriptor_v<corpus::DynArr>.fields[0].size == 16); // 2 × string slot
    STATIC_REQUIRE(descriptor_v<corpus::DynBase>.size == 40 &&
                   descriptor_v<corpus::DynBase>.align == 8);
    STATIC_REQUIRE(descriptor_v<corpus::DynBase>.fields[0].offset == 0 &&
                   descriptor_v<corpus::DynBase>.fields[0].size == sizeof(corpus::Order));
    // frozen (GCC 16.2, x86-64, format 1.2). Schema strings, for the record:
    //   dyn<corpus::DynMsg>[20,4]{id:u32@0[4,4];name:string@4[8,4];xs:vector<i16>@12[8,4];}
    //   dyn<corpus::DynNested>[24,4]{msgs:vector<dyn<0x33ea…>>@0[8,4];maybe:optional<struct<0x6268…>>@8[8,4];
    //                                v:variant<i32|struct<0x6268…>|string>@16[8,4];}
    //   dyn<corpus::DynArr>[40,4]{names:array<string,2>@0[16,4];tags:vector<string>@16[8,4];
    //                             side:optional<enum<corpus::Side,u8>>@24[8,4];maybe_msg:variant<struct<0x9e80…>|dyn<0x33ea…>>@32[8,4];}
    //   dyn<corpus::DynBase>[40,8]{<base:corpus::Order>:struct<0x6268…>@0[32,8];note:string@32[8,4];}
    STATIC_REQUIRE(schema_hash_v<corpus::DynMsg> == 0xb487de0c417a37e8ull);
    STATIC_REQUIRE(layout_hash_v<corpus::DynMsg> == 0x7835cb0574f155daull);
    STATIC_REQUIRE(schema_hash_v<corpus::DynNested> == 0xf4d2426d5a5d76dfull);
    STATIC_REQUIRE(layout_hash_v<corpus::DynNested> == 0x81a9968392688e75ull);
    STATIC_REQUIRE(schema_hash_v<corpus::DynArr> == 0x9032db9ee55d1318ull);
    STATIC_REQUIRE(layout_hash_v<corpus::DynArr> == 0xab9b51e9c98b131dull);
    STATIC_REQUIRE(schema_hash_v<corpus::DynBase> == 0x6d367348e3549641ull);
    STATIC_REQUIRE(layout_hash_v<corpus::DynBase> == 0xb36da7cca7cdd27aull);
    STATIC_REQUIRE(schema_hash_v<std::monostate> == 0xe3f5041aaaa18c06ull); // a variant alternative
    // a dynamic type's record is u32 len, u32 0, image: frame_size<>() is the minimum
    STATIC_REQUIRE(frame_size<corpus::DynMsg>() == 16 + 8);
    STATIC_REQUIRE(has_runtime_size_v<corpus::DynMsg>);
}

// ---- relations between hashes ------------------------------------------------------------------------------
TEST_CASE("FR-H4/FR-S11 hash relations", "[golden]")
{
    // Same layout, different member names: layout hash equal, schema hash different (FR-H4).
    STATIC_REQUIRE(layout_hash_v<corpus::Point2i> == layout_hash_v<corpus::Pair2i>);
    STATIC_REQUIRE(schema_hash_v<corpus::Point2i> != schema_hash_v<corpus::Pair2i>);
    // Same layout, different since/deprecated annotations: layout hash equal, schema hash different (FR-S11: annotations
    // enter the schema string only, never the layout string).
    STATIC_REQUIRE(layout_hash_v<corpus::Versioned> == layout_hash_v<corpus::Unversioned>);
    STATIC_REQUIRE(schema_hash_v<corpus::Versioned> != schema_hash_v<corpus::Unversioned>);
    // Call hashes depend on argument order and literal-ness.
    STATIC_REQUIRE(call_hash_v<decltype("evt"_lit), corpus::Order> !=
                   call_hash_v<corpus::Order, decltype("evt"_lit)>);
    STATIC_REQUIRE(call_hash_v<corpus::Order> != call_hash_v<corpus::Cancel>);
    // Literal ids are the XXH3-64 fingerprint of the text (FR-H7).
    STATIC_REQUIRE(decltype("evt"_lit)::id == fingerprint64("evt"));
}

// ---- printer -----------------------------------------------------------------------------------------------
namespace
{

template<class T> void print_one(const char* const label)
{
    const type_desc& d = descriptor_v<T>;
    std::println(
        "{:<14} {:<28} schema=0x{:016x} layout=0x{:016x} size={} align={} fields={} gaps={}", label,
        d.name, d.schema_hash, d.layout_hash, d.size, d.align, d.fields.size(),
        d.padding_gaps.size());
    for (const auto& f : d.fields)
        std::println(
            "    {:<22} off={:<3} size={:<3} align={:<2} kind={:<6} since={} dep={} extent={}",
            f.name, f.offset, f.size, f.align, to_string(f.k), f.since, f.deprecated, f.extent);
    for (const auto& e : d.enumerators)
        std::println("    {} = {}", e.name, e.value);
    for (const auto& g : d.padding_gaps)
        std::println("    gap {}+{}", g.offset, g.len);
}

} // namespace

TEST_CASE("golden corpus table", "[golden-print]")
{
    print_one<corpus::Scalars>("Scalars");
    print_one<corpus::Padded>("Padded");
    print_one<corpus::Nested>("Nested");
    print_one<corpus::Base>("Base");
    print_one<corpus::Tag2>("Tag2");
    print_one<corpus::Derived>("Derived");
    print_one<corpus::MultiDerived>("MultiDerived");
    print_one<corpus::Side>("Side");
    print_one<corpus::Venue>("Venue");
    print_one<corpus::Arrays>("Arrays");
    print_one<corpus::Empty>("Empty");
    print_one<corpus::WithEmpty>("WithEmpty");
    print_one<corpus::Account>("Account");
    print_one<corpus::Versioned>("Versioned");
    print_one<corpus::Unversioned>("Unversioned");
    print_one<corpus::Skipped>("Skipped");
    print_one<corpus::Vec3>("Vec3");
    print_one<corpus::Vec3i>("Vec3i");
    print_one<corpus::Stamp>("Stamp");
    print_one<corpus::Order>("Order");
    print_one<corpus::Cancel>("Cancel");
    print_one<corpus::Point2i>("Point2i");
    print_one<corpus::Pair2i>("Pair2i");
    print_one<Local>("Local");
    print_one<t::WBool>("WBool");
    print_one<t::WChar>("WChar");
    print_one<t::WEnum>("WEnum");
    print_one<t::WNeg>("WNeg");
    print_one<t::Mode>("Mode");
    SUCCEED();
}

TEST_CASE("FR-H1 schema hash is XXH3-64 of the canonical schema string", "[golden]")
{
    // Independent recomputation of one canonical string (spec §6.2) guards against silent format drift.
    constexpr std::string_view point = "struct<corpus::Point2i>[8,4]{x:i32@0[4,4];y:i32@4[4,4];}";
    STATIC_REQUIRE(fingerprint64(point) == schema_hash_v<corpus::Point2i>);
    constexpr std::string_view point_layout = "struct[8,4]{i32@0[4,4];i32@4[4,4];}";
    STATIC_REQUIRE(fingerprint64(point_layout) == layout_hash_v<corpus::Point2i>);
    constexpr std::string_view side = "enum<corpus::Side,u8>[1,1]{buy=1;sell=2;}";
    STATIC_REQUIRE(fingerprint64(side) == schema_hash_v<corpus::Side>);
    constexpr std::string_view versioned = "struct<corpus::Versioned>[12,4;s1;d0]{id:u32@0[4,4];"
                                           "venue:u32@4[4,4;s3;d0];legacy:u32@8[4,4;s0;d5];}";
    STATIC_REQUIRE(fingerprint64(versioned) == schema_hash_v<corpus::Versioned>);
    constexpr std::string_view vec3i = "struct<corpus::Vec<i32,3>>[12,4]{v:array<i32,3>@0[12,4];}";
    STATIC_REQUIRE(fingerprint64(vec3i) == schema_hash_v<corpus::Vec3i>);
    constexpr std::string_view empty = "struct<corpus::Empty>[1,1]{}";
    STATIC_REQUIRE(fingerprint64(empty) == schema_hash_v<corpus::Empty>);
}

// ---- 2.1.15, format 1.5: compile-time constants in the call signature (decision O15) --------------------
// The call strings are spelled out and their hashes were computed outside the library (XXH3-64 of the string,
// Python xxhash), so a change of either the spelling or the hash is caught here.
TEST_CASE("FR-A5 the numeric _ literal and k<V> build constant<V>", "[golden][const]")
{
    STATIC_REQUIRE(std::is_same_v<decltype(42_), constant<42>>);
    STATIC_REQUIRE(std::is_same_v<decltype(0_), constant<0>>);
    STATIC_REQUIRE(std::is_same_v<decltype(-7_), constant<-7>>);
    STATIC_REQUIRE(std::is_same_v<decltype(+7_), constant<7>>);
    STATIC_REQUIRE(std::is_same_v<decltype(0x10_), constant<16>>);
    STATIC_REQUIRE(std::is_same_v<decltype(0b101_), constant<5>>);
    STATIC_REQUIRE(std::is_same_v<decltype(017_), constant<15>>);
    STATIC_REQUIRE(std::is_same_v<decltype(1'000'000_), constant<1000000>>);
    // int, else long long, else unsigned long long
    STATIC_REQUIRE(std::is_same_v<decltype(2147483647_), constant<2147483647>>);
    STATIC_REQUIRE(std::is_same_v<decltype(2147483648_), constant<2147483648ll>>);
    STATIC_REQUIRE(std::is_same_v<decltype(-2147483648_), constant<-2147483648ll>>);
    STATIC_REQUIRE(std::is_same_v<decltype(5'000'000'000_), constant<5000000000ll>>);
    STATIC_REQUIRE(
        std::is_same_v<decltype(18'446'744'073'709'551'615_), constant<18446744073709551615ull>>);
    // floating literals are doubles with the value the compiler gives the same spelling
    STATIC_REQUIRE(std::is_same_v<decltype(1.5_), constant<1.5>>);
    STATIC_REQUIRE(std::is_same_v<decltype(-1.5_), constant<-1.5>>);
    STATIC_REQUIRE(std::is_same_v<decltype(0.1_), constant<0.1>>);
    STATIC_REQUIRE(std::is_same_v<decltype(1e3_), constant<1000.0>>);
    STATIC_REQUIRE(std::is_same_v<decltype(2.5e-3_), constant<2.5e-3>>);
    STATIC_REQUIRE(std::is_same_v<decltype(123456.789012345_), constant<123456.789012345>>);
    STATIC_REQUIRE(std::is_same_v<decltype(1.e2_), constant<100.0>>);
    STATIC_REQUIRE(std::is_same_v<decltype(6.02214076e22_), constant<6.02214076e22>>);
    STATIC_REQUIRE(std::is_same_v<decltype(1.6e-19_), constant<1.6e-19>>);
    // the string form is untouched
    STATIC_REQUIRE(std::is_same_v<decltype("42"_), lit<"42">>);
    // k<V>: the types a literal cannot spell
    STATIC_REQUIRE(std::is_same_v<std::remove_const_t<decltype(k<true>)>, constant<true>>);
    STATIC_REQUIRE(std::is_same_v<constant<42u>::value_type, unsigned>);
    STATIC_REQUIRE(std::is_same_v<constant<corpus::Side::buy>::value_type, corpus::Side>);
    STATIC_REQUIRE(detail::serializable_arg<constant<42>>);
    STATIC_REQUIRE(detail::serializable_arg<constant<corpus::Side::buy>>);
    STATIC_REQUIRE(detail::serializable_arg<constant<std::byte{7}>>);
}

TEST_CASE("FR-A5 a constant's type and value are in the call string and the call hash",
          "[golden][const]")
{
    using namespace std::string_view_literals;
    STATIC_REQUIRE(detail::call_string<constant<42>>() == "call(1){const:i32=42}"sv);
    STATIC_REQUIRE(detail::call_string<constant<-7>>() == "call(1){const:i32=-7}"sv);
    STATIC_REQUIRE(detail::call_string<constant<1.5>>() ==
                   "call(1){const:f64=0x3ff8000000000000}"sv);
    STATIC_REQUIRE(detail::call_string<constant<1.5f>>() ==
                   "call(1){const:f32=0x000000003fc00000}"sv);
    STATIC_REQUIRE(detail::call_string<constant<true>>() == "call(1){const:bool=true}"sv);
    STATIC_REQUIRE(detail::call_string<constant<5000000000ll>>() ==
                   "call(1){const:i64=5000000000}"sv);
    STATIC_REQUIRE(detail::call_string<constant<42u>>() == "call(1){const:u32=42}"sv);
    STATIC_REQUIRE(detail::call_string<constant<'x'>>() == "call(1){const:char=120}"sv);
    STATIC_REQUIRE(call_hash_v<constant<42>> == 0x3834d2cf9a6ec6bbull);
    STATIC_REQUIRE(call_hash_v<constant<-7>> == 0xaf9fa90d7148ba00ull);
    STATIC_REQUIRE(call_hash_v<constant<1.5>> == 0xfbba434a5c754e3dull);
    STATIC_REQUIRE(call_hash_v<constant<true>> == 0xc661038bf6710aa8ull);
    STATIC_REQUIRE(call_hash_v<constant<5000000000ll>> == 0xac5f301e0fb3e332ull);
    STATIC_REQUIRE(call_hash_v<constant<42u>> == 0x5da974596f7b3ccdull);
    STATIC_REQUIRE(call_hash_v<constant<'x'>> == 0x51bfee7347bac197ull);
    // "evt" = 0x198e4c6da0847235, "n={}" = 0xe697bbcabafe2ed0
    STATIC_REQUIRE(detail::call_string<lit<"evt">, constant<42>, const char*>() ==
                   "call(3){lit:0x198e4c6da0847235,const:i32=42,string}"sv);
    STATIC_REQUIRE(call_hash_v<lit<"evt">, constant<42>, const char*> == 0xc4d099fc7bd16c9cull);
    STATIC_REQUIRE(print_call_hash_v<"n={}", constant<42>> == 0x4adb5ee1abb5efbaull);
    // an enum constant: the enum's schema hash, then the underlying value
    STATIC_REQUIRE([]() consteval {
        std::string want = "call(1){const:";
        detail::append_hex(want, schema_hash_v<corpus::Side>);
        want += "=2}";
        return detail::call_string<constant<corpus::Side::sell>>() == want;
    }());
    // the value, the type and the position all matter; a run-time value is yet another signature
    STATIC_REQUIRE(call_hash_v<constant<42>> != call_hash_v<constant<43>>);
    STATIC_REQUIRE(call_hash_v<constant<42>> != call_hash_v<constant<42u>>);
    STATIC_REQUIRE(call_hash_v<constant<42>> != call_hash_v<constant<42ll>>);
    STATIC_REQUIRE(call_hash_v<constant<42>> != call_hash_v<int>);
    STATIC_REQUIRE(call_hash_v<constant<0.0>> != call_hash_v<constant<-0.0>>);
    STATIC_REQUIRE(call_hash_v<constant<1>, constant<2>> != call_hash_v<constant<2>, constant<1>>);
    STATIC_REQUIRE(call_hash_v<constant<corpus::Side::buy>> !=
                   call_hash_v<constant<corpus::Side::sell>>);
}

// ---- 2.1.16, decision D: the underlying kind of an enum without a fixed underlying type ---------------------
namespace d_rule
{
enum Plain
{
    a,
    b,
    c
}; // no fixed type, no negative enumerator → u32 (GCC: unsigned int)
enum Neg
{
    n = -1,
    p = 1
}; // a negative enumerator → i32 (GCC: int)
enum Big
{
    big = 0x1'0000'0000ll
}; // above 32 bits → 8 bytes, u64
enum Fixed : std::uint8_t
{
    f = 1
};
enum class Scoped
{
    x,
    y
}; // fixed by the standard: int
} // namespace d_rule
TEST_CASE("2.1.16 decision D: an enum without a fixed underlying type takes its kind from its "
          "enumerators",
          "[golden][enum][decision-D]")
{
    STATIC_REQUIRE(!detail::has_fixed_underlying_type_v<d_rule::Plain>);
    STATIC_REQUIRE(!detail::has_fixed_underlying_type_v<d_rule::Neg>);
    STATIC_REQUIRE(!detail::has_fixed_underlying_type_v<d_rule::Big>);
    STATIC_REQUIRE(detail::has_fixed_underlying_type_v<d_rule::Fixed>);
    STATIC_REQUIRE(detail::has_fixed_underlying_type_v<d_rule::Scoped>);
    STATIC_REQUIRE(detail::has_fixed_underlying_type_v<corpus::Side>);
    STATIC_REQUIRE(detail::has_fixed_underlying_type_v<corpus::Venue>); // enum Venue : int
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Plain>() == kind::u32);
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Neg>() == kind::i32);
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Big>() == kind::u64 &&
                   sizeof(d_rule::Big) == 8);
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Fixed>() == kind::u8);
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Scoped>() == kind::i32);
    // the rule is GCC's own choice, so on GCC the derived kind equals the trait's for every enum here and in the
    // corpus — which is why no hash moved; a compiler that picks `int` for d_rule::Plain now agrees with GCC
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Plain>() ==
                   detail::scalar_kind<std::underlying_type_t<d_rule::Plain>>());
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Neg>() ==
                   detail::scalar_kind<std::underlying_type_t<d_rule::Neg>>());
    STATIC_REQUIRE(detail::enum_underlying_kind<d_rule::Big>() ==
                   detail::scalar_kind<std::underlying_type_t<d_rule::Big>>());
    STATIC_REQUIRE(detail::enum_underlying_kind<corpus::Venue>() ==
                   detail::scalar_kind<std::underlying_type_t<corpus::Venue>>());
    STATIC_REQUIRE(descriptor_v<d_rule::Plain>.underlying == kind::u32);
    STATIC_REQUIRE(descriptor_v<d_rule::Neg>.underlying == kind::i32);
    using namespace std::string_view_literals;
    STATIC_REQUIRE(detail::canonical_string<d_rule::Plain>(false) == "enum<u32>[4,4]"sv);
    STATIC_REQUIRE(detail::canonical_string<d_rule::Neg>(true) ==
                   "enum<d_rule::Neg,i32>[4,4]{n=-1;p=1;}"sv);
}

TEST_CASE("2.1.16 decision D: the value range of an enum without a fixed underlying type",
          "[golden][enum][decision-D]")
{
    using detail::enum_ranges_v;
    STATIC_REQUIRE(enum_ranges_v<d_rule::Plain>.size() ==
                   1); // 0..2 → the hypothetical 2-bit type: 0..3
    STATIC_REQUIRE(enum_ranges_v<d_rule::Plain>[0].lo == 0 &&
                   enum_ranges_v<d_rule::Plain>[0].hi == 3);
    STATIC_REQUIRE(enum_ranges_v<d_rule::Plain>[0].size == 4 &&
                   !enum_ranges_v<d_rule::Plain>[0].is_signed);
    STATIC_REQUIRE(enum_ranges_v<d_rule::Neg>[0].lo == -2 &&
                   enum_ranges_v<d_rule::Neg>[0].hi == 1); // -1, 1 → 2 bits signed
    STATIC_REQUIRE(enum_ranges_v<d_rule::Neg>[0].is_signed);
    STATIC_REQUIRE(enum_ranges_v<d_rule::Big>[0].hi == (std::int64_t{1} << 33) - 1);
    STATIC_REQUIRE(enum_ranges_v<d_rule::Fixed>.size() ==
                   0); // a fixed underlying type: every value is valid
    STATIC_REQUIRE(enum_ranges_v<d_rule::Scoped>.size() == 0); // enum class: fixed (int)
    STATIC_REQUIRE(enum_ranges_v<corpus::Order>.size() == 0);  // Side is fixed (u8)
    struct WithPlain
    {
        std::uint16_t a;
        d_rule::Plain p;
        d_rule::Neg n[2];
    };
    STATIC_REQUIRE(enum_ranges_v<WithPlain>.size() == 3);
    STATIC_REQUIRE(enum_ranges_v<WithPlain>[0].offset == 4 &&
                   enum_ranges_v<WithPlain>[1].offset == 8 &&
                   enum_ranges_v<WithPlain>[2].offset == 12);
}

// ---- 2.2.0, format 1.6: maps, sets and containers as arguments (FR-A6, FR-W9) --------------------------------
// The strings are spelled out and the hashes were computed outside the library (XXH3-64 of the string, Python
// xxhash), so a change of either the spelling or the hash is caught here. A member's kindspec is the shape
// (`map<K,V>`, `set<K>`) whatever the C++ container; the container's vocabulary name appears only in the
// transparent type of a top-level argument.
namespace g16
{
struct Flat
{
    std::flat_map<std::uint16_t, std::uint8_t> fm;
    std::unordered_set<std::int16_t> us;
};
struct Maps
{
    std::map<std::int32_t, double> m;
    std::set<std::uint64_t> s;
    std::unordered_map<std::string, corpus::Order> u;
};
struct Ordered
{
    std::map<std::int32_t, double> m;
    std::set<std::uint64_t> s;
    std::map<std::string, corpus::Order> u;
};
} // namespace g16

TEST_CASE(
    "2.2.0 golden strings and hashes of the map/set shapes and the transparent types (format 1.6)",
    "[golden][containers]")
{
    using namespace std::string_view_literals;
    using detail::canonical_string;
    // member kindspecs: the shape, not the container
    STATIC_REQUIRE(canonical_string<g16::Flat>(true) ==
                   "dyn<g16::Flat>[16,4]{fm:map<u16,u8>@0[8,4];us:set<i16>@8[8,4];}"sv);
    STATIC_REQUIRE(canonical_string<g16::Flat>(false) ==
                   "dyn[16,4]{map<u16,u8>@0[8,4];set<i16>@8[8,4];}"sv);
    STATIC_REQUIRE(schema_hash_v<g16::Flat> == 0x2a3f2f2e1ee5ae40ull);
    STATIC_REQUIRE(layout_hash_v<g16::Flat> == 0xe1680fb615a56814ull);
    STATIC_REQUIRE(canonical_string<g16::Maps>(false) ==
                   "dyn[24,4]{map<i32,f64>@0[8,4];set<u64>@8[8,4];map<string,struct<L:"
                   "0x0661bb3e388c01fe>>@16[8,4];}"sv);
    // an unordered_map member and a map member of the same K, V: the same strings and hashes
    STATIC_REQUIRE(canonical_string<g16::Maps>(false) == canonical_string<g16::Ordered>(false));
    STATIC_REQUIRE(layout_hash_v<g16::Maps> == layout_hash_v<g16::Ordered>);
    STATIC_REQUIRE(schema_hash_v<g16::Maps> !=
                   schema_hash_v<g16::Ordered>); // the owners' names differ
    // transparent types: name = the container's vocabulary name, one unnamed slot field, size 8, align 4
    STATIC_REQUIRE(canonical_name_v<std::vector<std::int32_t>> == "std::vector<i32>"sv);
    STATIC_REQUIRE(canonical_string<std::vector<std::int32_t>>(true) ==
                   "dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}"sv);
    STATIC_REQUIRE(canonical_string<std::vector<std::int32_t>>(false) ==
                   "dyn[8,4]{vector<i32>@0[8,4];}"sv);
    STATIC_REQUIRE(fingerprint64("dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}"sv) ==
                   0x02c87ef3a015eb45ull);
    STATIC_REQUIRE(schema_hash_v<std::vector<std::int32_t>> == 0x02c87ef3a015eb45ull);
    STATIC_REQUIRE(layout_hash_v<std::vector<std::int32_t>> == 0x03d4f3aff8e167ebull);
    STATIC_REQUIRE(descriptor_v<std::vector<std::int32_t>>.size == 8 &&
                   descriptor_v<std::vector<std::int32_t>>.align == 4 &&
                   descriptor_v<std::vector<std::int32_t>>.transparent &&
                   !descriptor_v<std::vector<std::int32_t>>.is_flat);
    STATIC_REQUIRE(canonical_name_v<std::map<std::string, std::int32_t>> ==
                   "std::map<std::string,i32>"sv);
    STATIC_REQUIRE(schema_hash_v<std::map<std::string, std::int32_t>> == 0x9ba49a614d8d0569ull);
    STATIC_REQUIRE(canonical_name_v<std::unordered_map<std::int32_t, std::string>> ==
                   "std::unordered_map<i32,std::string>"sv);
    STATIC_REQUIRE(canonical_string<std::unordered_map<std::int32_t, std::string>>(true) ==
                   "dyn<std::unordered_map<i32,std::string>>[8,4]{:map<i32,string>@0[8,4];}"sv);
    STATIC_REQUIRE(schema_hash_v<std::unordered_map<std::int32_t, std::string>> ==
                   0xc0d088a8ef8a5703ull);
    STATIC_REQUIRE(canonical_name_v<std::set<std::uint64_t>> == "std::set<u64>"sv);
    STATIC_REQUIRE(schema_hash_v<std::set<std::uint64_t>> == 0xcfbc288f1d543208ull);
    // the same container as a member and as an argument share the layout string of the slot
    STATIC_REQUIRE(layout_hash_v<std::map<std::string, std::int32_t>> ==
                   layout_hash_v<std::unordered_map<std::string, std::int32_t>>);
    // the entry layout: key at 0, value at its canonical offset, stride rounded to the pair's alignment
    STATIC_REQUIRE(detail::map_entry_layout<std::string, corpus::Order>().value_offset == 8 &&
                   detail::map_entry_layout<std::string, corpus::Order>().size == 40 &&
                   detail::map_entry_layout<std::string, corpus::Order>().align == 8);
    STATIC_REQUIRE(detail::map_entry_layout<std::uint8_t, corpus::DynMsg>().value_offset == 4 &&
                   detail::map_entry_layout<std::uint8_t, corpus::DynMsg>().size == 24);
    STATIC_REQUIRE(detail::map_entry_layout<std::uint16_t, std::uint8_t>().size == 4);
    // the corpus type of the golden dynamic.ser
    STATIC_REQUIRE(schema_hash_v<corpus::DynMap> == 0x1aa730519093de87ull);
    STATIC_REQUIRE(layout_hash_v<corpus::DynMap> == 0x5e48ff513e467a38ull);
    STATIC_REQUIRE(descriptor_v<corpus::DynMap>.size == 36);
}

// ---- sequence, tuple-like and multimap shapes (format 1.7, library 2.2.3) ----------------------------------
namespace g17
{
struct Seqs
{
    std::deque<std::int32_t> d;
    std::list<std::int16_t> l;
    std::forward_list<std::uint8_t> f;
};
struct Multi
{
    std::multimap<std::int32_t, std::string> mm;
    std::unordered_multimap<std::uint16_t, std::int32_t> um;
    std::stack<std::int32_t> st;
};
struct WithPair
{
    std::uint8_t tag;
    std::pair<std::int32_t, double> p;
};
} // namespace g17

TEST_CASE(
    "golden strings of std::deque/list/forward_list, std::pair/tuple, multimaps and std::stack",
    "[golden][containers]")
{
    using namespace std::string_view_literals;
    using detail::canonical_string;
    // deque, list and forward_list are the vector shape
    STATIC_REQUIRE(
        canonical_string<g17::Seqs>(true) ==
        "dyn<g17::Seqs>[24,4]{d:vector<i32>@0[8,4];l:vector<i16>@8[8,4];f:vector<u8>@16[8,4];}"sv);
    STATIC_REQUIRE(canonical_name_v<std::deque<std::int32_t>> == "std::deque<i32>"sv);
    STATIC_REQUIRE(canonical_name_v<std::list<std::int16_t>> == "std::list<i16>"sv);
    STATIC_REQUIRE(canonical_name_v<std::forward_list<std::uint8_t>> == "std::forward_list<u8>"sv);
    // a std::vector<i32> and a std::deque<i32> argument share the slot layout, not the schema (the owner's name)
    STATIC_REQUIRE(layout_hash_v<std::deque<std::int32_t>> ==
                   layout_hash_v<std::vector<std::int32_t>>);
    STATIC_REQUIRE(schema_hash_v<std::deque<std::int32_t>> !=
                   schema_hash_v<std::vector<std::int32_t>>);
    // pair and tuple: positional parts at canonical offsets, always an image (never memcpy: the element order in
    // memory is the standard library's), names first/second and _0, _1, …
    STATIC_REQUIRE(canonical_name_v<std::pair<std::int32_t, double>> == "std::pair<i32,f64>"sv);
    STATIC_REQUIRE(canonical_string<std::pair<std::int32_t, double>>(true) ==
                   "dyn<std::pair<i32,f64>>[16,8]{first:i32@0[4,4];second:f64@8[8,8];}"sv);
    STATIC_REQUIRE(canonical_string<std::pair<std::int32_t, double>>(false) ==
                   "dyn[16,8]{i32@0[4,4];f64@8[8,8];}"sv);
    STATIC_REQUIRE(canonical_name_v<std::tuple<std::uint8_t, std::string, std::int64_t>> ==
                   "std::tuple<u8,std::string,i64>"sv);
    STATIC_REQUIRE(
        canonical_string<std::tuple<std::uint8_t, std::string, std::int64_t>>(true) ==
        "dyn<std::tuple<u8,std::string,i64>>[24,8]{_0:u8@0[1,1];_1:string@4[8,4];_2:i64@16[8,8];}"sv);
    STATIC_REQUIRE(!descriptor_v<std::pair<std::int32_t, double>>.is_flat);
    STATIC_REQUIRE(
        descriptor_v<std::tuple<std::uint8_t, std::string, std::int64_t>>.fields.size() == 3);
    // a class with a pair member is a dynamic class; the pair sits inline in the fixed part at its canonical offset
    STATIC_REQUIRE(canonical_string<g17::WithPair>(true) ==
                   "dyn<g17::WithPair>[24,8]{tag:u8@0[1,1];p:dyn<0xc7c8f023563fcd20>@8[16,8];}");
    STATIC_REQUIRE(schema_hash_v<std::pair<std::int32_t, double>> == 0xc7c8f023563fcd20ULL);
    STATIC_REQUIRE(layout_hash_v<std::pair<std::int32_t, std::string>> == 0x961e92e63c248ee2ULL);
    STATIC_REQUIRE(layout_hash_v<std::pair<std::uint16_t, std::int32_t>> == 0xf9e0315fabd2ceceULL);
    // multimap and unordered_multimap: one schema shape `multimap<K,V>`; the layout is a vector of pairs
    STATIC_REQUIRE(
        canonical_string<g17::Multi>(true) ==
        "dyn<g17::Multi>[24,4]{mm:multimap<i32,string>@0[8,4];um:multimap<u16,i32>@8[8,4];st:vector<i32>@16[8,4];}"sv);
    STATIC_REQUIRE(canonical_string<g17::Multi>(false) ==
                   "dyn[24,4]{vector<dyn<L:0x961e92e63c248ee2>>@0[8,4];vector<dyn<L:"
                   "0xf9e0315fabd2cece>>@8[8,4];"
                   "vector<i32>@16[8,4];}");
    STATIC_REQUIRE(canonical_name_v<std::multimap<std::int32_t, std::string>> ==
                   "std::multimap<i32,std::string>"sv);
    STATIC_REQUIRE(canonical_name_v<std::unordered_multimap<std::uint16_t, std::int32_t>> ==
                   "std::unordered_multimap<u16,i32>"sv);
    STATIC_REQUIRE(canonical_name_v<std::stack<std::int32_t>> == "std::stack<i32>"sv);
    STATIC_REQUIRE(layout_hash_v<std::stack<std::int32_t>> ==
                   layout_hash_v<std::vector<std::int32_t>>);
    // the schema strings behind the public hashes
    STATIC_REQUIRE(fingerprint64(schema_string_v<corpus::Order>) == schema_hash_v<corpus::Order>);
    STATIC_REQUIRE(fingerprint64(layout_string_v<corpus::Order>) == layout_hash_v<corpus::Order>);
    STATIC_REQUIRE(schema_string_v<corpus::Side> == "enum<corpus::Side,u8>[1,1]{buy=1;sell=2;}"sv);
}
