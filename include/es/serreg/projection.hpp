// es::serreg — schema evolution level 1 for flat types (R-9, DECISIONS; docs/schema-evolution-specs.md §2).
//
// A typed reader that meets a frame whose argument type W (the writer's, from a dictionary) is not the reader's
// T but *matches it by canonical name* (or through [[=es::serreg::renamed_from("ns::Old")]] on T) projects W into
// T member by member, by name:
//   R1 same kind → copy; R2/R3/R4 widen (iN→wider iN, uN→wider uN, uN→strictly wider iN, f32→f64);
//   R5 writer-only member → skipped; R6 reader-only member → the value it has in T{} (default member
//   initialisers apply); R7 anything else (narrowing, another family, bool↔integer) → the default is kept and the
//   member is reported; R8 enums by underlying kind; R9 nested flat classes by member name; R12 arrays of
//   scalars/enums that grew (M ≥ N); R13 [[=renamed_from("old")]] on a member; R14 renamed_from on a class.
// Scope here: *flat* T (memcpy-able native image) read from a *flat* W. Dynamic members (strings, vectors,
// optionals, variants, maps, sets) are level-0 only — a type with one keeps the exact-hash path.
// The plan for a (W, T) pair — a table of (source offset, destination offset, kinds, count) — is built once, on
// the heap, the first time the pair is met and kept for the life of the process (a lock-free list per T; nothing
// on the per-frame path allocates or locks). The annotations change neither the schema string nor the dictionary.
#pragma once

#include <es/serreg/arg.hpp>
#include <es/serreg/descriptor.hpp>
#include <es/serreg/deserialize.hpp>
#include <es/serreg/dictionary.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/reflect.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace es::serreg
{

// [[=es::serreg::renamed_from("px")]] on a member: frames whose writer called it `px` fill this member.
// [[=es::serreg::renamed_from("md::Quote")]] on a class: frames written as `md::Quote` project into it.
// Repeatable. Reader-side metadata only: not part of the schema string, not recorded in the dictionary.
template<std::size_t N> struct renamed_from
{
    char s[N]{};
    consteval renamed_from(const char (&a)[N]) noexcept
    {
        for (std::size_t i = 0; i < N; ++i)
            s[i] = a[i];
    }
    consteval std::string_view view() const noexcept
    {
        return {s, N - 1};
    }
};

namespace tools
{
// What one projection plan does, counted per reader member (arrays count once); `notes` has one line per member
// that is not a plain copy: "evo::Order.qty: u32 -> u64 widened", "evo::Order.flags: defaulted", "evo::Order.px:
// i64 -> i32 not projected (R7)".
struct projection_report
{
    std::uint32_t copied{}, widened{}, defaulted{}, skipped{}, conflicts{};
    std::vector<std::string> notes;
    void add(const projection_report& o)
    {
        copied += o.copied;
        widened += o.widened;
        defaulted += o.defaulted;
        skipped += o.skipped;
        conflicts += o.conflicts;
    }
};
} // namespace tools

namespace detail
{

// ---- renamed_from tables (compile time) ---------------------------------------------------------------------
template<std::meta::info M> consteval std::vector<std::string> old_names_of()
{
    std::vector<std::string> out;
    template for (constexpr std::meta::info a :
                  std::define_static_array(std::meta::annotations_of(M)))
    {
        constexpr std::meta::info ty = std::meta::remove_cv(std::meta::type_of(a));
        if constexpr (std::meta::has_template_arguments(ty) &&
                      std::meta::template_of(ty) == ^^es::serreg::renamed_from)
            out.emplace_back(std::meta::extract<typename[:ty:]>(a).view());
    }
    return out;
}
struct rename
{
    std::string_view member; // the current name
    std::string_view old;    // a name it had before
};
template<class T> consteval std::vector<rename> collect_renames()
{
    std::vector<rename> out;
    if constexpr (std::is_class_v<T>)
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&] {
                    if constexpr (!member_skipped<T, I>())
                        for (const std::string& o : old_names_of<members<T>()[I]>())
                            out.push_back(rename{std::define_static_string(part_name<T, I>()),
                                                 std::define_static_string(o)});
                }(),
                ...);
        }(std::make_index_sequence<part_count<T>()>{});
    return out;
}
template<class T> consteval bool renames_distinct()
{
    const auto v = collect_renames<T>();
    for (std::size_t i = 0; i < v.size(); ++i)
        for (std::size_t j = i + 1; j < v.size(); ++j)
            if (v[i].old == v[j].old && v[i].member != v[j].member)
                return false;
    return true;
}
template<class T> consteval bool renames_not_current()
{
    for (const rename& r : collect_renames<T>())
        for (const field_desc& f : descriptor_v<T>.fields)
            if (f.name == r.old)
                return false;
    return true;
}
template<class T> consteval std::size_t rename_count()
{
    return collect_renames<T>().size();
}
template<class T> consteval auto make_renames()
{
    static_assert(renames_distinct<T>(),
                  "es::serreg: two members of one type carry the same renamed_from name");
    static_assert(renames_not_current<T>(),
                  "es::serreg: renamed_from names a member the type still has");
    std::array<rename, rename_count<T>()> out{};
    const auto v = collect_renames<T>();
    for (std::size_t i = 0; i < v.size(); ++i)
        out[i] = v[i];
    return out;
}
template<class T> ES_SERREG_HIDDEN inline constexpr auto renames_v = make_renames<T>();
template<class T> consteval std::vector<std::string_view> collect_type_old_names()
{
    std::vector<std::string_view> out;
    if constexpr (std::is_class_v<T>)
        for (const std::string& o : old_names_of<std::meta::dealias(^^T)>())
            out.push_back(std::define_static_string(o));
    return out;
}
template<class T> consteval auto make_type_old_names()
{
    std::array<std::string_view, collect_type_old_names<T>().size()> out{};
    const auto v = collect_type_old_names<T>();
    for (std::size_t i = 0; i < v.size(); ++i)
        out[i] = v[i];
    return out;
}
template<class T>
ES_SERREG_HIDDEN inline constexpr auto type_old_names_v = make_type_old_names<T>();

// ---- the plan ----------------------------------------------------------------------------------------------
struct proj_step
{
    std::uint32_t src, dst;            // byte offsets in the writer's image / the reader's object
    kind sk, dk;                       // leaf kinds (struct_: a memcpy of `sstride` bytes)
    std::uint32_t n, sstride, dstride; // elements (1 for a scalar) and their strides
};
struct projection_plan
{
    std::uint64_t writer_hash{};
    std::uint32_t writer_size{};
    bool matched = false; // the writer's type is T by name (or renamed_from); false → hash_mismatch
    std::vector<proj_step> steps;
    tools::projection_report report;
};

constexpr int int_rank(const kind k) noexcept
{
    switch (k)
    {
    case kind::i8:
    case kind::u8:
        return 1;
    case kind::i16:
    case kind::u16:
        return 2;
    case kind::i32:
    case kind::u32:
        return 4;
    case kind::i64:
    case kind::u64:
        return 8;
    default:
        return 0;
    }
}
constexpr bool signed_int_kind(const kind k) noexcept
{
    return k == kind::i8 || k == kind::i16 || k == kind::i32 || k == kind::i64;
}
constexpr bool unsigned_int_kind(const kind k) noexcept
{
    return k == kind::u8 || k == kind::u16 || k == kind::u32 || k == kind::u64;
}
// 0 = not projectable (R7), 1 = copy (R1), 2 = widen (R2/R3/R4)
constexpr int leaf_rule(const kind sk, const kind dk) noexcept
{
    if (!is_scalar_kind(sk) || !is_scalar_kind(dk))
        return 0;
    if (sk == dk)
        return 1;
    if (signed_int_kind(sk) && signed_int_kind(dk) && int_rank(dk) > int_rank(sk))
        return 2;
    if (unsigned_int_kind(sk) && unsigned_int_kind(dk) && int_rank(dk) > int_rank(sk))
        return 2;
    if (unsigned_int_kind(sk) && signed_int_kind(dk) && int_rank(dk) > int_rank(sk))
        return 2;
    if (sk == kind::f32 && dk == kind::f64)
        return 2;
    return 0;
}
// One leaf: the bytes at src (kind sk) stored at dst (kind dk) under leaf_rule(sk, dk) != 0.
inline void convert_leaf(std::byte* const dst, const std::byte* const src, const kind sk,
                         const kind dk) noexcept
{
    if (sk == dk)
    {
        std::memcpy(dst, src, kind_size(sk));
        if (sk == kind::boolean && std::to_integer<unsigned>(*dst) > 1)
            *dst = std::byte{1};
        return;
    }
    if (sk == kind::f32)
    {
        float f;
        std::memcpy(&f, src, 4);
        const double d = f;
        std::memcpy(dst, &d, 8);
        return;
    }
    std::uint64_t u = 0;
    std::memcpy(&u, src, kind_size(sk));
    if (signed_int_kind(sk)) // sign-extend
    {
        const unsigned shift = 64 - 8 * kind_size(sk);
        u = static_cast<std::uint64_t>(static_cast<std::int64_t>(u << shift) >> shift);
    }
    std::memcpy(dst, &u,
                kind_size(dk)); // little-endian host (format.hpp): the low bytes are the value
}

inline const dyn::field* find_writer_field(const dyn::type& w, const std::string_view name,
                                           const std::span<const rename> renames) noexcept
{
    for (const dyn::field& f : w.fields)
        if (f.name == name)
            return &f;
    for (const rename& r : renames)
        if (r.member == name)
            for (const dyn::field& f : w.fields)
                if (f.name == r.old)
                    return &f;
    return nullptr;
}
inline const field_desc* find_reader_field(const type_desc& d, const std::string_view name) noexcept
{
    for (const field_desc& f : d.fields)
        if (f.name == name)
            return &f;
    return nullptr;
}
inline void plan_note(projection_plan& p, const std::string_view path, const std::string_view what)
{
    std::string s(path);
    s += ": ";
    s += what;
    p.report.notes.push_back(std::move(s));
}
inline std::string kinds_text(const kind sk, const kind dk)
{
    std::string s(to_string(sk));
    s += " -> ";
    s += to_string(dk);
    return s;
}

// A leaf or an array of leaves. `rk`/`wk` are the leaf kinds (an enum's underlying kind); n elements.
inline void plan_leaf(projection_plan& p, const std::string& path, const kind wk, const kind rk,
                      const std::uint32_t soff, const std::uint32_t doff, const std::uint32_t n,
                      const std::uint32_t sstride, const std::uint32_t dstride)
{
    const int rule = leaf_rule(wk, rk);
    if (rule == 0)
    {
        ++p.report.conflicts;
        plan_note(p, path, kinds_text(wk, rk) + " not projected (R7)");
        return;
    }
    p.steps.push_back(proj_step{soff, doff, wk, rk, n, sstride, dstride});
    if (rule == 2)
    {
        ++p.report.widened;
        plan_note(p, path, kinds_text(wk, rk) + " widened");
    }
    else
        ++p.report.copied;
}

// An array member (C array or std::array) of elements E: R12 — the writer's N elements into the reader's M ≥ N.
template<class E>
void plan_array(projection_plan& p, const dyn::dictionary& dict, const field_desc& rf,
                const dyn::field& wf, const std::uint32_t s, const std::uint32_t d,
                const std::string& path)
{
    static_assert(!std::is_array_v<E> && !is_std_array_v<E>,
                  "es::serreg: multi-dimensional arrays are not projected"); // keeps level 1 small
    constexpr auto esize = static_cast<std::uint32_t>(sizeof(E));
    if (wf.k != kind::array_ || wf.elem.size() != 1)
    {
        ++p.report.conflicts;
        plan_note(p, path, kinds_text(wf.k, kind::array_) + " not projected (R7)");
        return;
    }
    const std::uint32_t n = wf.extent, m = rf.extent;
    if (m < n)
    {
        ++p.report.conflicts;
        plan_note(p, path, "array shrank (R7)");
        return;
    }
    const dyn::field& we = wf.elem[0];
    if constexpr (std::is_class_v<E>)
    {
        const dyn::type* const wt = dict.find_type(we.type_hash);
        if (we.k != kind::struct_ || wt == nullptr || wt->layout_hash != layout_hash_v<E>)
        {
            ++p.report.conflicts;
            plan_note(p, path, "array of classes projects only with an equal layout (R7)");
            return;
        }
        p.steps.push_back(proj_step{s, d, kind::struct_, kind::struct_, n, esize, esize});
        ++p.report.copied;
    }
    else if constexpr (std::is_enum_v<E>)
    {
        const dyn::type* const wen = dict.find_type(we.type_hash);
        if (we.k != kind::enum_ || wen == nullptr)
        {
            ++p.report.conflicts;
            plan_note(p, path, "writer enum not in the dictionary (R7)");
            return;
        }
        plan_leaf(p, path, wen->underlying, scalar_kind<std::underlying_type_t<E>>(), s, d, n,
                  we.size, esize);
    }
    else
        plan_leaf(p, path, we.k, scalar_kind<E>(), s, d, n, we.size, esize);
    if (m > n)
        plan_note(p, path, "array grew: the new elements are defaulted (R12)");
}

template<class T>
void plan_class(projection_plan& p, const dyn::type& w, const dyn::dictionary& dict,
                std::uint32_t soff, std::uint32_t doff, const std::string& path);

// Projects the reader member R (a part or base of its enclosing class) from writer field wf, if any.
template<class R>
void plan_member(projection_plan& p, const dyn::dictionary& dict, const field_desc& rf,
                 const dyn::field* const wf, const std::uint32_t soff, const std::uint32_t doff,
                 const std::string& path)
{
    using U = std::remove_cv_t<R>;
    if (wf == nullptr)
    {
        ++p.report.defaulted;
        plan_note(p, path, "defaulted");
        return;
    }
    const std::uint32_t s = soff + wf->offset, d = doff + rf.offset;
    if constexpr (std::is_enum_v<U>)
    {
        if (wf->k != kind::enum_)
        {
            ++p.report.conflicts;
            plan_note(p, path, kinds_text(wf->k, kind::enum_) + " not projected (R7)");
            return;
        }
        const dyn::type* const we = dict.find_type(wf->type_hash);
        if (we == nullptr || !we->is_enum)
        {
            ++p.report.conflicts;
            plan_note(p, path, "writer enum not in the dictionary (R7)");
            return;
        }
        plan_leaf(p, path, we->underlying, scalar_kind<std::underlying_type_t<U>>(), s, d, 1, 0, 0);
    }
    else if constexpr (std::is_array_v<U>)
        plan_array<std::remove_cv_t<std::remove_extent_t<U>>>(p, dict, rf, *wf, s, d, path);
    else if constexpr (is_std_array_v<U>)
        plan_array<std::remove_cv_t<typename U::value_type>>(p, dict, rf, *wf, s, d, path);
    else if constexpr (std::is_class_v<U>)
    {
        if (wf->k != kind::struct_)
        {
            ++p.report.conflicts;
            plan_note(p, path, kinds_text(wf->k, kind::struct_) + " not projected (R7)");
            return;
        }
        const dyn::type* const wt = dict.find_type(wf->type_hash);
        if (wt == nullptr)
        {
            ++p.report.conflicts;
            plan_note(p, path, "writer type not in the dictionary (R7)");
            return;
        }
        if (wt->layout_hash == layout_hash_v<U>) // R1 for a whole nested object
        {
            p.steps.push_back(proj_step{s, d, kind::struct_, kind::struct_, 1,
                                        static_cast<std::uint32_t>(sizeof(U)),
                                        static_cast<std::uint32_t>(sizeof(U))});
            ++p.report.copied;
            return;
        }
        if (!wt->is_flat)
        {
            ++p.report.conflicts;
            plan_note(p, path, "writer type is not flat (R7)");
            return;
        }
        if (wt->name != descriptor_v<U>.name)
            plan_note(p, path, "nested type named " + wt->name + " (R9)");
        plan_class<U>(p, *wt, dict, s, d, path); // R9
    }
    else
        plan_leaf(p, path, wf->k, scalar_kind<U>(), s, d, 1, 0, 0);
}

template<class T>
void plan_class(projection_plan& p, const dyn::type& w, const dyn::dictionary& dict,
                const std::uint32_t soff, const std::uint32_t doff, const std::string& path)
{
    constexpr const type_desc& rd = descriptor_v<T>;
    [&]<std::size_t... B, std::size_t... I>(std::index_sequence<B...>, std::index_sequence<I...>) {
        ( // bases first, as "<base:Name>" pseudo-fields (matched by that name)
            [&] {
                const field_desc& rf = rd.fields[B];
                plan_member<base_t<T, B>>(p, dict, rf, find_writer_field(w, rf.name, renames_v<T>),
                                          soff, doff, path + '.' + std::string(rf.name));
            }(),
            ...);
        (
            [&] {
                if constexpr (!member_skipped<T, I>())
                {
                    static constexpr std::string_view nm =
                        std::define_static_string(part_name<T, I>());
                    const field_desc* const rf = find_reader_field(rd, nm);
                    if (rf != nullptr)
                        plan_member<part_t<T, I>>(p, dict, *rf,
                                                  find_writer_field(w, nm, renames_v<T>), soff,
                                                  doff, path + '.' + std::string(nm));
                }
            }(),
            ...);
    }(std::make_index_sequence<base_count<T>()>{}, std::make_index_sequence<part_count<T>()>{});
    for (const dyn::field& wf : w.fields) // R5: writer-only members
    {
        bool known = find_reader_field(rd, wf.name) != nullptr;
        for (const rename& r : renames_v<T>)
            known = known || r.old == wf.name;
        if (!known)
        {
            ++p.report.skipped;
            plan_note(p, path + '.' + wf.name, "writer-only member skipped (R5)");
        }
    }
}

template<class T> bool name_matches(const dyn::type& w) noexcept
{
    if (w.name == descriptor_v<T>.name)
        return true;
    for (const std::string_view o : type_old_names_v<T>)
        if (w.name == o)
            return true;
    return false;
}

template<class T>
std::unique_ptr<projection_plan> build_plan(const dyn::type& w, const dyn::dictionary& dict)
{
    auto p = std::make_unique<projection_plan>();
    p->writer_hash = w.schema_hash;
    p->writer_size = w.size;
    p->matched = name_matches<T>(w) && w.is_flat && !w.is_enum && !w.transparent;
    if (p->matched)
        plan_class<T>(*p, w, dict, 0, 0, std::string(descriptor_v<T>.name));
    return p;
}

// The plans of T, one per writer schema hash, kept for the life of the process.
template<class T> struct plan_cache
{
    struct node
    {
        std::unique_ptr<projection_plan> plan;
        node* next;
    };
    static inline std::atomic<node*> head{nullptr};
    // `built` is set when this call created the plan (its notes are new).
    static const projection_plan* get(const dyn::type& w, const dyn::dictionary& dict, bool& built)
    {
        built = false;
        for (node* n = head.load(std::memory_order_acquire); n != nullptr; n = n->next)
            if (n->plan->writer_hash == w.schema_hash)
                return n->plan.get();
        node* const n = new node{build_plan<T>(w, dict), nullptr};
        node* old = head.load(std::memory_order_acquire);
        do
            n->next = old;
        while (!head.compare_exchange_weak(old, n, std::memory_order_acq_rel,
                                           std::memory_order_acquire));
        built = true;
        return n->plan.get();
    }
};

// out = T{} then every step of the plan; `src` holds at least plan.writer_size bytes.
template<class T>
void apply_plan(const projection_plan& p, T& out, const std::byte* const src) noexcept
{
    out = T{};
    std::byte* const dst = reinterpret_cast<std::byte*>(std::addressof(out));
    for (const proj_step& s : p.steps)
        if (s.sk == kind::struct_)
            for (std::uint32_t i = 0; i < s.n; ++i)
                std::memcpy(dst + s.dst + i * s.dstride, src + s.src + i * s.sstride, s.sstride);
        else
            for (std::uint32_t i = 0; i < s.n; ++i)
                convert_leaf(dst + s.dst + i * s.dstride, src + s.src + i * s.sstride, s.sk, s.dk);
}

// A reader argument that can be projected: a flat class without as_flat, not a container shape.
template<class T>
constexpr bool projectable_v =
    std::is_class_v<T> && flat_type<T> && !has_as_flat<T> && !is_literal_v<T> &&
    !is_constant_v<T> && !is_string_arg_v<T> && std::is_default_constructible_v<T>;

} // namespace detail

namespace tools
{
// Fill a T from the bytes of a writer value of type `writer` (its ENTRY, from `dict`). hash_mismatch when the
// writer's type is not T by name; truncated when fewer than writer.size bytes are given. R7 members are not an
// error: they keep their default and are counted in `report`.
template<class T>
    requires detail::projectable_v<T>
result<T> project(const dyn::type& writer, const dyn::dictionary& dict,
                  const std::span<const std::byte> bytes, projection_report* const report = nullptr)
{
    detail::touch<T>();
    bool built = false;
    const detail::projection_plan* const p = detail::plan_cache<T>::get(writer, dict, built);
    if (!p->matched)
        return std::unexpected(error::hash_mismatch);
    if (bytes.size() < p->writer_size)
        return std::unexpected(error::truncated);
    T out;
    detail::apply_plan(*p, out, bytes.data());
    if (report != nullptr)
    {
        report->add(p->report);
        if (built)
            report->notes.insert(report->notes.end(), p->report.notes.begin(),
                                 p->report.notes.end());
    }
    return out;
}
} // namespace tools

// The dictionary a projecting reader resolves writer types through, and where it counts what it did.
struct projection_context
{
    const dyn::dictionary* dict;
    tools::projection_report* report = nullptr;
};

namespace detail
{
template<class... Ts>
decode_result<std::size_t> deserialize_projecting(const projection_context& ctx,
                                                  const std::span<const std::byte> bytes,
                                                  Ts&... outs)
{
    constexpr std::uint16_t expected = 1 + sizeof...(Ts);
    const auto fail = [&](const error e, const std::uint16_t done, const std::uint16_t failed) {
        return std::unexpected(decode_error{e, expected, done, failed});
    };
    const auto h = read_frame_header(bytes);
    if (!h)
        return fail(h.error(), 0, 0);
    if (h->meta())
        return fail(error::hash_mismatch, 0, 0);
    if (h->n_records != sizeof...(Ts))
        return fail(error::length_mismatch, 0, 0);
    const dyn::call* wcall = nullptr;
    if (!h->full())
    {
        wcall = ctx.dict->find_call(h->call_hash);
        if (wcall == nullptr)
            return fail(error::unknown_call, 0, 0);
        if (wcall->args.size() != sizeof...(Ts))
            return fail(error::hash_mismatch, 0, 0);
    }
    // Pass 1: resolve every record — exact (the writer's hash is the reader's) or a plan — and check the lengths
    // before anything is written (G2-1).
    struct slot
    {
        const projection_plan* plan = nullptr; // null: exact
        const std::byte* at = nullptr;         // the record's payload (after a full-record header)
        std::uint32_t len = 0;                 // payload bytes (plan: the writer's size)
        bool built = false;
    };
    slot slots[sizeof...(Ts) == 0 ? 1 : sizeof...(Ts)];
    const std::byte* p = bytes.data() + fmt::frame_header_size;
    const std::byte* const end = bytes.data() + h->payload_end();
    status e;
    std::uint16_t item = 0;
    auto resolve = [&]<class T>(std::type_identity<T>) {
        if (!e)
            return;
        slot& s = slots[item];
        const std::uint16_t i = ++item;
        std::uint64_t whash;
        std::uint32_t wlen = 0;
        if (h->full())
        {
            if (static_cast<std::size_t>(end - p) < fmt::full_record_header_size)
            {
                e = std::unexpected(error::truncated);
                return;
            }
            std::memcpy(&whash, p, 8);
            std::memcpy(&wlen, p + 8, 4);
        }
        else
            whash = wcall->args[i - 1].hash;
        if (whash == arg_hash<T>())
        { // exact: get_record steps over it in pass 2; here only its size is checked (as validate_frame does)
            std::size_t n = 0;
            if constexpr (is_literal_v<T> || is_constant_v<T>)
                n = h->full() ? fmt::align8(is_literal_v<T> ? 8 : sizeof(arg_value_t<T>)) : 0;
            else if constexpr (is_string_arg_v<T> || is_dynamic<T>())
            {
                constexpr std::size_t prefix =
                    is_string_arg_v<T> ? fmt::string_len_prefix : fmt::image_prefix;
                const std::byte* const q = p + (h->full() ? fmt::full_record_header_size : 0);
                if (static_cast<std::size_t>(end - q) < prefix)
                {
                    e = std::unexpected(error::truncated);
                    return;
                }
                std::uint32_t len;
                std::memcpy(&len, q, 4);
                n = fmt::align8(prefix + std::size_t{len});
            }
            else
                n = fmt::align8(record_payload_size<T>());
            if (h->full())
                n += fmt::full_record_header_size;
            if (static_cast<std::size_t>(end - p) < n)
            {
                e = std::unexpected(error::truncated);
                return;
            }
            if constexpr (is_constant_v<T>)
                if (h->full())
                { // 1.5: the value must be the one the reader names
                    const arg_value_t<T> want = T::value;
                    if (std::memcmp(p + fmt::full_record_header_size, &want, sizeof(want)) != 0)
                    {
                        e = std::unexpected(error::hash_mismatch);
                        return;
                    }
                }
            s.at = p;
            p += n;
            return;
        }
        if constexpr (projectable_v<T>)
        {
            if (h->full() || wcall->args[i - 1].ak == fmt::arg_type)
            {
                const dyn::type* const w = ctx.dict->find_type(whash);
                if (w == nullptr)
                {
                    e = std::unexpected(error::unknown_type);
                    return;
                }
                touch<T>();
                s.plan = plan_cache<T>::get(*w, *ctx.dict, s.built);
                if (!s.plan->matched)
                {
                    e = std::unexpected(error::hash_mismatch);
                    return;
                }
                if (h->full() && wlen != s.plan->writer_size)
                {
                    e = std::unexpected(error::length_mismatch);
                    return;
                }
                const std::size_t n = (h->full() ? fmt::full_record_header_size : 0) +
                                      fmt::align8(std::size_t{s.plan->writer_size});
                if (static_cast<std::size_t>(end - p) < n)
                {
                    e = std::unexpected(error::truncated);
                    return;
                }
                s.at = p + (h->full() ? fmt::full_record_header_size : 0);
                s.len = s.plan->writer_size;
                p += n;
                return;
            }
        }
        e = std::unexpected(error::hash_mismatch);
    };
    (resolve(std::type_identity<std::remove_cv_t<Ts>>{}), ...);
    if (!e)
        return fail(e.error(), item == 0 ? 0 : 1, item);
    // Pass 2: write the outputs.
    status soft;
    std::uint16_t done = 1, failed = 0;
    item = 0;
    auto one = [&](auto& out) {
        if (!e)
            return;
        slot& s = slots[item++];
        using T = std::remove_cv_t<std::remove_reference_t<decltype(out)>>;
        if (s.plan == nullptr)
        {
            const std::byte* q = s.at;
            e = get_record(q, end, out, h->full());
        }
        else if constexpr (projectable_v<T>)
        {
            apply_plan(*s.plan, out, s.at);
            if (ctx.report != nullptr)
            {
                ctx.report->add(s.plan->report);
                if (s.built)
                    ctx.report->notes.insert(ctx.report->notes.end(), s.plan->report.notes.begin(),
                                             s.plan->report.notes.end());
            }
        }
        if (e)
        {
            ++done;
            return;
        }
        if (failed == 0)
            failed = item;
        if (e.error() == error::invalid_value)
        {
            if (soft)
                soft = e;
            e = {};
        }
    };
    (one(outs), ...);
    if (!e)
        return fail(e.error(), done, failed);
    if (!soft)
        return fail(soft.error(), done, failed);
    return h->frame_len;
}
} // namespace detail

// Style 1 with a dictionary: as deserialize(bytes, outs...), and when the frame's call is not the reader's but
// each of its arguments either matches exactly or is a flat type that projects into the reader's argument by name,
// the frame is read through the projections (level 1). Compact frames need the writer's CALL entry in the
// dictionary; full-records frames carry the hashes themselves. Literals and constants must match exactly.
template<class... Ts>
    requires(detail::serializable_arg<std::remove_cvref_t<Ts>> && ...)
decode_result<std::size_t> deserialize(const projection_context& ctx,
                                       const std::span<const std::byte> bytes, Ts&&... outs)
{
    static_assert(
        ((is_literal_v<std::remove_cvref_t<Ts>> || is_constant_v<std::remove_cvref_t<Ts>> ||
          (std::is_lvalue_reference_v<Ts> && !std::is_const_v<std::remove_reference_t<Ts>>)) &&
         ...),
        "es::serreg::deserialize: non-literal outputs must be non-const lvalues");
    if (const auto h = read_frame_header(bytes);
        h && (h->full() || h->call_hash == call_hash_v<std::remove_cvref_t<Ts>...>))
    { // the exact path first: a full-records frame whose hashes all match costs nothing more than before
        const auto r = deserialize(bytes, static_cast<Ts&&>(outs)...);
        if (r || r.error().code != error::hash_mismatch || !h->full())
            return r;
    }
    return detail::deserialize_projecting(ctx, bytes, outs...);
}
template<class... Ts>
    requires(detail::serializable_arg<std::remove_cvref_t<Ts>> && ...)
decode_result<std::size_t> deserialize(const dyn::dictionary& dict,
                                       const std::span<const std::byte> bytes, Ts&&... outs)
{
    return deserialize(projection_context{&dict}, bytes, static_cast<Ts&&>(outs)...);
}

} // namespace es::serreg
