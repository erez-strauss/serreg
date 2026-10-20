// es/serreg/detail/embed_none.hpp — no embedded registry copy (review addendum A4, 2.1.8). Included by
// <es/serreg/blob.hpp> when ES_SERREG_EMBED_NONE is defined (CMake ES_SERREG_IMAGE_FORMAT=none; or by hand for a
// toolchain without named sections). The program then carries no `es_serreg_dict` section: offline tools need an
// .rri file (Registry::export_image, `srdump --rri`) or a capture, which embeds its dictionary anyway.
// Descriptors, hashes and the wire format are identical to the ELF build — only the section copy is missing.
#pragma once
#include <es/serreg/detail/blob_bytes.hpp>
#include <es/serreg/literal.hpp>

namespace es::serreg::detail
{

template<class T> consteval void embed_type_blob()
{
}
template<class... Ts> consteval void embed_call_blob()
{
}
template<fixed_string S> consteval void embed_literal_blob()
{
}

} // namespace es::serreg::detail
