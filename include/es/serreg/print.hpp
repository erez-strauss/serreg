// es/serreg/print.hpp — print / println (plan v1.1 §2, FR-P1..P6): binary structured
// logging on top of serialize(). The format string is a literal (its id crosses the wire, its text lives in
// the dictionary), the arguments go out as records, and the *reader* runs the formatting
// (tools::render_frame, srdump). Nothing is formatted or allocated on the writer's side.
//
//   using namespace es::serreg::literals;
//   print  (out, "hello {} {} {}"_, "new", 100, "worlds"_);   // "new" is data, "worlds"_ an id
//   println(out, "px={:.2f} qty={}"_, px, qty);              // the reader appends '\n'
//   println<"order {}">(out, order);                          // template-argument spelling
//
// The format string is checked at compile time by the standard's own checker (std::format_string) against
// the rendered types: scalars as themselves, literals/strings/enums/structs as std::string_view. A wrong
// placeholder count or a spec that does not fit the argument is a compile error at the call site. Readers
// format structs and enums generically (srdump's inline text, the enumerator name): a user's std::formatter<T>
// is not consulted, because the reader need not have T (cc review 2.1.10, C-14). There is no timestamp, level or
// source location in a print frame; put them in the arguments.
#pragma once
#include <es/serreg/serialize.hpp>

#include <format>
#include <string_view>
#include <type_traits>

namespace es::serreg
{

namespace detail
{
// How the reader renders each argument category — what the compile-time format check is done against.
template<class T> struct render_type
{
    using type = std::string_view; // literal text, string bytes, enumerator name, struct as text
};
template<class T>
    requires(is_scalar_arg_v<T> && !std::is_same_v<T, std::byte> && !std::is_same_v<T, char8_t> &&
             !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t> &&
             !std::is_same_v<T, wchar_t>)
struct render_type<T>
{
    using type = T;
};
// 1.5: a constant renders as the value it stands for
template<class T> using render_t = typename render_type<arg_value_t<std::remove_cvref_t<T>>>::type;

template<class T>
concept printable_arg =
    serializable_arg<T> && !std::is_same_v<arg_value_t<std::remove_cvref_t<T>>, std::byte> &&
    !std::is_same_v<arg_value_t<std::remove_cvref_t<T>>, char8_t> &&
    !std::is_same_v<arg_value_t<std::remove_cvref_t<T>>, char16_t> &&
    !std::is_same_v<arg_value_t<std::remove_cvref_t<T>>, char32_t> &&
    !std::is_same_v<arg_value_t<std::remove_cvref_t<T>>,
                    wchar_t>; // std::format<char> cannot render these

// FR-P2: constructing a std::format_string in a constant expression runs libstdc++'s format-string checker.
template<fixed_string F, class... Ts> consteval bool check_format()
{
    const std::format_string<render_t<Ts>...> f{F.view()};
    static_cast<void>(f);
    return true;
}
// 2.1.12 (review T-05): nested replacement fields ({:>{}} — a width or precision taken from an argument) pass
// std::format_string, but no reader renders them (srdump reports invalid_argument, the Wireshark dissector shows
// the field as written). A '{' inside a replacement field is rejected at compile time instead.
template<fixed_string F> consteval bool no_nested_fields()
{
    const std::string_view s = F.view();
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '{')
            continue;
        if (i + 1 < s.size() && s[i + 1] == '{')
        { // an escaped brace
            ++i;
            continue;
        }
        for (std::size_t j = i + 1; j < s.size() && s[j] != '}'; ++j)
            if (s[j] == '{')
                return false;
    }
    return true;
}
} // namespace detail

template<sink S, fixed_string F, detail::printable_arg... Ts>
result<std::size_t> print_with(const serialize_options o, S& out, lit<F>,
                               const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    static_assert(detail::check_format<F, Ts...>());
    static_assert(
        detail::no_nested_fields<F>(),
        "print: nested replacement fields ({:{}}) are not supported by the readers; write the "
        "width and precision into the format string");
    return detail::serialize_frame<print_tag, lit<F>, std::remove_cvref_t<Ts>...>(o, out, lit<F>{},
                                                                                  vs...);
}
template<sink S, fixed_string F, detail::printable_arg... Ts>
result<std::size_t> println_with(const serialize_options o, S& out, lit<F>,
                                 const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    static_assert(detail::check_format<F, Ts...>());
    static_assert(
        detail::no_nested_fields<F>(),
        "print: nested replacement fields ({:{}}) are not supported by the readers; write the "
        "width and precision into the format string");
    return detail::serialize_frame<println_tag, lit<F>, std::remove_cvref_t<Ts>...>(
        o, out, lit<F>{}, vs...);
}
// print / println: the names mirror std::print / std::println on purpose — `namespace sr = es::serreg;
// sr::println(out, "px={:.2f}"_, px)` reads as what it is. The first parameter is a sink, so the overload sets never
// collide with std's (a FILE* or a format string is not a sink) and ADL finds them from a capture_file argument.
template<sink S, fixed_string F, detail::printable_arg... Ts>
result<std::size_t> print(S& out, const lit<F> f,
                          const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return print_with(serialize_options{}, out, f, vs...);
}
template<sink S, fixed_string F, detail::printable_arg... Ts>
result<std::size_t> println(S& out, const lit<F> f,
                            const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return println_with(serialize_options{}, out, f, vs...);
}
// Template-argument spellings: print<"x = {}">(out, x).
template<fixed_string F, sink S, detail::printable_arg... Ts>
result<std::size_t> print(S& out, const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return print_with(serialize_options{}, out, lit<F>{}, vs...);
}
template<fixed_string F, sink S, detail::printable_arg... Ts>
result<std::size_t> println(S& out, const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return println_with(serialize_options{}, out, lit<F>{}, vs...);
}

// Call hashes of the print forms (what a typed reader or a test compares against).
template<fixed_string F, class... Ts>
inline constexpr std::uint64_t print_call_hash_v = call_hash_v<print_tag, lit<F>, Ts...>;
template<fixed_string F, class... Ts>
inline constexpr std::uint64_t println_call_hash_v = call_hash_v<println_tag, lit<F>, Ts...>;

} // namespace es::serreg
