// src/decoder.cpp — tools::visit_record: descriptor-driven walk of one flat record payload through a
// decoded_value_visitor (spec §6.4 style 4, §6.1b flatten option, FR-D5 bounds checks everywhere).
#include "internal.hpp"

#include <es/serreg/tools.hpp>

#include <algorithm>
#include <atomic>

namespace es::serreg::tools
{
namespace
{
using internal::dec;
using internal::hex64;

// Guards against self-referential (corrupt) dictionaries. A struct level costs two units here (the member, then
// the struct), so 2.1.12 (review D-10) doubles the limit: a type nested as deep as the typed decoder accepts
// (fmt::max_nesting levels) decodes in srdump too.
// 2.1.16 (FR-D5): decode_limits::image_depth narrows it per process; fmt::max_nesting is the ceiling.
int max_depth() noexcept
{
    const std::uint32_t d = get_decode_limits().image_depth;
    return static_cast<int>(
        2 * std::min<std::uint32_t>(d, static_cast<std::uint32_t>(fmt::max_nesting)));
}

// Reads an enum value of `size` bytes, sign-extending for signed underlying kinds.
bool read_enum_value(const std::span<const std::byte> b, const kind underlying, std::int64_t& out)
{
    const bool is_signed = underlying == kind::i8 || underlying == kind::i16 ||
                           underlying == kind::i32 || underlying == kind::i64;
    std::uint64_t u = 0;
    switch (b.size())
    {
    case 1: {
        std::uint8_t v;
        std::memcpy(&v, b.data(), 1);
        u = v;
        if (is_signed)
            out = static_cast<std::int8_t>(v);
        break;
    }
    case 2: {
        std::uint16_t v;
        std::memcpy(&v, b.data(), 2);
        u = v;
        if (is_signed)
            out = static_cast<std::int16_t>(v);
        break;
    }
    case 4: {
        std::uint32_t v;
        std::memcpy(&v, b.data(), 4);
        u = v;
        if (is_signed)
            out = static_cast<std::int32_t>(v);
        break;
    }
    case 8: {
        std::uint64_t v;
        std::memcpy(&v, b.data(), 8);
        u = v;
        if (is_signed)
            out = static_cast<std::int64_t>(v);
        break;
    }
    default:
        return false;
    }
    if (!is_signed)
        out = static_cast<std::int64_t>(u);
    return true;
}

void emit_enum(const std::span<const std::byte> b, const dyn::type* const t,
               const std::string_view field_name, decoded_value_visitor& v)
{
    std::int64_t value = 0;
    read_enum_value(b, t ? t->underlying : kind::u8, value);
    std::string_view name;
    if (t)
        for (const auto& [n, val] : t->enumerators)
            if (val == value)
            {
                name = n;
                break;
            }
    v.enumerator(field_name, t ? std::string_view(t->name) : std::string_view{}, name, value);
}

struct walk_ctx;
result<void> walk_struct(std::span<const std::byte> payload, const dyn::type& t,
                         const dyn::dictionary& d, decoded_value_visitor& v,
                         std::string_view field_name, bool flatten, int depth, walk_ctx& c);

// v2: the text a variant alternative is announced with — variant<i:kind>
std::string alt_text(const dyn::field& a, const std::uint32_t i, const dyn::dictionary& d)
{
    std::string s = "variant<" + dec(i) + ":";
    if (a.k == kind::struct_ || a.k == kind::enum_)
    {
        const dyn::type* const t = d.find_type(a.type_hash);
        s += t ? t->name : hex64(a.type_hash);
    }
    else
        s += to_string(a.k);
    return s + ">";
}
// One walk of one record: the record image, the slot rules and byte budget shared with the typed decoder
// (fmt::image_budget, review G2-6), and whether the bytes are a native object representation (flat records).
struct walk_ctx
{
    fmt::image_budget budget;
    bool native;
    // 2.1.12 (review D-02): visits left for this record. A dictionary whose struct fields overlap can make a tiny
    // record fan out into K^L visits; every leaf of an honest record consumes at least one byte and sits at most
    // max_depth units deep (one visit per unit), so (2·max_depth + 2) visits per byte, plus room for empty
    // members, bound every honest walk.
    std::uint64_t steps;
    bool step() noexcept
    {
        if (steps == 0)
            return false;
        --steps;
        return true;
    }
};
std::uint64_t stride_of(const dyn::field& e)
{
    return fmt::slot_stride(e.size, e.align);
}

// Walks exactly the bytes of one value described by `f` (f.offset already applied by the caller). Dynamic members
// (v2) reference their tails relative to the start of the record image, c.budget.image.
result<void> walk_value(const std::span<const std::byte> b, const dyn::field& f,
                        const dyn::dictionary& d, decoded_value_visitor& v,
                        const std::string_view name, const bool flatten, const int depth,
                        walk_ctx& c)
{
    const std::span<const std::byte> image = c.budget.image;
    if (depth > max_depth())
        return std::unexpected(error::unsupported);
    if (!c.step())
        return std::unexpected(error::length_mismatch);
    if (is_scalar_kind(f.k))
    {
        if (f.k == kind::boolean && b.size() == 1 && std::to_integer<unsigned>(b[0]) > 1)
        { // G2-12: the byte reads as true, as in the typed decoder
            static constexpr std::byte one{1};
            v.noncanonical_bool(name, b[0]);
            v.scalar(name, f.k, {&one, 1});
            return {};
        }
        v.scalar(name, f.k, b);
        return {};
    }
    // a slot {u32 off, u32 n} inside `b` (the member's fixed part)
    const auto read_slot = [&](fmt::slot& s) { return fmt::read_slot(b, 0, s); };
    switch (f.k)
    {
    case kind::enum_:
        if (b.size() != 1 && b.size() != 2 && b.size() != 4 && b.size() != 8)
            return std::unexpected(error::length_mismatch);
        emit_enum(b, d.find_type(f.type_hash), name, v);
        return {};
    case kind::array_: {
        if (f.elem.size() != 1)
            return std::unexpected(error::corrupt_dictionary);
        const dyn::field& e = f.elem[0];
        // cc F2 / gk 1.15: a zero-size element with a non-zero extent would loop `extent` times over nothing;
        // otherwise never iterate more than b.size() / e.size elements (the product check in u64 is equivalent).
        if (e.size == 0)
        {
            if (f.extent != 0)
                return std::unexpected(error::length_mismatch);
        }
        else if (f.extent > b.size() / e.size)
            return std::unexpected(error::length_mismatch);
        v.begin_array(name, f.extent);
        // v2: dynamic elements are laid out at their stride
        const std::uint64_t stride = stride_of(e);
        for (std::uint32_t i = 0; i < f.extent; ++i)
        {
            const std::uint64_t at = std::uint64_t{i} * stride;
            if (at > b.size() || e.size > b.size() - at)
                return std::unexpected(error::length_mismatch);
            if (const auto r =
                    walk_value(b.subspan(at, e.size), e, d, v, {}, flatten, depth + 1, c);
                !r)
                return r;
        }
        v.end_array();
        return {};
    }
    case kind::struct_: {
        const dyn::type* const nested = d.find_type(f.type_hash);
        if (!nested)
            return std::unexpected(error::unknown_type);
        if (flatten && name.starts_with("<base:"))
        { // §6.1b: present base members inline
            for (const auto& nf : nested->fields)
            {
                if (static_cast<std::uint64_t>(nf.offset) + nf.size > b.size())
                    return std::unexpected(error::length_mismatch);
                if (const auto r = walk_value(b.subspan(nf.offset, nf.size), nf, d, v, nf.name,
                                              flatten, depth + 1, c);
                    !r)
                    return r;
            }
            return {};
        }
        return walk_struct(b, *nested, d, v, name, flatten, depth + 1, c);
    }
    // ---- v2 dynamic members: a reference slot into the image (plan v2.0 §2); rules in format.hpp ----
    case kind::string_: {
        fmt::slot s;
        if (!read_slot(s) || !c.budget.take(s, s.n))
            return std::unexpected(error::length_mismatch);
        v.string(name, {s.n ? reinterpret_cast<const char*>(image.data() + s.off) : "", s.n});
        return {};
    }
    case kind::vector_: {
        if (f.elem.size() != 1)
            return std::unexpected(error::corrupt_dictionary);
        const dyn::field& e = f.elem[0];
        fmt::slot s;
        const std::uint64_t stride = stride_of(e);
        // The block must hold every element; take_n refuses an n × stride that does not fit in 64 bits (2.1.12,
        // review D-01: a forged element size and alignment made the product wrap to a small block).
        // 2.1.3 (G2-6): the empty vector obeys the same rule as the typed decoder (n == 0 needs off == 0).
        if (!read_slot(s) || (s.n && e.size == 0) || !c.budget.take_n(s, stride))
            return std::unexpected(error::length_mismatch);
        v.begin_vector(name, s.n);
        for (std::uint32_t i = 0; i < s.n; ++i)
        {
            const std::uint64_t at =
                s.off + std::uint64_t{i} * stride; // inside the block checked above
            if (const auto r = walk_value(image.subspan(static_cast<std::size_t>(at), e.size), e, d,
                                          v, {}, flatten, depth + 1, c);
                !r)
                return r;
        }
        v.end_vector();
        return {};
    }
    case kind::map_:
    case kind::
        set_: { // 1.6: a block of `extent`-byte entries; the key at 0, a map's value at elem[1].offset
        const bool is_map = f.k == kind::map_;
        if (f.elem.size() != (is_map ? 2u : 1u) || f.extent == 0)
            return std::unexpected(error::corrupt_dictionary);
        const dyn::field& key = f.elem[0];
        fmt::slot s;
        if (!read_slot(s) || !c.budget.take_n(s, f.extent))
            return std::unexpected(error::length_mismatch);
        if (is_map)
            v.begin_map(name, s.n);
        else
            v.begin_set(name, s.n);
        for (std::uint32_t i = 0; i < s.n; ++i)
        {
            const std::uint64_t at =
                s.off + std::uint64_t{i} * f.extent; // inside the block checked above
            if (is_map)
            {
                const dyn::field& val = f.elem[1];
                v.begin_entry(i);
                if (const auto r = walk_value(image.subspan(static_cast<std::size_t>(at), key.size),
                                              key, d, v, "key", flatten, depth + 1, c);
                    !r)
                    return r;
                if (const auto r = walk_value(
                        image.subspan(static_cast<std::size_t>(at + val.offset), val.size), val, d,
                        v, "value", flatten, depth + 1, c);
                    !r)
                    return r;
                v.end_entry();
            }
            else if (const auto r =
                         walk_value(image.subspan(static_cast<std::size_t>(at), key.size), key, d,
                                    v, {}, flatten, depth + 1, c);
                     !r)
                return r;
        }
        if (is_map)
            v.end_map();
        else
            v.end_set();
        return {};
    }
    case kind::optional_: {
        if (f.elem.size() != 1)
            return std::unexpected(error::corrupt_dictionary);
        const dyn::field& e = f.elem[0];
        fmt::slot s;
        if (!read_slot(s) || s.n > 1 || !c.budget.take(s, e.size))
            return std::unexpected(error::length_mismatch);
        if (s.n == 0)
        {
            v.null_value(name);
            return {};
        }
        return walk_value(image.subspan(s.off, e.size), e, d, v, name, flatten, depth + 1, c);
    }
    case kind::variant_: {
        if (f.elem.size() != f.extent || f.extent == 0)
            return std::unexpected(error::corrupt_dictionary);
        fmt::slot s;
        if (!read_slot(s) || s.n > f.extent)
            return std::unexpected(error::length_mismatch);
        if (s.n == 0)
        { // valueless: shown as null here; the typed decoder picks std::monostate or reports invalid_value
            if (!c.budget.take(s, 0))
                return std::unexpected(error::length_mismatch);
            v.null_value(name);
            return {};
        }
        const dyn::field& a = f.elem[s.n - 1];
        if (!c.budget.take(s, a.size))
            return std::unexpected(error::length_mismatch);
        v.begin_variant(name, s.n - 1, alt_text(a, s.n - 1, d));
        if (const auto r =
                walk_value(image.subspan(s.off, a.size), a, d, v, "value", flatten, depth + 1, c);
            !r)
            return r;
        v.end_variant();
        return {};
    }
    default:
        return std::unexpected(error::unsupported); // literal members
    }
}

result<void> walk_struct(const std::span<const std::byte> payload, const dyn::type& t,
                         const dyn::dictionary& d, decoded_value_visitor& v,
                         const std::string_view field_name, const bool flatten, const int depth,
                         walk_ctx& c)
{
    if (depth > max_depth())
        return std::unexpected(error::unsupported);
    if (!c.step())
        return std::unexpected(error::length_mismatch);
    if (t.is_enum)
    {
        if (payload.size() != 1 && payload.size() != 2 && payload.size() != 4 &&
            payload.size() != 8)
            return std::unexpected(error::length_mismatch);
        emit_enum(payload, &t, field_name, v);
        return {};
    }
    if (payload.size() < t.size)
        return std::unexpected(error::length_mismatch);
    // 2.0.6: the visitor may take the whole value over (tools::walk_values decodes it into a compiled type).
    // The pointer difference is defined because every payload handed down here is a subspan of the image.
    if (v.take_struct(
            field_name, t,
            value_location{payload.first(t.size), c.budget.image,
                           static_cast<std::size_t>(payload.data() - c.budget.image.data()),
                           c.native, &c.budget}))
        return {};
    if (t.transparent)
    { // 1.6: a container argument — the record *is* the container; renderers show the value alone
        if (t.fields.size() != 1 || t.fields[0].size > payload.size())
            return std::unexpected(error::corrupt_dictionary);
        v.begin_transparent(t.name, field_name);
        if (const auto r = walk_value(payload.first(t.fields[0].size), t.fields[0], d, v,
                                      field_name, flatten, depth + 1, c);
            !r)
            return r;
        v.end_transparent();
        return {};
    }
    v.begin_struct(t.name, field_name);
    for (const auto& f : t.fields)
    {
        if (static_cast<std::uint64_t>(f.offset) + f.size > payload.size())
            return std::unexpected(error::length_mismatch);
        if (const auto r = walk_value(payload.subspan(f.offset, f.size), f, d, v, f.name, flatten,
                                      depth + 1, c);
            !r)
            return r;
    }
    v.end_struct();
    return {};
}

} // namespace

result<void> visit_record(const std::span<const std::byte> payload, const dyn::type& t,
                          const dyn::dictionary& d, decoded_value_visitor& v,
                          const std::string_view field_name, const bool flatten)
{
    // a dynamic type's payload is its image (its fixed part is t.size bytes; every tail follows it); a flat type's
    // payload is its native object representation and has no slots
    walk_ctx c{fmt::image_budget(payload, t.size), t.is_flat,
               static_cast<std::uint64_t>(2 * max_depth() + 2) *
                       (std::uint64_t{payload.size()} + 1) +
                   65536};
    return walk_struct(payload, t, d, v, field_name, flatten, 0, c);
}

// ---- warnings (error.hpp, 2.1.3) ----------------------------------------------------------------------------
namespace
{
constinit std::atomic<warning_handler> g_warning_handler{nullptr};
}
} // namespace es::serreg::tools

namespace es::serreg
{
warning_handler set_warning_handler(const warning_handler h) noexcept
{
    return tools::g_warning_handler.exchange(h, std::memory_order_acq_rel);
}
namespace detail
{
[[gnu::cold]] void report_warning(const warning w, const std::uint64_t schema_hash,
                                  const std::uint32_t count) noexcept
{
    if (const warning_handler h = tools::g_warning_handler.load(std::memory_order_acquire))
        h(w, schema_hash, count);
}
} // namespace detail

} // namespace es::serreg
