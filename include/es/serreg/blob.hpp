// es/serreg/blob.hpp — compile-time ENTRY blobs (format.hpp) for types, literals and call signatures, the
// embedded registry copy in the ELF section `es_serreg_dict` (spec §6.7, §6.8, D6), and the completed
// descriptor_v<T> / call_desc_v<Ts...> constants. One blob format, three carriers: this section, the RRI
// export and the dictionary inside captures. The blob bytes live in detail/blob_bytes.hpp (2.1.13), the
// executable-format part of the headers in detail/embed_elf.hpp or detail/embed_none.hpp (A4, 2.1.8).
#pragma once
#include <es/serreg/descriptor.hpp>
#include <es/serreg/detail/blob_bytes.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/literal.hpp>
#include <es/serreg/reflect.hpp>
#if defined(ES_SERREG_EMBED_NONE)
#include <es/serreg/detail/embed_none.hpp>
#else
#include <es/serreg/detail/embed_elf.hpp>
#endif

#include <cstdint>
#include <meta>
#include <span>
#include <string_view>
#include <type_traits>

namespace es::serreg
{
namespace detail
{

template<class T> consteval type_desc make_type_desc()
{
    embed_type_blob<T>(); // this TU's section copy (embed_elf.hpp), or nothing
    type_desc d = make_type_desc_base<T>();
    d.blob = type_blob_bytes_v<T>;
    return d;
}
template<class... Ts> consteval call_desc make_call_desc()
{
    embed_call_blob<Ts...>();
    return call_desc{fingerprint64(call_string<Ts...>()), call_args_v<Ts...>,
                     call_blob_bytes_v<Ts...>, call_flags<Ts...>()};
}
template<fixed_string S> consteval std::span<const std::byte> literal_blob_span()
{
    embed_literal_blob<S>();
    return literal_blob_bytes_v<S>;
}

} // namespace detail

// ---- public constants --------------------------------------------------------------------------------
template<class T>
ES_SERREG_HIDDEN inline constexpr type_desc descriptor_v =
    detail::make_type_desc<detail::mapped_t<T>>();
template<class... Ts>
inline constexpr std::uint64_t call_hash_v =
    fingerprint64(detail::call_string<std::remove_cvref_t<Ts>...>());
template<class... Ts>
ES_SERREG_HIDDEN inline constexpr std::string_view call_name_v =
    std::define_static_string(detail::call_string<std::remove_cvref_t<Ts>...>());
template<class... Ts>
ES_SERREG_HIDDEN inline constexpr call_desc call_desc_v =
    detail::make_call_desc<std::remove_cvref_t<Ts>...>();
template<fixed_string S>
ES_SERREG_HIDDEN inline constexpr std::span<const std::byte> literal_blob_v =
    detail::literal_blob_span<S>();

} // namespace es::serreg
