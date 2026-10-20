// es/serreg/serialize.hpp — serialize(): one frame per call (spec §6.4, §6.5, FR-S1..S6, FR-W2).
//
// Hot path for flat arguments into a claiming_sink: claim(frame_size<Ts...>()), 16-byte header, memcpy per
// object, zero the padding gaps and normalise bools from the compile-time tables, literal ids, commit. No
// allocation, no lock, no registry access — registration happened at image load (registry.hpp).
//
// Plain sinks (write-only): frames up to 64 KiB are assembled in an exactly-sized stack buffer and written
// once; larger frames are streamed record by record through a 16 KiB chunk buffer (gap scrub applied per
// chunk), so stack use is bounded for any object size and nothing is heap-allocated (gk 1.3, cc F22).
// Writing stops at the first sink failure. A sink may expose `status state() const`; its error is returned
// instead of error::sink_full (fd_sink, capture_file do).
//
// Hot-path rules (what can allocate, 2.2.6): a claiming sink (capture_file, es::ipc, span_sink) builds the frame
// in place and never allocates for flat, scalar, string or literal arguments; for a dynamic argument the only
// allocation left is the canonical sort's order table of an unordered container (or a multimap shape sorted by
// key) above 64 entries — detail::may_sort_v marks those types and serialize_noexcept excludes them. A plain
// sink assembles a frame with dynamic arguments on the stack up to 4 KiB; above that it streams record by record
// and heap-buffers a dynamic image above the 16 KiB chunk, or the whole frame when a CRC trailer is requested
// (the trailer covers the frame, so it cannot be streamed). serialize_options::canonical_unordered = false is the
// escape hatch for the sort: no order table at any size, at the price that two captures of one value can then
// differ in bytes (readers accept both orders).
#pragma once
#include <es/serreg/arg.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/sink.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace es::serreg
{

struct serialize_options
{
    bool scrub_padding = true; // FR-S6 (D3): deterministic bytes (padding zeroed, bools normalised)
    bool full_records = false; // FR-W2: per-record schema hash + length (archival); default compact
    // 1.7 (2.2.1): an 8-byte CRC32C trailer per frame (frame flag crc). A sink may ask for it on every frame it
    // receives (capture_file with {.crc = true}: detail::sink_wants_crc); the frame then carries the flag and the
    // readers check it. Off: zero bytes, zero work.
    bool crc = false;
    // 1.6 containers: an unordered container (std::unordered_map/_set/_multimap, a map with a non-std::less
    // comparator) is sorted by key on the wire, so identical values give identical bytes. `false` writes it in
    // its iteration order instead: no sort, no allocation above 64 entries; same value, possibly different
    // bytes; every reader accepts both (a reader's own container decides its order). Hashes are unaffected.
    bool canonical_unordered = true;
};

inline constexpr std::size_t stack_frame_limit = 64 * 1024; // largest frame assembled on the stack
inline constexpr std::size_t stream_chunk_size = 16 * 1024; // chunk for streaming larger records

namespace detail
{

// Payload size of one record: exact for types, literals and scalars; the *minimum* (an empty string: its u32
// length prefix) for a runtime string — record_bytes(v) below is exact for everything. A literal's payload is
// its u64 id in full-records mode; in a compact frame the call signature already names it and it occupies
// no bytes at all (FR-L6, format 1.1) — record_size()/record_bytes() apply that.
template<class T> constexpr std::size_t record_payload_size()
{
    if constexpr (is_literal_v<T>)
        return 8;
    else if constexpr (is_constant_v<T>)
        return record_payload_size<
            arg_value_t<T>>(); // 1.5: in a full-records frame, the value's record
    else if constexpr (is_string_arg_v<T>)
        return fmt::string_len_prefix;
    else if constexpr (is_scalar_arg_v<T>)
        return sizeof(T);
    else if constexpr (is_dynamic<T>())
        return fmt::
            image_prefix; // v2: minimum (u32 len, u32 0); the image length is a runtime value
    else
        return sizeof(mapped_t<T>);
}
template<class T> constexpr std::size_t record_size(const bool full)
{
    if constexpr (is_literal_v<T>)
        return full ? fmt::full_record_header_size + 8 : 0; // FR-L6
    else if constexpr (is_constant_v<T>)
        return full ? record_size<arg_value_t<T>>(true) : 0; // 1.5: the signature carries the value
    else
        return (full ? fmt::full_record_header_size : 0) + fmt::align8(record_payload_size<T>());
}
template<class T> inline std::size_t record_payload_bytes(const T& v) noexcept
{
    if constexpr (is_string_arg_v<std::remove_cvref_t<T>>)
        return fmt::string_len_prefix + string_bytes(v).size();
    else if constexpr (is_dynamic<std::remove_cvref_t<T>>())
        return fmt::image_prefix + measure_image(v); // v2: one measure pass, no allocation
    else
        return record_payload_size<std::remove_cvref_t<T>>();
}

template<class T> inline std::size_t record_bytes(const T& v, const bool full) noexcept
{
    if constexpr (is_literal_v<std::remove_cvref_t<T>> || is_constant_v<std::remove_cvref_t<T>>)
        return record_size<std::remove_cvref_t<T>>(full);
    else
        return (full ? fmt::full_record_header_size : 0) + fmt::align8(record_payload_bytes(v));
}

template<class S>
concept sink_with_state = requires(const S& s) {
    { s.state() } -> std::same_as<status>;
};
template<class S> error sink_failure(const S& s) noexcept
{
    if constexpr (sink_with_state<S>)
    {
        const status st = s.state();
        return st ? error::sink_full : st.error(); // ok state + refused write = plain sink_full
    }
    else
        return error::sink_full;
}
template<class S> constexpr bool claim_noexcept = true; // plain sinks: nothing more to check
template<claiming_sink S>
constexpr bool claim_noexcept<S> = noexcept(std::declval<S&>().claim(std::size_t{0})) &&
                                   noexcept(std::declval<S&>().commit(std::size_t{0}));
template<class S>
constexpr bool sink_noexcept =
    noexcept(std::declval<S&>().write(static_cast<const std::byte*>(nullptr), std::size_t{0})) &&
    claim_noexcept<S>;
// G2-7 (2.1.1): a dynamic argument written to a plain (non-claiming) sink may be assembled in a heap buffer
// (stream_record, records above stream_chunk_size), so that combination is not noexcept; claiming sinks build the
// image in claimed memory and flat/scalar/string/literal arguments never allocate.
// 2.2.6 (review B1): the canonical sort of an unordered container (or a multimap shape sorted by key) builds its
// order table on the heap above 64 entries, on every sink — so an argument that may sort (detail::may_sort_v,
// reflect.hpp: the type or anything it contains) is never noexcept; a std::bad_alloc from that table propagates
// instead of terminating. canonical_unordered = false avoids the sort at run time but not the specification.
template<class... Ts> constexpr bool any_may_sort()
{ // a loop, not a fold: -Wlogical-op objects to `a || a` when one type repeats in the pack
    constexpr bool each[] = {false, may_sort_v<std::remove_cvref_t<Ts>>...};
    for (const bool b : each)
        if (b)
            return true;
    return false;
}
template<class... Ts> constexpr bool any_may_sort_v = any_may_sort<Ts...>();
template<class S, class... Ts>
constexpr bool serialize_noexcept =
    sink_noexcept<S> && !any_may_sort_v<Ts...> &&
    (claiming_sink<S> || !(dynamic_type<std::remove_cvref_t<Ts>> || ...));

// Scrub the bytes [off, off+len) of an object image at `dst` (dst points at byte `off` of the image).
inline void scrub_range(std::byte* const dst, const type_desc& d, const std::size_t off,
                        const std::size_t len) noexcept
{
    for (const gap g : d.padding_gaps)
    {
        const std::size_t lo = g.offset > off ? g.offset : off;
        const std::size_t hi = (g.offset + g.len < off + len) ? g.offset + g.len : off + len;
        if (lo < hi)
            std::memset(dst + (lo - off), 0, hi - lo);
    }
    for (const std::uint32_t b : d.bool_offsets)
        if (b >= off && b < off + len)
            dst[b - off] = dst[b - off] == std::byte{0} ? std::byte{0} : std::byte{1};
}

// Copy one whole flat object into `dst` (sized sizeof(U)), applying as_flat and the scrub.
template<class T>
inline void put_object(std::byte* const dst, const T& v, const bool scrub) noexcept
{
    using U = mapped_t<T>;
    if constexpr (has_as_flat<std::remove_cv_t<T>>)
    {
        const U u = as_flat<std::remove_cv_t<T>>::to(v);
        std::memcpy(dst, &u, sizeof(U));
    }
    else
        std::memcpy(dst, &v, sizeof(U));
    if (scrub)
        scrub_range(dst, descriptor_v<U>, 0, sizeof(U));
}

template<class T>
inline void put_record_header(std::byte*& p, const serialize_options& o,
                              const std::size_t payload_len) noexcept
{
    if (!o.full_records)
        return;
    fmt::writer w{p};
    w.put(arg_hash<T>());
    w.put(static_cast<std::uint32_t>(payload_len));
    w.put(std::uint16_t{0});
    w.put(std::uint16_t{0});
    p += fmt::full_record_header_size;
}

// One record's payload: object bytes | u64 literal id | scalar bytes | u32 len + string bytes; then pad to 8.
template<class T>
inline void put_value_record(std::byte*& p, const T& v,
                             const serialize_options& o) noexcept(!may_sort_v<T>)
{
    if constexpr (is_literal_v<T>)
        if (!o.full_records)
            return; // FR-L6: the signature carries the id
    const std::size_t payload = record_payload_bytes(v);
    put_record_header<T>(p, o, payload);
    if constexpr (is_literal_v<T>)
    {
        const std::uint64_t id = T::id;
        std::memcpy(p, &id, 8);
    }
    else if constexpr (is_string_arg_v<T>)
    {
        const std::string_view sv = string_bytes(v);
        const std::uint32_t len = static_cast<std::uint32_t>(sv.size());
        std::memcpy(p, &len, 4);
        if (!sv.empty())
            std::memcpy(p + 4, sv.data(), sv.size());
    }
    else if constexpr (is_scalar_arg_v<T>)
        std::memcpy(p, &v, sizeof(T));
    else if constexpr (is_dynamic<T>())
    { // v2: u32 len, u32 0, canonical image
        const std::uint32_t len = static_cast<std::uint32_t>(payload - fmt::image_prefix);
        std::memcpy(p, &len, 4);
        std::memset(p + 4, 0, 4);
        encode_image(v, p + fmt::image_prefix, len, o.canonical_unordered);
    }
    else
        put_object(p, v, o.scrub_padding);
    if (fmt::align8(payload) != payload)
        std::memset(p + payload, 0, fmt::align8(payload) - payload);
    p += fmt::align8(payload);
}

// One argument: a constant (1.5) or a value's record.
template<class T>
inline void put_record(std::byte*& p, const T& v,
                       const serialize_options& o) noexcept(!may_sort_v<T>)
{
    if constexpr (is_constant_v<T>)
    { // nothing in a compact frame; in a full-records frame the record of the value it stands for
        if (o.full_records)
        {
            const arg_value_t<T> value = T::value;
            put_record(p, value, o);
        }
    }
    else
        put_value_record(p, v, o);
}

inline void put_frame_header(std::byte* const p, const std::uint32_t len, const std::uint16_t flags,
                             const std::uint16_t n, const std::uint64_t call_hash) noexcept
{
    fmt::writer w{p};
    w.put(len);
    w.put(flags);
    w.put(n);
    w.put(call_hash);
}

// Copy `total` object bytes to a plain sink through the caller's chunk buffer, scrubbing per chunk.
template<sink S>
inline bool stream_bytes(S& out, const std::byte* const src, const std::size_t total,
                         const type_desc& d, const serialize_options& o, std::byte* const chunk,
                         const bool pad_to_8) noexcept(sink_noexcept<S>)
{
    for (std::size_t off = 0; off < total; off += stream_chunk_size)
    {
        const std::size_t len = total - off < stream_chunk_size ? total - off : stream_chunk_size;
        std::memcpy(chunk, src + off, len);
        if (o.scrub_padding)
            scrub_range(chunk, d, off, len);
        if (!out.write(chunk, len))
            return false;
    }
    if (pad_to_8 && fmt::align8(total) != total)
    {
        const std::byte zeros[8]{};
        return out.write(zeros, fmt::align8(total) - total);
    }
    return true;
}

// Stream one record through a bounded chunk buffer (plain sinks, large frames). Only as_flat types need a
// converted copy on the stack; plain objects stream straight from the caller's storage (cc review follow-up).
template<sink S, class T>
inline bool stream_record(S& out, const T& v, const serialize_options& o,
                          const bool pad_to_8 = true) noexcept(serialize_noexcept<S, T>)
{
    alignas(16) std::byte chunk[stream_chunk_size];
    std::byte* p = chunk;
    if constexpr (is_literal_v<T> || is_constant_v<T>)
    {
        put_record(p, v,
                   o); // full: header + id (or the constant's value); compact: nothing (FR-L6)
        return p == chunk || out.write(chunk, static_cast<std::size_t>(p - chunk));
    }
    else if constexpr (is_scalar_arg_v<T>)
    {
        put_record(p, v, o); // header (full) + the scalar bytes, padded
        return out.write(chunk, static_cast<std::size_t>(p - chunk));
    }
    else if constexpr (is_dynamic<T>())
    { // v2: the image needs random access while it is built (tail offsets are patched into the fixed part),
        // so it is assembled whole: in the chunk when it fits, otherwise in a heap buffer — the one place a
        // plain sink can allocate; claiming sinks (capture_file, rings) never do.
        const std::size_t need = record_bytes(v, o.full_records);
        if (need <= stream_chunk_size)
        {
            put_record(p, v, o);
            return out.write(chunk, static_cast<std::size_t>(p - chunk));
        }
        std::vector<std::byte> big(need);
        std::byte* q = big.data();
        put_record(q, v, o);
        return out.write(big.data(), big.size());
    }
    else if constexpr (is_string_arg_v<T>)
    { // header (full), u32 len in the chunk, then the bytes straight from the caller's storage, then the pad
        put_record_header<T>(p, o, record_payload_bytes(v));
        const std::string_view sv = string_bytes(v);
        const std::uint32_t len = static_cast<std::uint32_t>(sv.size());
        std::memcpy(p, &len, 4);
        p += 4;
        if (!out.write(chunk, static_cast<std::size_t>(p - chunk)))
            return false;
        if (!sv.empty() && !out.write(reinterpret_cast<const std::byte*>(sv.data()), sv.size()))
            return false;
        const std::size_t payload = fmt::string_len_prefix + sv.size();
        if (pad_to_8 && fmt::align8(payload) != payload)
        {
            const std::byte zeros[8]{};
            return out.write(zeros, fmt::align8(payload) - payload);
        }
        return true;
    }
    else
    {
        put_record_header<T>(p, o, record_payload_bytes(v));
        if (p != chunk && !out.write(chunk, static_cast<std::size_t>(p - chunk)))
            return false;
        using U = mapped_t<T>;
        if constexpr (has_as_flat<std::remove_cv_t<T>>)
        {
            const U u = as_flat<std::remove_cv_t<T>>::to(v);
            return stream_bytes(out, reinterpret_cast<const std::byte*>(&u), sizeof(U),
                                descriptor_v<U>, o, chunk, pad_to_8);
        }
        else
        {
            return stream_bytes(out, reinterpret_cast<const std::byte*>(&v), sizeof(U),
                                descriptor_v<U>, o, chunk, pad_to_8);
        }
    }
}

} // namespace detail

// Frame size for a call signature (FR-S3): exact and constexpr when no argument is a runtime string, the
// minimum otherwise. frame_size(o, vs...) is exact always (one length pass, FR-A3).
template<class... Ts> constexpr std::size_t frame_size(const serialize_options o = {}) noexcept
{
    return fmt::frame_header_size + (o.crc ? fmt::crc_trailer_size : 0) +
           (std::size_t{0} + ... + detail::record_size<std::remove_cvref_t<Ts>>(o.full_records));
}
template<class... Ts>
inline std::size_t frame_size(const serialize_options o, const Ts&... vs) noexcept
{
    return fmt::frame_header_size + (o.crc ? fmt::crc_trailer_size : 0) +
           (std::size_t{0} + ... + detail::record_bytes(vs, o.full_records));
}
// True when any argument is a runtime string (frame size known only at run time).
template<class... Ts>
constexpr bool has_string_arg_v = (detail::is_string_arg_v<std::remove_cvref_t<Ts>> || ...);
// True when the frame size of these arguments is only known at run time (strings, dynamic types — v2).
template<class... Ts>
constexpr bool has_runtime_size_v = ((detail::is_string_arg_v<std::remove_cvref_t<Ts>> ||
                                      detail::is_dynamic<std::remove_cvref_t<Ts>>()) ||
                                     ...);

namespace detail
{
// 1.7: a sink that wants every frame checksummed (capture_file{.crc = true}) says so through frame_crc(); the
// frame then carries the flag and the trailer whatever the caller's serialize_options.
template<class S> constexpr bool sink_wants_crc(const S& out) noexcept
{
    if constexpr (requires {
                      { out.frame_crc() } -> std::convertible_to<bool>;
                  })
        return out.frame_crc();
    else
        return false;
}
// 1.7: the trailer — the CRC32C of everything before it, then four zero bytes.
inline void put_crc_trailer(std::byte* const frame, const std::size_t total) noexcept
{
    const std::uint32_t c =
        crc32c(std::span<const std::byte>(frame, total - fmt::crc_trailer_size));
    std::memcpy(frame + total - fmt::crc_trailer_size, &c, 4);
    std::memset(frame + total - 4, 0, 4);
}
// One frame. `Sig...` is the call signature (an optional form tag, then the argument categories); `vs...`
// are the records. serialize_with passes Sig = Ts; print passes Sig = print_tag, lit<F>, Ts.
template<class... Sig, sink S, class... Ts>
result<std::size_t> serialize_frame(serialize_options o, S& out,
                                    const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    (detail::touch_arg<Sig>(), ...); // FR-R1: registration by instantiation
    static_cast<void>(&detail::registrar_call<Sig...>::g);
    if constexpr (scoped_sink<S>) // 2.2.1: the (scope, signature) node of a scoped sink
        static_cast<void>(&detail::registrar_scoped<typename S::dictionary_scope, Sig...>::g);
    static_assert(sizeof...(Ts) <= 65535,
                  "es::serreg: at most 65535 records per frame (u16 n_records)");
    static_assert(frame_size<Ts...>(serialize_options{.full_records = true, .crc = true}) <=
                      0xFFFFFFFFull,
                  "es::serreg: frame exceeds the u32 frame_len (FR-W2)");
    if constexpr (has_runtime_size_v<Ts...>)
        if (frame_size(serialize_options{.full_records = true, .crc = true}, vs...) > 0xFFFFFFFFull)
            return std::unexpected(error::invalid_argument); // a > 4 GiB string or image
    constexpr bool dynamic = has_runtime_size_v<Ts...>;
    if (detail::sink_wants_crc(out))
        o.crc = true;
    const std::size_t total = dynamic ? frame_size(o, vs...) : frame_size<Ts...>(o);
    const std::uint16_t flags = static_cast<std::uint16_t>(
        (o.full_records ? fmt::frame_flag_full : 0) | (o.crc ? fmt::frame_flag_crc : 0));

    // 2.1.12 (review C-08): always_inline, in the position before the parameter list where it appertains to the
    // call operator — at -O2 (RelWithDebInfo) GCC otherwise keeps fill out of line and calls it through a closure.
    auto fill = [&] [[gnu::always_inline]] (std::byte *
                                            p) noexcept(!detail::any_may_sort_v<Ts...>) {
        std::byte* const frame = p;
        detail::put_frame_header(p, static_cast<std::uint32_t>(total), flags,
                                 static_cast<std::uint16_t>(sizeof...(Ts)), call_hash_v<Sig...>);
        p += fmt::frame_header_size;
        (detail::put_record(p, vs, o), ...);
        if (o.crc)
            detail::put_crc_trailer(frame, total);
    };
    // the stack buffers are sized for the largest form of this signature: full records and the CRC trailer
    constexpr serialize_options largest{.full_records = true, .crc = true};

    if constexpr (claiming_sink<S>)
    {
        std::byte* const p = out.claim(total);
        if (!p)
            return std::unexpected(detail::sink_failure(out));
        fill(p);
        if (!detail::commit_claimed(out, total))
            return std::unexpected(detail::sink_failure(out));
    }
    else if constexpr (!dynamic && frame_size<Ts...>(largest) <= stack_frame_limit)
    {
        alignas(16) std::byte buf[frame_size<Ts...>(largest)]; // exactly sized (cc S2)
        fill(buf);
        if (!out.write(buf, total))
            return std::unexpected(detail::sink_failure(out));
    }
    else if (dynamic && total <= 4096)
    { // runtime strings: a small fixed stack buffer for the common short-message case
        alignas(16) std::byte buf[4096];
        fill(buf);
        if (!out.write(buf, total))
            return std::unexpected(detail::sink_failure(out));
    }
    else if (o.crc)
    { // 1.7: a large frame with a trailer cannot be streamed (the CRC covers the whole frame): assembled on the
        // heap — the one allocation a plain sink's serialize may make, like the > 16 KiB image case
        std::vector<std::byte> buf(total);
        fill(buf.data());
        if (!out.write(buf.data(), total))
            return std::unexpected(detail::sink_failure(out));
    }
    else
    {
        std::byte hdr[fmt::frame_header_size];
        detail::put_frame_header(hdr, static_cast<std::uint32_t>(total), flags,
                                 static_cast<std::uint16_t>(sizeof...(Ts)), call_hash_v<Sig...>);
        if (!out.write(hdr, sizeof hdr))
            return std::unexpected(detail::sink_failure(out));
        bool ok = true;
        ((ok = ok && detail::stream_record(out, vs, o)), ...); // stops at the first failure
        if (!ok)
            return std::unexpected(detail::sink_failure(out));
    }
    return total;
}
} // namespace detail

// Serialize `vs...` as one frame into `out`. Returns bytes written.
template<sink S, detail::serializable_arg... Ts>
result<std::size_t> serialize_with(const serialize_options o, S& out,
                                   const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return detail::serialize_frame<std::remove_cvref_t<Ts>...>(o, out, vs...);
}

template<sink S, detail::serializable_arg... Ts>
result<std::size_t> serialize(S& out,
                              const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return serialize_with(serialize_options{}, out, vs...);
}

// 2.1.16 (B10): an argument that is no category gets a message instead of an overload-resolution failure. Since
// 2.2.0 (format 1.6) containers are arguments too; what remains outside every category: pointers, references,
// unions, bit-fields and virtual classes.
template<sink S, class... Ts>
    requires(!(detail::serializable_arg<Ts> && ...))
result<std::size_t> serialize(S& out, const Ts&... vs) =
    delete ("es::serreg::serialize: an argument is not a serialisable category (a class or enum, a "
            "literal, a constant, a scalar, a runtime string, or a container). "
            "Pointers, references, unions, bit-fields and virtual classes are never serialised; "
            "map them with as_flat<T>.");
template<sink S, class... Ts>
    requires(!(detail::serializable_arg<Ts> && ...))
result<std::size_t> serialize_with(const serialize_options o, S& out, const Ts&... vs) =
    delete ("es::serreg::serialize_with: an argument is not a serialisable category (a class or "
            "enum, a literal, a constant, a scalar, a runtime string, or a "
            "container). Pointers, references, unions, bit-fields and virtual classes are never "
            "serialised; map them with as_flat<T>.");

// The common "message literal first" call without a UDL: serialize<"order rejected">(out, order) is exactly
// serialize(out, lit<"order rejected">{}, order) — same call hash, same frame (FR-L1).
template<fixed_string L, sink S, detail::serializable_arg... Ts>
result<std::size_t> serialize(S& out,
                              const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return serialize_with(serialize_options{}, out, lit<L>{}, vs...);
}
template<fixed_string L, sink S, detail::serializable_arg... Ts>
result<std::size_t> serialize_with(const serialize_options o, S& out,
                                   const Ts&... vs) noexcept(detail::serialize_noexcept<S, Ts...>)
{
    return serialize_with(o, out, lit<L>{}, vs...);
}

// Payload only — no frame header, no id (FR-S5). The caller owns framing (e.g. an existing SBE envelope).
template<sink S, flat_type T>
result<std::size_t> serialize_raw(S& out, const T& v, const bool scrub_padding = true) noexcept(
    detail::sink_noexcept<S>)
{
    detail::touch<T>();
    using U = detail::mapped_t<T>;
    if constexpr (claiming_sink<S>)
    {
        std::byte* const p = out.claim(sizeof(U));
        if (!p)
            return std::unexpected(detail::sink_failure(out));
        detail::put_object(p, v, scrub_padding);
        if (!detail::commit_claimed(out, sizeof(U)))
            return std::unexpected(detail::sink_failure(out));
    }
    else if constexpr (sizeof(U) <= stack_frame_limit)
    {
        alignas(U) std::byte tmp[sizeof(U)];
        detail::put_object(tmp, v, scrub_padding);
        if (!out.write(tmp, sizeof(U)))
            return std::unexpected(detail::sink_failure(out));
    }
    else
    {
        const serialize_options o{.scrub_padding = scrub_padding, .full_records = false};
        if (!detail::stream_record(out, v, o, false))
            return std::unexpected(detail::sink_failure(out));
    }
    return sizeof(U);
}

} // namespace es::serreg
