// es/serreg/detail/blob_bytes.hpp — the compile-time ENTRY blob bytes (format.hpp) of types, literals and call
// signatures, and call_arg_desc/call_desc. Split out of <es/serreg/blob.hpp> in 2.1.13 so that the executable-format
// backends (embed_elf.hpp, embed_none.hpp), which copy these bytes, include what they use and compile on their own;
// users include <es/serreg/blob.hpp> (or <es/serreg/serreg.hpp>).
#pragma once
#include <es/serreg/arg.hpp>
#include <es/serreg/descriptor.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/literal.hpp>
#include <es/serreg/reflect.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace es::serreg
{

struct call_arg_desc
{
    bool is_literal;    // ak == fmt::arg_literal (kept for readers written against 1.0)
    std::uint64_t hash; // schema hash | literal id | scalar name hash | "string" hash (arg.hpp)
    fmt::arg_kind ak;   // 1.1: the category
    kind k; // 1.1: scalar kind when ak == arg_scalar (1.5: or a scalar arg_const), else kind{}
    std::uint64_t value; // 1.5: arg_const — the constant's bytes, zero-extended; else 0
};
struct call_desc
{
    std::uint64_t hash;
    std::span<const call_arg_desc> args;
    std::span<const std::byte> blob;
    std::uint16_t flags; // 1.1: fmt::call_flag_print / call_flag_newline
};

// Call forms (1.1). A print form is a marker type at the front of the signature pack: it is not an argument,
// it selects the call string prefix ("print(" / "println(") and the call flags. print<…> adds it.
struct print_tag
{
};
struct println_tag
{
};
// 2.2.1 (1.7): a logger's call — rendered like println, flagged `log`; the envelope (level, file, line, options
// mask, timestamp, optional thread id and sequence) precedes the format literal in the signature (log.hpp).
struct log_tag
{
};

namespace detail
{

constexpr void put_field(cbuf& b, const field_desc& f)
{
    const std::uint16_t n_elem =
        (f.k == kind::array_ || f.k == kind::vector_ || f.k == kind::optional_ || f.k == kind::set_)
            ? 1
        : f.k == kind::map_ ? 2 // 1.6: key, value
                            : (f.k == kind::variant_ ? static_cast<std::uint16_t>(f.extent) : 0);
    b.put(static_cast<std::uint16_t>(f.name.size()));
    b.put(static_cast<std::uint8_t>(f.k));
    b.put(std::uint8_t{0});
    b.put(f.offset);
    b.put(f.size);
    b.put(f.align);
    b.put(f.since);
    b.put(f.deprecated);
    b.put(n_elem);
    b.put(f.type_hash);
    b.put(f.extent);
    for (const char c : f.name)
        b.v.push_back(static_cast<std::byte>(c));
    for (std::uint16_t i = 0; i < n_elem; ++i)
        put_field(b, f.elem[i]);
}

template<class T> consteval std::vector<std::byte> type_blob_bytes()
{
    constexpr type_desc d = make_type_desc_base<T>();
    cbuf b;
    if constexpr (std::is_enum_v<mapped_t<T>>)
    {
        b.begin_entry(fmt::tag_enum);
        b.put(d.schema_hash);
        b.put(d.layout_hash);
        b.put(d.size);
        b.put(d.align);
        b.put(static_cast<std::uint8_t>(d.underlying));
        b.put(static_cast<std::uint8_t>(d.enum_fixed ? fmt::enum_flag_fixed : 0)); // 1.6
        b.put(d.since);
        b.put(d.deprecated);
        b.put(static_cast<std::uint16_t>(d.name.size()));
        b.put(static_cast<std::uint16_t>(d.enumerators.size()));
        for (const char c : d.name)
            b.v.push_back(static_cast<std::byte>(c));
        for (const auto& e : d.enumerators)
        {
            b.put(e.value);
            b.str16(e.name);
        }
    }
    else
    {
        b.begin_entry(fmt::tag_struct);
        b.put(d.schema_hash);
        b.put(d.layout_hash);
        b.put(d.size);
        b.put(d.align);
        b.put(static_cast<std::uint8_t>((d.is_flat ? fmt::struct_flag_flat : 0) |
                                        (d.transparent ? fmt::struct_flag_transparent : 0)));
        b.put(std::uint8_t{0});
        b.put(d.since);
        b.put(d.deprecated);
        b.put(static_cast<std::uint16_t>(d.name.size()));
        b.put(static_cast<std::uint16_t>(d.fields.size()));
        for (const char c : d.name)
            b.v.push_back(static_cast<std::byte>(c));
        for (const auto& f : d.fields)
            put_field(b, f);
    }
    b.end_entry();
    return b.v;
}

template<class T>
constexpr bool is_form_tag_v =
    std::is_same_v<T, print_tag> || std::is_same_v<T, println_tag> || std::is_same_v<T, log_tag>;
template<class... Ts> consteval std::uint16_t call_flags()
{
    if constexpr (sizeof...(Ts) == 0)
        return 0;
    else
    {
        using F = std::tuple_element_t<0, std::tuple<Ts...>>;
        if constexpr (std::is_same_v<F, print_tag>)
            return fmt::call_flag_print;
        else if constexpr (std::is_same_v<F, println_tag>)
            return fmt::call_flag_print | fmt::call_flag_newline;
        else if constexpr (std::is_same_v<F, log_tag>)
            return fmt::call_flag_print | fmt::call_flag_newline | fmt::call_flag_log;
        else
            return 0;
    }
}
template<class... Ts> consteval std::size_t call_arity()
{
    return sizeof...(Ts) - (call_flags<Ts...>() != 0 ? 1 : 0);
}

template<class T> consteval call_arg_desc call_arg()
{
    return {arg_kind_of<T>() == fmt::arg_literal, arg_hash<T>(), arg_kind_of<T>(),
            arg_scalar_kind<T>(), arg_const_value<T>()};
}
template<class T> consteval void append_call_item(std::string& s)
{
    if constexpr (is_literal_v<T>)
    {
        s += "lit:";
        append_hex(s, T::id);
    }
    else if constexpr (is_constant_v<T>)
    { // 1.5: const:<scalar name | enum schema hash>=<value> — integers in decimal, bool as true/false, floating
        // values by their bit pattern (0x…, so -0.0, NaN payloads and rounding are all part of the identity)
        using V = arg_value_t<T>;
        s += "const:";
        if constexpr (std::is_enum_v<V> && !std::is_same_v<V, std::byte>)
        {
            append_hex(s, schema_hash_v<V>);
            s += '=';
            using U = std::underlying_type_t<V>;
            if constexpr (std::is_signed_v<U>)
                append_int(s, static_cast<std::int64_t>(static_cast<U>(T::value)));
            else
                append_uint(s, static_cast<std::uint64_t>(static_cast<U>(T::value)));
        }
        else
        {
            s += arg_name<V>();
            s += '=';
            if constexpr (std::is_same_v<V, bool>)
                s += T::value ? "true" : "false";
            else if constexpr (std::is_floating_point_v<V>)
                append_hex(s, const_bits(T::value));
            else if constexpr (std::is_signed_v<V>)
                append_int(s, static_cast<std::int64_t>(T::value));
            else
                append_uint(s, const_bits(T::value));
        }
    }
    else if constexpr (is_string_arg_v<T> || is_scalar_arg_v<T>)
        s += arg_name<T>();
    else
        append_hex(s, schema_hash_v<T>);
}
// call(n){item,…} | print(n){…} | println(n){…}  — n counts arguments, not the form tag.
template<class... Ts> consteval std::string call_string()
{
    constexpr std::uint16_t flags = call_flags<Ts...>();
    std::string s = flags == 0                         ? "call("
                    : (flags & fmt::call_flag_log)     ? "log("
                    : (flags & fmt::call_flag_newline) ? "println("
                                                       : "print(";
    append_uint(s, call_arity<Ts...>());
    s += "){";
    bool first = true;
    std::size_t i = 0;
    ((i++ == 0 && flags != 0
          ? void()
          : (s += first ? "" : ",", first = false, append_call_item<Ts>(s), void())),
     ...);
    s += '}';
    return s;
}
template<class... Ts> consteval std::array<call_arg_desc, call_arity<Ts...>()> make_call_args()
{
    std::array<call_arg_desc, call_arity<Ts...>()> out{};
    std::size_t i = 0, n = 0;
    ((i++ == 0 && call_flags<Ts...>() != 0 ? void() : void(out[n++] = call_arg<Ts>())), ...);
    return out;
}
template<class... Ts>
ES_SERREG_HIDDEN inline constexpr std::array<call_arg_desc, call_arity<Ts...>()> call_args_v =
    make_call_args<Ts...>();

template<class... Ts> consteval std::vector<std::byte> call_blob_bytes()
{
    cbuf b;
    b.begin_entry(fmt::tag_call);
    b.put(fingerprint64(call_string<Ts...>()));
    b.put(static_cast<std::uint16_t>(call_arity<Ts...>()));
    b.put(call_flags<Ts...>());
    b.put(std::uint32_t{0});
    for (const auto& a : call_args_v<Ts...>)
    {
        b.put(static_cast<std::uint8_t>(a.ak));
        b.put(static_cast<std::uint8_t>(
            a.ak == fmt::arg_scalar || a.ak == fmt::arg_const ? a.k : kind{}));
        for (int i = 0; i < 6; ++i)
            b.put(std::uint8_t{0});
        b.put(a.hash);
        if (a.ak == fmt::arg_const)
            b.put(a.value); // 1.5
    }
    b.end_entry();
    return b.v;
}

template<class T> struct type_blob_p
{
    static consteval std::vector<std::byte> bytes()
    {
        return type_blob_bytes<T>();
    }
};
template<class... Ts> struct call_blob_p
{
    static consteval std::vector<std::byte> bytes()
    {
        return call_blob_bytes<Ts...>();
    }
};

// Per-image entities. Hidden visibility keeps them out of the dynamic symbol table: no STB_GNU_UNIQUE
// binding (which would mark a DSO DF_1_NODELETE and make dlclose a silent no-op), and every image carries its
// own copy — exactly the per-image reference counting the registry does (FR-R4, FR-R12).
// The blobs referenced by descriptors/nodes: ordinary COMDAT-deduplicated .rodata (ODR-clean).
template<class T>
ES_SERREG_HIDDEN inline constexpr auto type_blob_bytes_v = to_blob_array<type_blob_p<T>>();
template<class... Ts>
ES_SERREG_HIDDEN inline constexpr auto call_blob_bytes_v = to_blob_array<call_blob_p<Ts...>>();

} // namespace detail
} // namespace es::serreg
