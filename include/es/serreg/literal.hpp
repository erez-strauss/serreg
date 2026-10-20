// es/serreg/literal.hpp — compile-time string literals as serialisable arguments (spec §4.4, §6.6, FR-L1..L5).
//
//   using namespace es::serreg::literals;
//   serialize(sink, "order rejected"_lit, order);        // UDL
//   serialize(sink, "order rejected"_, order);           // shorter UDL, same thing
//   serialize<"order rejected">(sink, order);            // literal as the leading template argument
//   constexpr es::serreg::lit<"order rejected"> k_rejected;  serialize(sink, k_rejected, order);  // named
//
// The literal's identity must be in the *type* (or a template argument): that is the only channel that reaches
// static-initialisation-time registration, so a runtime `const char*`/`string_view` cannot be a literal.
// The text is registered once per process; only the 8-byte id (XXH3-64 of the bytes, FR-H7) crosses the wire.
//
// Footprint (v1.0.3): the text lives in exactly one static object per image — the COMDAT ENTRY blob
// (`detail::literal_blob_bytes_v<S>`) — plus the TU-local copy in the `es_serreg_dict` section that offline
// readers need. `lit<S>::text()` and the registry node both view the blob; the template-parameter object S is
// never odr-used and is not emitted.
#pragma once
#include <es/serreg/descriptor.hpp> // ES_SERREG_HIDDEN
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace es::serreg
{

namespace detail
{
template<fixed_string S> consteval std::vector<std::byte> literal_blob_bytes()
{
    cbuf b;
    b.begin_entry(fmt::tag_literal);
    b.put(fingerprint64(S.view()));
    b.put(static_cast<std::uint32_t>(S.size()));
    b.put(std::uint32_t{0});
    for (const char c : S.view())
        b.v.push_back(static_cast<std::byte>(c));
    b.end_entry();
    return b.v;
}
template<fixed_string S> struct literal_blob_p
{
    static consteval std::vector<std::byte> bytes()
    {
        return literal_blob_bytes<S>();
    }
};
// The literal ENTRY blob: ordinary COMDAT-deduplicated .rodata, one per image (hidden visibility, see blob.hpp).
template<fixed_string S>
ES_SERREG_HIDDEN inline constexpr auto literal_blob_bytes_v = to_blob_array<literal_blob_p<S>>();
} // namespace detail

template<fixed_string S> struct lit
{
    static constexpr auto str = S; // the NTTP itself (for registrar_lit<lit<S>::str>)
    static constexpr std::size_t size = S.size();
    static constexpr std::uint64_t id = fingerprint64(S.view());
    // The text, viewed inside the ENTRY blob (not constexpr: a byte→char view needs reinterpret_cast).
    static std::string_view text() noexcept
    {
        return {reinterpret_cast<const char*>(detail::literal_blob_bytes_v<S>.data()) +
                    fmt::literal_text_offset,
                size};
    }
};

template<class T> constexpr bool is_literal_v = false;
template<fixed_string S> constexpr bool is_literal_v<lit<S>> = true;

// ---- compile-time constants as arguments (2.1.15, format 1.5) --------------------------------------------
//   serialize(sink, "fill"_, 42_, order, -7_, 1.5_);     // 42, -7 and 1.5 are part of the call signature
//   serialize(sink, "side"_, k<Side::buy>, k<true>, k<'x'>, k<42u>);
// A constant<V> is to a number what lit<S> is to a string: its value is in the *type*, so it flows into the call
// string ("const:i32=42") and the call hash, is written once in the call's dictionary entry and occupies no bytes
// in a compact frame. In a full-records frame it is written as the scalar or enum record it would be at run time.
// V is a scalar (bool, char, integers, float, double, std::byte) or an enum; arg.hpp checks that.
// `42_` has the type C++ gives the literal without a suffix, except that only signed types are tried first:
// int, else long long, else unsigned long long; a floating literal is a double. `k<V>` spells every other type.
template<auto V> struct constant
{
    using value_type = std::remove_const_t<decltype(V)>;
    static constexpr value_type value = V;
};
template<class T> constexpr bool is_constant_v = false;
template<auto V> constexpr bool is_constant_v<constant<V>> = true;
template<auto V> inline constexpr constant<V> k{};

// -7_ : the literal operator sees "7"; unary minus on the constant negates it (signed and floating types).
template<auto V>
    requires((std::is_signed_v<decltype(V)> || std::is_floating_point_v<decltype(V)>) &&
             !std::is_same_v<std::remove_const_t<decltype(V)>, char>)
constexpr constant<-V> operator-(constant<V>) noexcept
{
    return {};
}
template<auto V>
    requires(std::is_arithmetic_v<decltype(V)> &&
             !std::is_same_v<std::remove_const_t<decltype(V)>, bool>)
constexpr constant<V> operator+(constant<V>) noexcept
{
    return {};
}

namespace detail
{
// Not constexpr and never defined: reaching a call during constant evaluation is the compile error, and the
// message is in the diagnostic (the library is built without exceptions, so `throw` is not available).
void constant_error(const char* what);
consteval bool numeric_is_hex(const char* const s, const std::size_t n)
{
    return n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}
consteval bool numeric_is_float(const char* const s, const std::size_t n)
{
    const bool hex = numeric_is_hex(s, n);
    for (std::size_t i = 0; i < n; ++i)
        if (s[i] == '.' || (!hex && (s[i] == 'e' || s[i] == 'E')) ||
            (hex && (s[i] == 'p' || s[i] == 'P')))
            return true;
    return false;
}
// 42  0x2A  052  0b101010  1'000 — the value; a literal that does not fit 64 bits is not a constant expression.
consteval unsigned long long numeric_uint(const char* const s, const std::size_t n)
{
    unsigned base = 10;
    std::size_t i = 0;
    if (n > 1 && s[0] == '0')
    {
        if (s[1] == 'x' || s[1] == 'X')
            base = 16, i = 2;
        else if (s[1] == 'b' || s[1] == 'B')
            base = 2, i = 2;
        else
            base = 8, i = 1;
    }
    unsigned long long v = 0;
    for (; i < n; ++i)
    {
        const char c = s[i];
        if (c == '\'')
            continue;
        const unsigned d = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                           : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                                  : static_cast<unsigned>(c - 'A' + 10);
        if (d >= base || v > (~0ull - d) / base)
            constant_error(
                "es::serreg: an integer constant that does not fit 64 bits (or a bad digit)");
        v = v * base + d;
    }
    return v;
}
// 1.5  1e-3  2.5e10  1'000.25 — decimal floating literals. Digits are accumulated exactly as an integer while they
// fit 2^53 and scaled by one exact power of ten when |exponent| <= 22, so every literal with at most 15
// significant digits in that range is the correctly rounded double, the same value the compiler gives `1.5`.
consteval double numeric_double(const char* const s, const std::size_t n)
{
    if (numeric_is_hex(s, n))
        constant_error(
            "es::serreg: hexadecimal floating constants are not supported; write k<0x1p3>");
    unsigned long long mant = 0;
    int digits = 0, scale = 0, ex = 0;
    bool frac = false;
    std::size_t i = 0;
    for (; i < n && s[i] != 'e' && s[i] != 'E'; ++i)
    {
        if (s[i] == '\'')
            continue;
        if (s[i] == '.')
        {
            frac = true;
            continue;
        }
        if (mant != 0 || s[i] != '0')
            ++digits;
        if (digits > 15)
            constant_error("es::serreg: more than 15 significant digits in a floating constant; "
                           "write k<value>");
        mant = mant * 10 + static_cast<unsigned>(s[i] - '0');
        if (frac)
            --scale;
    }
    if (i < n)
    {
        ++i;
        bool neg = false;
        if (i < n && (s[i] == '-' || s[i] == '+'))
            neg = s[i++] == '-';
        for (; i < n; ++i)
        {
            ex = ex * 10 + (s[i] - '0');
            if (ex > 400)
                constant_error("es::serreg: floating constant exponent out of range");
        }
        if (neg)
            ex = -ex;
    }
    const int e10 = ex + scale;
    if (e10 < -22 || e10 > 22)
        constant_error(
            "es::serreg: a floating constant with |decimal exponent| > 22; write k<value>");
    double p10 = 1;
    for (int j = 0; j < (e10 < 0 ? -e10 : e10); ++j)
        p10 *= 10;                              // exact up to 1e22
    const double m = static_cast<double>(mant); // exact: at most 15 digits
    return e10 < 0 ? m / p10 : m * p10;         // one correctly rounded operation
}
template<char... Cs> consteval auto numeric()
{
    constexpr char s[]{Cs..., 0};
    constexpr std::size_t n = sizeof...(Cs);
    if constexpr (numeric_is_float(s, n))
        return constant<numeric_double(s, n)>{};
    else if constexpr (numeric_uint(s, n) <= 0x7fffffffull)
        return constant<static_cast<int>(numeric_uint(s, n))>{};
    else if constexpr (numeric_uint(s, n) <= 0x7fffffffffffffffull)
        return constant<static_cast<long long>(numeric_uint(s, n))>{};
    else
        return constant<numeric_uint(s, n)>{};
}
} // namespace detail

inline namespace literals
{
template<fixed_string S> consteval lit<S> operator""_lit()
{
    return {};
}
template<fixed_string S> consteval lit<S> operator""_() // the short form: "text"_
{
    return {};
}
template<char... Cs>
// Note: a numeric UDL does not follow the usual integer-literal typing — `numeric` picks int, long long or unsigned
// long long by the value alone, so 0xFFFFFFFF_ is a `long long` constant, not the `unsigned int` that 0xFFFFFFFFu is;
// name the type with constant<std::uint32_t{0xFFFFFFFF}> when the width matters (docs/deviations.md).
consteval auto operator""_() // 2.1.15: 42_  0x10_  1.5_  5'000'000'000_  (-7_ with unary minus)
{
    return detail::numeric<Cs...>();
}
} // namespace literals

} // namespace es::serreg

#define ES_SERREG_LIT(s) (::es::serreg::lit<::es::serreg::fixed_string{s}>{})
