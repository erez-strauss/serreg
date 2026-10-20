// es/serreg/arg.hpp — the call-argument model (plan v1.1 §1, FR-A1..A4). Every argument of serialize() /
// deserialize() / dispatch<>() is one of five categories, each with a canonical name (the call string is
// built from them), a 64-bit hash (the ARG hash in the call ENTRY and the full-record header) and a record
// layout (format.hpp):
//
//   category  example arguments                                 canonical name        record (compact)
//   type      any flat_type T (struct, enum, as_flat)           <hex schema hash>     sizeof(T) bytes
//   type      any dynamic_type T (v2: string/vector/… members)  <hex schema hash>     u32 len, u32 0, image
//   literal   "text"_lit / "text"_ / lit<"text">{}              lit:<hex id>          u64 id
//   scalar    bool, char, int, std::uint64_t, double, std::byte i32, f64, bool, …      sizeof bytes      (1.1)
//   string    const char*, "text", std::string_view, std::string string                u32 len, bytes    (1.1)
//             (2.1.5: also std::pmr::string and any std::basic_string<char, char_traits<char>, Alloc>)
//   constant  42_, -7_, 1.5_, k<true>, k<Side::buy>             const:i32=42          none             (1.5)
//             (literal.hpp; a scalar or an enum)                const:<enum hash>=1
//
// Names are category names, not C++ type names: serialize(out, "abc") and deserialize(bytes, std::string&)
// build the same call string. A runtime string is data on the wire (FR-L5); a literal is an interned id.
#pragma once
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/literal.hpp>
#include <es/serreg/reflect.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace es::serreg
{

namespace detail
{

template<class T> constexpr bool is_string_arg_v = false; // T = std::remove_cvref_t<argument>
template<> inline constexpr bool is_string_arg_v<const char*> = true;
template<> inline constexpr bool is_string_arg_v<char*> = true;
template<std::size_t N> inline constexpr bool is_string_arg_v<char[N]> = true;
template<std::size_t N> inline constexpr bool is_string_arg_v<const char[N]> = true;
template<> inline constexpr bool is_string_arg_v<std::string_view> = true;
// 2.1.5: std::string with any allocator (std::pmr::string, arena strings) is the same string category
template<class A>
inline constexpr bool is_string_arg_v<std::basic_string<char, std::char_traits<char>, A>> = true;

template<class T> constexpr bool is_scalar_arg_v = is_scalar_v<std::remove_cv_t<T>>;

// 1.5: the type a constant<V> stands for (T itself for every other argument). A constant is described, hashed
// and — in a full-records frame — written exactly as a run-time value of this type.
template<class T> struct arg_value
{
    using type = T;
};
template<auto V> struct arg_value<constant<V>>
{
    using type = typename constant<V>::value_type;
};
template<class T> using arg_value_t = typename arg_value<T>::type;
template<class T>
constexpr bool is_constant_arg_ok_v =
    is_constant_v<T> &&
    (is_scalar_v<arg_value_t<T>> || (std::is_enum_v<arg_value_t<T>> && flat_type<arg_value_t<T>>));
// The 8 value bytes of a constant's ARG: the object's bytes, zero-extended (little-endian, like every record).
template<class V> consteval std::uint64_t const_bits(const V v)
{
    if constexpr (std::is_enum_v<V> && !std::is_same_v<V, std::byte>)
        return const_bits(static_cast<std::underlying_type_t<V>>(v));
    else if constexpr (std::is_same_v<V, std::byte>)
        return std::to_integer<std::uint64_t>(v);
    else if constexpr (std::is_same_v<V, bool>)
        return v ? 1 : 0;
    else if constexpr (std::is_same_v<V, double>)
        return std::bit_cast<std::uint64_t>(v);
    else if constexpr (std::is_same_v<V, float>)
        return std::bit_cast<std::uint32_t>(v);
    else
        return static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<V>>(v));
}

// The category of an (already remove_cvref'd) argument type.
template<class T> consteval fmt::arg_kind arg_kind_of()
{
    if constexpr (is_literal_v<T>)
        return fmt::arg_literal;
    else if constexpr (is_constant_v<T>)
        return fmt::arg_const;
    else if constexpr (is_string_arg_v<T>)
        return fmt::arg_string;
    else if constexpr (is_scalar_arg_v<T>)
        return fmt::arg_scalar;
    else
        return fmt::arg_type;
}

inline constexpr std::string_view string_arg_name = "string";
inline constexpr std::uint64_t string_arg_hash = fingerprint64(string_arg_name);

// Canonical name of a scalar / string argument (types and literals are rendered by call_string()).
template<class T> consteval std::string_view arg_name()
{
    if constexpr (is_string_arg_v<T>)
        return string_arg_name;
    else
        return to_string(scalar_kind<std::remove_cv_t<T>>());
}

// The ARG hash: schema hash | literal id | fingerprint64(scalar name) | fingerprint64("string"); a constant has
// the hash of the value it stands for.
template<class T> consteval std::uint64_t arg_hash()
{
    if constexpr (is_literal_v<T>)
        return T::id;
    else if constexpr (is_constant_v<T>)
        return arg_hash<arg_value_t<T>>();
    else if constexpr (is_string_arg_v<T>)
        return string_arg_hash;
    else if constexpr (is_scalar_arg_v<T>)
        return fingerprint64(arg_name<T>());
    else
        return schema_hash_v<T>;
}
template<class T> consteval kind arg_scalar_kind()
{
    if constexpr (is_constant_v<T>)
        return arg_scalar_kind<arg_value_t<T>>(); // kind{} for an enum constant
    else if constexpr (is_scalar_arg_v<T>)
        return scalar_kind<std::remove_cv_t<T>>();
    else
        return kind{};
}
template<class T> consteval std::uint64_t arg_const_value()
{
    if constexpr (is_constant_v<T>)
        return const_bits(T::value);
    else
        return 0;
}

// Runtime string arguments: bytes and length. A char array is NUL-terminated text (strnlen), never a fixed
// buffer — fixed buffers are members of flat types, not top-level arguments.
inline std::string_view string_bytes(const char* const s) noexcept
{
    return s ? std::string_view{s} : std::string_view{};
}
template<std::size_t N> inline std::string_view string_bytes(const char (&a)[N]) noexcept
{
    return {a, ::strnlen(a, N)};
}
inline std::string_view string_bytes(const std::string_view s) noexcept
{
    return s;
}
template<class A>
inline std::string_view string_bytes(
    const std::basic_string<char, std::char_traits<char>, A>& s) noexcept
{
    return s;
}

template<class T>
concept serializable_arg =
    is_literal_v<std::remove_cvref_t<T>> || is_constant_arg_ok_v<std::remove_cvref_t<T>> ||
    (!is_constant_v<std::remove_cvref_t<T>> && flat_type<std::remove_cvref_t<T>>) ||
    dynamic_type<std::remove_cvref_t<T>> || // v2: canonical image
    is_scalar_arg_v<std::remove_cvref_t<T>> || is_string_arg_v<std::remove_cvref_t<T>>;

} // namespace detail

// Public aliases for the categories (the wire values are fmt::arg_kind).
template<class T>
inline constexpr fmt::arg_kind arg_kind_v = detail::arg_kind_of<std::remove_cvref_t<T>>();
template<class T>
inline constexpr std::uint64_t arg_hash_v = detail::arg_hash<std::remove_cvref_t<T>>();

} // namespace es::serreg
