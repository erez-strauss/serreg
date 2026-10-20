// es/serreg/walk.hpp — tools::walk_values<Ts...> (v2.0.6): walk every value of a capture, typed where the
// reader was compiled with the type, generic where it was not.
//
//   tools::walk_values<demo::Order, demo::Fill>(capture,
//       overloaded{[](std::string_view field, const demo::Order& o) { ... },   // decoded into the C++ type
//                  [](std::string_view field, const demo::Fill& f) { ... }},
//       rest);                                                             // decoded_value_visitor for the rest
//
// For every record, and then for every nested struct value (member, array/vector element, variant alternative),
// outermost first: when the value's schema hash (from the file's dictionary) equals schema_hash_v<T> of one of
// Ts..., the value is decoded into a T and handed to on_value(field_name, const T&), and nothing inside it is
// walked; otherwise `rest` receives the value field by field (begin_struct / scalar / string / begin_vector / ...)
// and the same test is applied one level down. Literal, scalar and string records go to `rest` as well
// (literal / scalar / string events, field names "[i]"). An equal schema hash means the same names, types and
// offsets, so the typed decode is exact; a type that changed since the file was written simply does not match
// and is walked generically.
//
// Why Ts... and not "the compiled registry": the registry knows which hashes this program has, but only a
// template argument gives the C++ type to decode into and call on_value with.
#pragma once

#include <es/serreg/deserialize.hpp>
#include <es/serreg/reflect.hpp>
#include <es/serreg/tools.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace es::serreg::tools
{

namespace detail
{
// Decodes the value at `where` into `out`: native bytes (flat records) or the canonical image (dynamic records).
// 2.1.3: the image decode charges the walk's budget (G2-6) and reports a valueless variant that T cannot hold as
// invalid_value; bool bytes are repaired as everywhere else (G2-12).
template<class T> status decode_at(T& out, const value_location& where)
{
    if (where.native)
    {
        if constexpr (flat_type<T>)
        {
            if (where.bytes.size() < sizeof(serreg::detail::mapped_t<T>))
                return std::unexpected(error::length_mismatch);
            if (!serreg::detail::get_object(out, where.bytes.data()))
                return std::unexpected(
                    error::invalid_value); // 2.1.16: an enum leaf outside its range
            return {};
        }
        else
            return std::unexpected(error::unsupported); // a dynamic type is never stored natively
    }
    fmt::image_budget fresh(where.image, 0);
    serreg::detail::decode_ctx c(where.image, where.budget ? where.budget->fixed : 0);
    c.left = (where.budget ? *where.budget : fresh).left;
    const bool ok = serreg::detail::decode_value(out, c, where.image_offset, 0);
    if (where.budget)
        where.budget->left = c.left;
    if (!ok)
        return std::unexpected(error::length_mismatch);
    if (c.bools)
        serreg::detail::report_warning(warning::noncanonical_bool, schema_hash_v<T>, c.bools);
    if (c.invalid)
        return std::unexpected(error::invalid_value);
    return {};
}

// Forwards every event to `rest` and takes over the struct values whose schema hash is one of Ts...
template<class F, class... Ts> struct typed_adapter final : decoded_value_visitor
{
    F& on_value;
    decoded_value_visitor& rest;
    std::vector<std::string>& problems;
    std::size_t typed = 0,
                fallbacks = 0; // values delivered typed; hash matches whose decode failed
    std::uint64_t frame = 0;   // index of the frame being walked (for the fallback problem text)
    std::string record;        // "[i]" of the record being walked

    typed_adapter(F& f, decoded_value_visitor& r, std::vector<std::string>& p) noexcept
        : on_value(f), rest(r), problems(p)
    {
    }

    bool take_struct([[maybe_unused]] const std::string_view field,
                     [[maybe_unused]] const dyn::type& t,
                     [[maybe_unused]] const value_location& where) override
    {
        bool taken = false;
        (void)((t.schema_hash == schema_hash_v<Ts> && (taken = deliver<Ts>(field, where), true)) ||
               ...);
        return taken;
    }
    template<class T> bool deliver(const std::string_view field, const value_location& where)
    {
        T v{};
        if (const auto d = decode_at(v, where); !d)
        { // G2-9: reported, then walked field by field (a valueless variant shows as null there)
            ++fallbacks;
            problems.push_back(
                "frame " + std::to_string(frame) + ": record " + record + ": " +
                (field.empty() ? std::string("value") : std::string(field)) + ": " + t_name<T>() +
                " decoded generically (typed decode: " + std::string(to_string(d.error())) + ")");
            return false;
        }
        ++typed;
        on_value(field, static_cast<const T&>(v));
        return true;
    }

    void begin_struct(const std::string_view type_name, const std::string_view field) override
    {
        rest.begin_struct(type_name, field);
    }
    void end_struct() override
    {
        rest.end_struct();
    }
    void begin_array(const std::string_view field, const std::uint32_t n) override
    {
        rest.begin_array(field, n);
    }
    void end_array() override
    {
        rest.end_array();
    }
    void scalar(const std::string_view field, const kind k,
                const std::span<const std::byte> b) override
    {
        rest.scalar(field, k, b);
    }
    void enumerator(const std::string_view field, const std::string_view type,
                    const std::string_view name, const std::int64_t value) override
    {
        rest.enumerator(field, type, name, value);
    }
    void literal(const std::string_view field, const std::uint64_t id,
                 const std::string_view text) override
    {
        rest.literal(field, id, text);
    }
    void string(const std::string_view field, const std::string_view s) override
    {
        rest.string(field, s);
    }
    void begin_vector(const std::string_view field, const std::uint32_t n) override
    {
        rest.begin_vector(field, n);
    }
    void end_vector() override
    {
        rest.end_vector();
    }
    void null_value(const std::string_view field) override
    {
        rest.null_value(field);
    }
    void begin_variant(const std::string_view field, const std::uint32_t index,
                       const std::string_view alt) override
    {
        rest.begin_variant(field, index, alt);
    }
    void begin_map(const std::string_view field, const std::uint32_t n) override
    {
        rest.begin_map(field, n);
    }
    void begin_entry(const std::uint32_t i) override
    {
        rest.begin_entry(i);
    }
    void end_entry() override
    {
        rest.end_entry();
    }
    void end_map() override
    {
        rest.end_map();
    }
    void begin_set(const std::string_view field, const std::uint32_t n) override
    {
        rest.begin_set(field, n);
    }
    void end_set() override
    {
        rest.end_set();
    }
    void begin_transparent(const std::string_view type, const std::string_view field) override
    {
        rest.begin_transparent(type, field);
    }
    void end_transparent() override
    {
        rest.end_transparent();
    }
    void end_variant() override
    {
        rest.end_variant();
    }
    void noncanonical_bool(const std::string_view field, const std::byte raw) override
    {
        rest.noncanonical_bool(field, raw);
    }
    template<class T> static std::string t_name()
    {
        return std::string(canonical_name_v<T>);
    }
};

// The records of one frame through the adapter. Returns false when a record could not be walked (reported).
template<class Adapter>
bool walk_frame(const dyn::dictionary& dict, const frame_ref& f, Adapter& a,
                std::vector<std::string>& problems)
{
    const auto fv = inspect(f.bytes, &dict);
    if (!fv)
    {
        problems.push_back("frame " + std::to_string(f.index) + ": " +
                           std::string(to_string(fv.error())));
        return false;
    }
    std::uint32_t i = 0;
    bool ok = true;
    a.frame = f.index;
    for (const record_view& r : fv->records())
    {
        const std::string name = "[" + std::to_string(i++) + "]";
        a.record = name;
        switch (r.ak)
        {
        case fmt::arg_literal: {
            const dyn::literal* const l = dict.find_literal(r.hash);
            a.literal(name, r.hash, l ? std::string_view(l->text) : std::string_view{});
            break;
        }
        case fmt::arg_scalar:
            a.scalar(name, r.k, r.payload);
            break;
        case fmt::arg_string:
            a.string(name, r.text());
            break;
        case fmt::
            arg_const: // never: inspect() presents a constant as the scalar or enum it stands for
            break;
        case fmt::arg_type: {
            const dyn::type* const t = dict.find_type(r.hash);
            const auto v = t ? visit_record(r.payload, *t, dict, a, name)
                             : result<void>(std::unexpected(error::unknown_type));
            if (!v)
            {
                problems.push_back("frame " + std::to_string(f.index) + ": record " + name + ": " +
                                   std::string(to_string(v.error())));
                ok = false;
            }
            break;
        }
        }
    }
    return ok;
}
} // namespace detail

// Walks `capture` (a FILE or a bare frame stream, e.g. a mapped_file's bytes). on_frame, when given, is called
// before each frame's records and may return false to skip that frame. Returns the for_each_frame summary, with
// records that could not be walked added to its problems.
template<class... Ts, class F>
result<frame_walk> walk_values(const std::span<const std::byte> capture, F&& on_value,
                               decoded_value_visitor& rest, const frame_callback& on_frame = {},
                               const dyn::dictionary* const extra = nullptr)
{
    static_assert((std::is_class_v<Ts> && ...),
                  "walk_values<Ts...>: Ts are the struct types to decode");
    static_assert((std::is_default_constructible_v<Ts> && ...),
                  "walk_values<Ts...>: Ts must be default-constructible");
    std::vector<std::string> problems;
    detail::typed_adapter<std::remove_reference_t<F>, Ts...> a(on_value, rest, problems);
    auto w = for_each_frame(
        capture,
        [&](const dyn::dictionary& d, const frame_ref& f) {
            if (on_frame && !on_frame(d, f))
                return true; // skip this frame, keep walking
            detail::walk_frame(d, f, a, problems);
            return true;
        },
        extra);
    if (w)
    {
        w->problems.insert(w->problems.end(), problems.begin(), problems.end());
        w->typed = a.typed;
        w->fallbacks = a.fallbacks;
    }
    return w;
}

// 2.1.10: the same without compiled types — every value goes to `rest` (a dictionary-only reader).
inline result<frame_walk> walk_values(const std::span<const std::byte> capture,
                                      decoded_value_visitor& rest,
                                      const frame_callback& on_frame = {},
                                      const dyn::dictionary* const extra = nullptr)
{
    return walk_values<>(capture, [](std::string_view, const auto&) {}, rest, on_frame, extra);
}

// The same over a file read in blocks (for_each_frame_in_file): bounded memory, pipes and stdin.
template<class... Ts, class F>
result<frame_walk> walk_values_in_file(const std::string_view path, F&& on_value,
                                       decoded_value_visitor& rest,
                                       const frame_callback& on_frame = {},
                                       const block_options& o = {},
                                       const dyn::dictionary* const extra = nullptr)
{
    std::vector<std::string> problems;
    detail::typed_adapter<std::remove_reference_t<F>, Ts...> a(on_value, rest, problems);
    auto w = for_each_frame_in_file(
        path,
        [&](const dyn::dictionary& d, const frame_ref& f) {
            if (on_frame && !on_frame(d, f))
                return true;
            detail::walk_frame(d, f, a, problems);
            return true;
        },
        o, extra);
    if (w)
    {
        w->problems.insert(w->problems.end(), problems.begin(), problems.end());
        w->typed = a.typed;
        w->fallbacks = a.fallbacks;
    }
    return w;
}

// 2.1.10: a capture on disk in one call — mapped, or read in blocks (file_options, as for_each_frame(path, …)).
template<class... Ts, class F>
result<frame_walk> walk_values(const std::string_view path, F&& on_value,
                               decoded_value_visitor& rest, const frame_callback& on_frame = {},
                               const file_options& o = {},
                               const dyn::dictionary* const extra = nullptr)
{
    if (!o.blocks && path != "-")
    {
        const auto m = mapped_file::open(path, o.populate);
        if (m)
            return walk_values<Ts...>(m->bytes(), on_value, rest, on_frame, extra);
        if (m.error() != error::unsupported)
            return std::unexpected(m.error());
    }
    return walk_values_in_file<Ts...>(path, on_value, rest, on_frame,
                                      block_options{o.block_bytes, o.limits}, extra);
}
// …and without compiled types.
inline result<frame_walk> walk_values(const std::string_view path, decoded_value_visitor& rest,
                                      const frame_callback& on_frame = {},
                                      const file_options& o = {},
                                      const dyn::dictionary* const extra = nullptr)
{
    return walk_values<>(path, [](std::string_view, const auto&) {}, rest, on_frame, o, extra);
}

} // namespace es::serreg::tools
