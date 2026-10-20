// es/serreg/descriptor.hpp — compile-time type description data model (spec §6.1).
// This header has NO reflection dependency; it is shared by the reflection layer (reflect.hpp) and the
// compiled runtime (libserreg). Everything here is a literal/structural type so it can live in .rodata.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Per-image entities (descriptors, blobs, registrar nodes, tables) are hidden: no STB_GNU_UNIQUE binding
// (which would mark a DSO DF_1_NODELETE and make dlclose a silent no-op) and one copy per image, which is the
// registry's per-image reference counting model (spec §6.3b, cc F12).
#define ES_SERREG_HIDDEN [[gnu::visibility("hidden")]]

namespace es::serreg
{

enum class kind : std::uint8_t
{
    i8 = 1,
    i16,
    i32,
    i64,
    u8,
    u16,
    u32,
    u64,
    f32,
    f64,
    boolean,
    char_,
    char8,
    char16,
    char32,
    wchar,
    byte_,
    enum_ = 32,
    array_,
    struct_, // v1
    string_ = 48,
    vector_,
    optional_,
    variant_,
    literal_, // v2 encodings; kinds reserved in the v1 dictionary schema
    map_, // 1.6 (2.2.0): sorted (key, value) entries — std::map, std::unordered_map, std::flat_map, …
    set_ // 1.6 (2.2.0): sorted keys — std::set, std::unordered_set, std::flat_set, …
};

// Native size of one scalar kind (0 for non-scalars).
constexpr std::uint32_t kind_size(const kind k) noexcept
{
    switch (k)
    {
    case kind::i8:
    case kind::u8:
    case kind::boolean:
    case kind::char_:
    case kind::char8:
    case kind::byte_:
        return 1;
    case kind::i16:
    case kind::u16:
    case kind::char16:
        return 2;
    case kind::i32:
    case kind::u32:
    case kind::f32:
    case kind::char32:
    case kind::wchar:
        return 4;
    case kind::i64:
    case kind::u64:
    case kind::f64:
        return 8;
    default:
        return 0;
    }
}

constexpr std::string_view to_string(const kind k) noexcept
{
    switch (k)
    {
    case kind::i8:
        return "i8";
    case kind::i16:
        return "i16";
    case kind::i32:
        return "i32";
    case kind::i64:
        return "i64";
    case kind::u8:
        return "u8";
    case kind::u16:
        return "u16";
    case kind::u32:
        return "u32";
    case kind::u64:
        return "u64";
    case kind::f32:
        return "f32";
    case kind::f64:
        return "f64";
    case kind::boolean:
        return "bool";
    case kind::char_:
        return "char";
    case kind::char8:
        return "char8";
    case kind::char16:
        return "char16";
    case kind::char32:
        return "char32";
    case kind::wchar:
        return "wchar";
    case kind::byte_:
        return "byte";
    case kind::enum_:
        return "enum";
    case kind::array_:
        return "array";
    case kind::struct_:
        return "struct";
    case kind::string_:
        return "string";
    case kind::vector_:
        return "vector";
    case kind::optional_:
        return "optional";
    case kind::variant_:
        return "variant";
    case kind::literal_:
        return "literal";
    case kind::map_:
        return "map";
    case kind::set_:
        return "set";
    }
    return "?";
}

constexpr bool is_scalar_kind(const kind k) noexcept
{
    return static_cast<std::uint8_t>(k) < 32;
}

struct gap
{
    std::uint32_t offset;
    std::uint32_t len;
}; // padding range inside a native object (FR-S6)

struct field_desc
{
    std::string_view
        name; // member identifier, or "<base:Name>" for a base-class pseudo-field (§6.1b)
    std::uint32_t offset; // native byte offset (flat types)
    std::uint32_t size;
    std::uint16_t align;
    kind k;
    std::uint16_t since;      // FR-S11: [[=since(N)]], 0 = unset
    std::uint16_t deprecated; // FR-S11: [[=deprecated(N)]], 0 = unset
    std::uint64_t type_hash;  // struct_/enum_: nested schema hash; else 0
    std::uint32_t extent;     // array_: element count; variant_: alternative count
    const field_desc*
        elem; // array_: element description (name empty); variant_: alternatives[extent]
};

struct enumerator_desc
{
    std::string_view name;
    std::int64_t value;
};

struct type_desc
{
    std::string_view name; // canonical name (§6.2b)
    std::uint64_t schema_hash;
    std::uint64_t layout_hash;
    std::uint32_t size;
    std::uint32_t align;
    kind k;          // struct_ | enum_
    kind underlying; // enum_: underlying scalar kind; struct_: unused
    bool is_flat;    // memcpy-able native image
    bool
        transparent; // 1.6: a container argument's synthetic type (one unnamed field; struct_flag_transparent)
    bool enum_fixed; // 1.6: enum_ with a fixed underlying type (enum_flag_fixed)
    std::uint16_t since;
    std::uint16_t deprecated;
    std::span<const field_desc> fields;           // struct_
    std::span<const enumerator_desc> enumerators; // enum_
    std::span<const gap> padding_gaps;            // FR-S6 scrub table
    std::span<const std::uint32_t> bool_offsets;  // bool leaves, normalised to 0/1 when scrubbing
    std::span<const std::byte>
        blob; // this descriptor serialized as one ENTRY (format.hpp), in .es_serreg_dict
};

} // namespace es::serreg
