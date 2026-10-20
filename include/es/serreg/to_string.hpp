// es/serreg/to_string.hpp — 2.2.1: the text and JSON forms of any value the library can serialize, rendered
// from the process's own dictionary (DECISIONS A-10).
//
//   struct Order { int id; double px; std::string sym; };
//   std::string s = es::serreg::to_string(order);   // Order{id: 42, px: 101.5, sym: "AAPL"}
//   std::string j = es::serreg::to_json(order);     // {"id":42,"px":101.5,"sym":"AAPL"}
//   std::println("{}", order);                      // with `template<> struct es::serreg::formattable<Order>
//                                                   //            : std::true_type {};` (opt-in std::formatter)
//
// What it does, in order: it registers T (the same `touch<T>` a serialize call performs, so the type's ENTRY
// is in the ELF dictionary and in the next capture file whether or not T was ever serialised), encodes the
// value as a compact record (the bytes a frame would carry: an object's bytes, a dynamic type's canonical
// image, a scalar, a string), and hands them with T's schema hash to tools::render_value over
// tools::process_dictionary() — the registry's image, parsed once per registry version. The text is what print
// renders a struct argument as (`Type{field: value, …}`, enums by enumerator name, strings quoted); the JSON
// is srdump's value.
//
// Cost: an allocation for the record and for the string, a shared lock, and the dictionary-driven walk — a
// debugging and logging convenience, not for the hot path (serialize the value instead; a reader renders it).
// Links against es::serreg_tools (the renderer lives there, decision G).
#pragma once
#include <es/serreg/arg.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/serialize.hpp>
#include <es/serreg/tools.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace es::serreg
{

namespace detail
{
// The compact record of one value: what put_value_record writes for it in a frame (the vector is padded to 8
// as the frame would be; `payload` bytes are the record itself).
template<class T> result<std::vector<std::byte>> record_of(const T& v, std::size_t& payload)
{
    using U = std::remove_cvref_t<T>;
    payload = record_payload_bytes(v);
    if (payload >
        0xFFFF'FFFFu) // B7 (2.2.6): the measure saturates on an oversized or lying size(); refuse it
        return std::unexpected(error::invalid_argument);
    std::vector<std::byte> out(fmt::align8(payload));
    std::byte* p = out.data();
    put_value_record<U>(p, v, serialize_options{});
    return out;
}
} // namespace detail

// A value's rendering, or why it could not be rendered (unknown_type when T's entry is not in the process
// dictionary — not possible after touch<T>, kept for the dlclose edge; a decoder error for a corrupt image).
template<class T>
    requires(detail::serializable_arg<T> && !is_literal_v<std::remove_cvref_t<T>> &&
             !is_constant_v<std::remove_cvref_t<T>>)
result<std::string> render(const T& v, const tools::format f = tools::format::text)
{
    using U = std::remove_cvref_t<T>;
    detail::touch_arg<U>(); // registers U (and its component types) exactly as serialize would
    std::size_t payload = 0;
    const auto rec = detail::record_of(v, payload);
    if (!rec)
        return std::unexpected(rec.error());
    const std::shared_ptr<const dyn::dictionary> d = tools::process_dictionary();
    return tools::render_value(*d, arg_kind_v<U>, detail::arg_scalar_kind<U>(), arg_hash_v<U>,
                               std::span<const std::byte>(*rec).first(payload), f);
}

// to_string / to_json never fail on the caller: a rendering error becomes "<error: code>" in the string.
template<class T>
    requires requires(const T& v) { render(v); }
std::string to_string(const T& v)
{
    auto r = render(v, tools::format::text);
    return r ? std::move(*r) : "<error: " + std::string(to_string(r.error())) + ">";
}
template<class T>
    requires requires(const T& v) { render(v); }
std::string to_json(const T& v)
{
    auto r = render(v, tools::format::json);
    return r ? std::move(*r) : "{\"error\":\"" + std::string(to_string(r.error())) + "\"}";
}

// Opt-in std::formatter: specialise `formattable<T>` as std::true_type and `std::format("{}", v)` /
// `std::print("{}", v)` render T through to_string (spec `{:j}` gives the JSON form). Opt-in because a
// formatter for every serializable type would hijack std::format for types that already have one.
template<class T> struct formattable : std::false_type
{
};

} // namespace es::serreg

template<class T>
    requires(es::serreg::formattable<T>::value && requires(const T& v) { es::serreg::render(v); })
struct std::formatter<T, char>
{
    bool json = false;
    constexpr auto parse(std::format_parse_context& ctx)
    { // the spec is "" (text) or "j" (JSON); anything else is a compile error in std::format's checked
        // format string (the parser stops before the stray character) and is ignored by std::vformat
        auto it = ctx.begin();
        if (it != ctx.end() && *it == 'j')
        {
            json = true;
            ++it;
        }
        return it;
    }
    template<class Ctx> auto format(const T& v, Ctx& ctx) const
    {
        const std::string s = json ? es::serreg::to_json(v) : es::serreg::to_string(v);
        return std::ranges::copy(s, ctx.out()).out;
    }
};
