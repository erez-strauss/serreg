// es/serreg/detail/embed_elf.hpp — the embedded registry copy for ELF images (spec §6.8, FR-D6; split out of
// blob.hpp in 2.1.8, review addendum A4). Included only by <es/serreg/blob.hpp>, after the blob variables it
// copies; the default unless ES_SERREG_EMBED_NONE is defined (CMake ES_SERREG_IMAGE_FORMAT=none sets it).
//
// A TU-local copy of each blob in the read-only section `es_serreg_dict`, 8-aligned and contiguous. It is
// internal-linkage on purpose: GCC emits every COMDAT variable that names this section into ONE section carrying
// the group of whichever came first, which breaks multi-TU links; TU-local copies avoid that. Duplicates across TUs
// are harmless — readers key entries by hash. [[gnu::used, gnu::retain]] keep the copies under --gc-sections even
// though nothing references them at runtime. tools::extract_embedded_dictionary reads the section back.
#pragma once
#include <es/serreg/detail/blob_bytes.hpp>
#include <es/serreg/literal.hpp>

#define ES_SERREG_DICT_SECTION [[gnu::section("es_serreg_dict"), gnu::used, gnu::retain]]

namespace es::serreg::detail
{

template<class T>
ES_SERREG_DICT_SECTION alignas(8) static constexpr auto type_blob_copy_v = type_blob_bytes_v<T>;
template<fixed_string S>
ES_SERREG_DICT_SECTION alignas(8) static constexpr auto literal_blob_copy_v =
    literal_blob_bytes_v<S>;
template<class... Ts>
ES_SERREG_DICT_SECTION alignas(8) static constexpr auto call_blob_copy_v = call_blob_bytes_v<Ts...>;

// Called from the consteval descriptor factories: taking the address instantiates this TU's section copy.
template<class T> consteval void embed_type_blob()
{
    static_cast<void>(&type_blob_copy_v<T>);
}
template<class... Ts> consteval void embed_call_blob()
{
    static_cast<void>(&call_blob_copy_v<Ts...>);
}
template<fixed_string S> consteval void embed_literal_blob()
{
    static_cast<void>(&literal_blob_copy_v<S>);
}

} // namespace es::serreg::detail
