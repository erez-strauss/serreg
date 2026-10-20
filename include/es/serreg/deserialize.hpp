// es/serreg/deserialize.hpp — the five reader styles (spec §6.4, FR-S4/S5):
//   1. deserialize(bytes, T&...)             typed, whole objects. Flat records are validated before any output is
//                                            touched; a dynamic record is checked as it is decoded, so a failed call
//                                            with several arguments leaves the earlier ones complete and the failing
//                                            one partially written — a basic guarantee (valid, unspecified value);
//                                            the error says how far it got (decode_error, deviations "FR-S4")
//   2. inspect(bytes[, dictionary])          frame_view: header + a lazy record range ("what is in this buffer"),
//                                            no copies, no record-count cap; a side dictionary cuts foreign frames
//   3. dispatch<Ts...>(bytes, f[, dict])     compile-time match of each record against Ts; zero-copy when aligned
//   4. dynamic visitor                       tools::visit_record (tools.hpp) over a dyn::dictionary — used by srdump
//   5. view<T>(record_view)                  zero-copy typed pointer when alignment allows, error::misaligned otherwise
#pragma once
#include <es/serreg/dictionary.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/serialize.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace es::serreg
{

struct frame_header
{
    std::uint32_t frame_len;
    std::uint16_t flags;
    std::uint16_t n_records;
    std::uint64_t call_hash;
    bool full() const noexcept
    {
        return flags & fmt::frame_flag_full;
    }
    bool dictionary() const noexcept
    {
        return (flags & fmt::frame_flag_dict) || call_hash == fmt::dict_frame_sentinel;
    }
    // 1.7: checkpoint and writer-info frames carry no records either
    bool meta() const noexcept
    {
        return dictionary() || (flags & (fmt::frame_flag_checkpoint | fmt::frame_flag_info)) != 0;
    }
    bool crc() const noexcept
    {
        return (flags & fmt::frame_flag_crc) != 0;
    }
    // 1.7: where the records end — before the CRC32C trailer when the frame carries one
    std::size_t payload_end() const noexcept
    {
        return frame_len - (crc() ? fmt::crc_trailer_size : 0);
    }
};

inline result<frame_header> read_frame_header(const std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < fmt::frame_header_size)
        return std::unexpected(error::truncated);
    frame_header h{};
    fmt::cursor c{bytes};
    c.read(h.frame_len);
    c.read(h.flags);
    c.read(h.n_records);
    c.read(h.call_hash);
    if (h.frame_len < fmt::frame_header_size || h.frame_len % 8)
        return std::unexpected(error::length_mismatch);
    if (h.frame_len > bytes.size())
        return std::unexpected(error::truncated);
    if (h.flags & ~(fmt::frame_flag_full | fmt::frame_flag_dict | fmt::frame_flag_crc |
                    fmt::frame_flag_checkpoint | fmt::frame_flag_info))
        return std::unexpected(error::bad_version); // 1.1: unknown frame flags are not guessed at
    if (h.crc())
    { // 1.7: the trailer is checked before anything in the frame is believed
        if (h.frame_len < fmt::frame_header_size + fmt::crc_trailer_size)
            return std::unexpected(error::length_mismatch);
        std::uint32_t stored{};
        std::memcpy(&stored, bytes.data() + h.frame_len - fmt::crc_trailer_size, 4);
        if (crc32c(bytes.first(h.frame_len - fmt::crc_trailer_size)) != stored)
            return std::unexpected(error::crc_mismatch);
    }
    return h;
}

struct literal_view
{
    std::uint64_t id;
    std::string_view text;
}; // text empty when unknown

struct record_view
{
    std::uint64_t hash; // schema hash | literal id | scalar name hash | "string" hash
    bool is_literal;
    const type_desc* desc; // this program's descriptor, or nullptr (literal / foreign type)
    const dyn::type*
        dyn_desc; // from the side dictionary when given and the type is not in the registry
    std::span<const std::byte>
        payload;      // object bytes | u64 id | scalar bytes | string bytes (no prefix)
    fmt::arg_kind ak; // 1.1: the record's category
    kind k;           // 1.1: scalar kind when ak == arg_scalar
    // 1.5: the record is a compile-time constant of the call (42_). It is presented as the scalar or enum record
    // it stands for (ak is arg_scalar or arg_type); in a compact frame its payload is the value stored in the
    // call's dictionary entry, not bytes of the frame.
    bool is_const = false;
    std::string_view text() const noexcept // the bytes of a string record as text
    {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

namespace detail
{

// Resolves call signatures and types through the registry first, then an optional side dictionary.
struct resolver
{
    const dyn::dictionary* dict;
    const call_desc* cd = nullptr;    // compact frames: argument list from the registry
    const dyn::call* dcall = nullptr; //                 or from the dictionary
    std::size_t n_args() const noexcept
    {
        return cd ? cd->args.size() : dcall ? dcall->args.size() : 0;
    }
    bool arg_is_literal(const std::size_t i) const noexcept
    {
        return cd ? cd->args[i].is_literal : dcall->args[i].is_literal;
    }
    fmt::arg_kind arg_kind(const std::size_t i) const noexcept
    {
        return cd ? cd->args[i].ak : dcall->args[i].ak;
    }
    kind arg_scalar_kind(const std::size_t i) const noexcept
    {
        return cd ? cd->args[i].k : dcall->args[i].k;
    }
    std::uint64_t arg_hash(const std::size_t i) const noexcept
    {
        return cd ? cd->args[i].hash : dcall->args[i].hash;
    }
    // 1.5: the 8 value bytes of a constant argument (they live as long as the registry / the dictionary)
    const std::byte* arg_value(const std::size_t i) const noexcept
    {
        return reinterpret_cast<const std::byte*>(cd ? &cd->args[i].value : &dcall->args[i].value);
    }
    // 1.5: size of the value a constant argument stands for; 0 when its enum is unknown or the entry claims a
    // type that is not a scalar or an enum of at most 8 bytes (only a corrupt dictionary does)
    std::size_t const_size(const std::size_t i) const noexcept
    {
        if (const std::size_t n = kind_size(arg_scalar_kind(i)); n != 0)
            return n;
        const type_desc* d;
        const dyn::type* dd;
        const std::size_t n = type_size(arg_hash(i), d, dd);
        return n <= fmt::const_value_size && !is_dynamic(d, dd) ? n : 0;
    }
    // Fills desc/dyn_desc and returns the record size, or 0 if the type is unknown everywhere. For a dynamic
    // type (v2) the size is its fixed part; the record itself carries `u32 len` — see compact_size().
    std::size_t type_size(const std::uint64_t h, const type_desc*& d,
                          const dyn::type*& dd) const noexcept
    {
        d = registry::instance().find_type(h);
        dd = nullptr;
        if (d)
            return d->size;
        if (dict)
        {
            dd = dict->find_type(h);
            if (dd)
                return dd->size;
        }
        return 0;
    }
    static bool is_dynamic(const type_desc* const d, const dyn::type* const dd) noexcept
    {
        return (d && !d->is_flat) || (dd && !dd->is_flat);
    }
    bool resolve_call(const std::uint64_t h) noexcept
    {
        cd = registry::instance().find_call(h);
        if (!cd && dict)
            dcall = dict->find_call(h);
        return cd || dcall;
    }
};

} // namespace detail

// Style 2. Validated up front (every record is bounds-checked once); iteration cannot fail afterwards.
class frame_view
{
public:
    frame_header header{};

    class iterator
    {
    public:
        using value_type = record_view;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        iterator() = default;
        iterator(const frame_view* const fv, const std::size_t i) noexcept : fv_(fv), i_(i)
        {
            if (fv_)
                p_ = fv_->data_ + fmt::frame_header_size;
        }
        record_view operator*() const noexcept
        {
            return fv_->decode(i_, p_);
        }
        iterator& operator++() noexcept
        {
            p_ += fv_->advance(i_, p_);
            ++i_;
            return *this;
        }
        iterator operator++(int) noexcept
        {
            iterator t = *this;
            ++*this;
            return t;
        }
        bool operator==(const iterator& o) const noexcept
        {
            return i_ == o.i_;
        }

    private:
        const frame_view* fv_ = nullptr;
        std::size_t i_ = 0;
        const std::byte* p_ = nullptr;
    };

    std::size_t size() const noexcept
    {
        return count_;
    }
    iterator begin() const noexcept
    {
        return iterator{this, 0};
    }
    iterator end() const noexcept
    {
        return iterator{this, count_};
    }
    auto records() const noexcept
    {
        return std::ranges::subrange(begin(), end());
    }

private:
    friend result<frame_view> inspect(std::span<const std::byte>, const dyn::dictionary*) noexcept;
    // Bytes consumed by record i starting at p (header + aligned payload).
    std::size_t advance(const std::size_t i, const std::byte* const p) const noexcept
    {
        if (header.full())
        {
            std::uint32_t len;
            std::memcpy(&len, p + 8, 4);
            return fmt::full_record_header_size + fmt::align8(len);
        }
        return fmt::align8(sizes_ ? compact_size(i, p) : 0);
    }
    // Compact payload size of record i at p (a string reads its own u32 length prefix; validated by inspect()).
    std::size_t compact_size(const std::size_t i, const std::byte* const p) const noexcept
    {
        switch (res_.arg_kind(i))
        {
        case fmt::arg_literal: // FR-L6: no bytes in a compact frame
        case fmt::arg_const:   // 1.5: nor for a constant
            return 0;
        case fmt::arg_scalar:
            return kind_size(res_.arg_scalar_kind(i));
        case fmt::arg_string: {
            std::uint32_t len;
            std::memcpy(&len, p, 4);
            return fmt::string_len_prefix + len;
        }
        default: {
            const type_desc* d;
            const dyn::type* dd;
            const std::size_t sz = res_.type_size(res_.arg_hash(i), d, dd);
            if (detail::resolver::is_dynamic(d, dd))
            { // v2: u32 len, u32 0, image
                std::uint32_t len;
                std::memcpy(&len, p, 4);
                return fmt::image_prefix + len;
            }
            return sz;
        }
        }
    }
    record_view decode(const std::size_t i, const std::byte* p) const noexcept
    {
        record_view r{};
        std::size_t len;
        if (header.full())
        {
            std::uint32_t l32;
            std::memcpy(&r.hash, p, 8);
            std::memcpy(&l32, p + 8, 4);
            p += fmt::full_record_header_size;
            len = l32;
            if (res_.n_args() > i)
                r.ak = res_.arg_kind(i);
            else if (r.hash == detail::string_arg_hash)
                r.ak = fmt::arg_string; // a foreign print/string frame without its call entry
            else if (!registry::instance().find_literal(r.hash).empty())
                r.ak = fmt::arg_literal;
            else
                r.ak = fmt::arg_type;
            if (r.ak == fmt::arg_const)
            { // written as the record it stands for
                r.is_const = true;
                r.ak = kind_size(res_.arg_scalar_kind(i)) != 0 ? fmt::arg_scalar : fmt::arg_type;
            }
            if (r.ak == fmt::arg_scalar)
                r.k = res_.arg_scalar_kind(i);
        }
        else
        {
            r.hash = res_.arg_hash(i);
            r.ak = res_.arg_kind(i);
            if (r.ak == fmt::arg_const)
            { // no bytes in the frame: the value is in the call's entry
                r.is_const = true;
                r.ak = kind_size(res_.arg_scalar_kind(i)) != 0 ? fmt::arg_scalar : fmt::arg_type;
                p = res_.arg_value(i);
                len = res_.const_size(i);
            }
            else
                len = compact_size(i, p);
            if (r.ak == fmt::arg_scalar)
                r.k = res_.arg_scalar_kind(i);
        }
        r.is_literal = r.ak == fmt::arg_literal;
        if (r.ak == fmt::arg_string)
        { // payload = the bytes after the length prefix
            r.payload = {p + fmt::string_len_prefix,
                         len >= fmt::string_len_prefix ? len - fmt::string_len_prefix : 0};
        }
        else
        {
            r.payload = {p, len};
            if (r.ak == fmt::arg_type)
            {
                res_.type_size(r.hash, r.desc, r.dyn_desc);
                if (detail::resolver::is_dynamic(r.desc, r.dyn_desc))
                    r.payload = {p + fmt::image_prefix, len >= fmt::image_prefix
                                                            ? len - fmt::image_prefix
                                                            : 0}; // the image
            }
        }
        return r;
    }
    const std::byte* data_ = nullptr;
    std::size_t count_ = 0;
    bool sizes_ = true;
    detail::resolver res_{};
};

inline result<frame_view> inspect(const std::span<const std::byte> bytes,
                                  const dyn::dictionary* const dict = nullptr) noexcept
{
    const auto h = read_frame_header(bytes);
    if (!h)
        return std::unexpected(h.error());
    frame_view fv;
    fv.header = *h;
    fv.data_ = bytes.data();
    fv.res_.dict = dict;
    if (h->meta())
        return fv;
    const std::byte* p = bytes.data() + fmt::frame_header_size;
    const std::byte* const end = bytes.data() + h->payload_end();
    if (h->full())
    {
        static_cast<void>(
            fv.res_.resolve_call(h->call_hash)); // optional: improves literal classification
        for (std::uint16_t i = 0; i < h->n_records; ++i)
        {
            if (static_cast<std::size_t>(end - p) < fmt::full_record_header_size)
                return std::unexpected(error::truncated);
            std::uint64_t hash;
            std::uint32_t len;
            std::memcpy(&hash, p, 8);
            std::memcpy(&len, p + 8, 4);
            p += fmt::full_record_header_size;
            if (static_cast<std::size_t>(end - p) < fmt::align8(len))
                return std::unexpected(error::truncated);
            if (hash == detail::string_arg_hash)
            { // string payload = u32 len + bytes: the prefix must agree with payload_len
                std::uint32_t inner;
                if (len < fmt::string_len_prefix)
                    return std::unexpected(error::length_mismatch);
                std::memcpy(&inner, p, 4);
                if (inner != len - fmt::string_len_prefix)
                    return std::unexpected(error::length_mismatch);
            }
            else
            {
                const type_desc* d;
                const dyn::type* dd;
                fv.res_.type_size(hash, d, dd);
                if (detail::resolver::is_dynamic(d, dd))
                { // v2: u32 len, u32 0, image — the prefix must agree with payload_len
                    std::uint32_t inner;
                    if (len < fmt::image_prefix)
                        return std::unexpected(error::length_mismatch);
                    std::memcpy(&inner, p, 4);
                    if (inner != len - fmt::image_prefix)
                        return std::unexpected(error::length_mismatch);
                }
            }
            p += fmt::align8(len);
        }
    }
    else
    {
        if (!fv.res_.resolve_call(h->call_hash))
            return std::unexpected(error::unknown_call);
        if (fv.res_.n_args() != h->n_records)
            return std::unexpected(error::length_mismatch);
        for (std::size_t i = 0; i < h->n_records; ++i)
        {
            if (fv.res_.arg_kind(i) == fmt::arg_string &&
                static_cast<std::size_t>(end - p) < fmt::string_len_prefix)
                return std::unexpected(error::truncated);
            if (fv.res_.arg_kind(i) == fmt::arg_type)
            { // v2: a dynamic record needs its 8-byte prefix before its length can be read
                const type_desc* d;
                const dyn::type* dd;
                fv.res_.type_size(fv.res_.arg_hash(i), d, dd);
                if (detail::resolver::is_dynamic(d, dd) &&
                    static_cast<std::size_t>(end - p) < fmt::image_prefix)
                    return std::unexpected(error::truncated);
            }
            const std::size_t len = fv.compact_size(i, p);
            if (len == 0 && fv.res_.arg_kind(i) == fmt::arg_type)
                return std::unexpected(error::unknown_type);
            if (fv.res_.arg_kind(i) == fmt::arg_const && fv.res_.const_size(i) == 0)
                return std::unexpected(error::unknown_type); // 1.5: its enum is not known
            if (static_cast<std::size_t>(end - p) < fmt::align8(len))
                return std::unexpected(error::truncated);
            p += fmt::align8(len);
        }
    }
    fv.count_ = h->n_records;
    return fv;
}

// Style 5.
template<flat_type T> result<const detail::mapped_t<T>*> view(const record_view& r) noexcept
{
    using U = detail::mapped_t<T>;
    if (r.is_literal || r.hash != schema_hash_v<T>)
        return std::unexpected(error::hash_mismatch);
    if (r.payload.size() != sizeof(U))
        return std::unexpected(error::length_mismatch);
    if (reinterpret_cast<std::uintptr_t>(r.payload.data()) % alignof(U))
        return std::unexpected(error::misaligned);
    // 2.1.3 (G2-12): a view cannot repair a bool byte other than 0/1 in place; copy with deserialize instead
    if (!detail::bools_canonical<U>(r.payload.data()))
        return std::unexpected(error::invalid_value);
#if defined(__cpp_lib_start_lifetime_as)
    return std::start_lifetime_as<U>(r.payload.data());
#else
    return std::launder(reinterpret_cast<const U*>(
        r.payload.data())); // trivially copyable bytes written by serialize()
#endif
}

namespace detail
{

// A flat value from its bytes. 2.1.3 (G2-12): a bool byte other than 0/1 is read as true and reported as
// warning::noncanonical_bool (copy_flat, reflect.hpp). 2.1.16 (decision D): returns false when an enum leaf
// without a fixed underlying type held a value outside its range — that leaf now reads as 0 and the caller
// reports error::invalid_value.
template<class T> inline bool get_object(T& out, const std::byte* const src) noexcept
{
    using U = mapped_t<T>;
    flat_copy fc;
    if constexpr (has_as_flat<std::remove_cv_t<T>>)
    {
        U u;
        fc = copy_flat<U>(&u, src);
        out = as_flat<std::remove_cv_t<T>>::from(u);
    }
    else
        fc = copy_flat<U>(&out, src);
    if (fc.bools) [[unlikely]]
    {
        if constexpr (std::is_class_v<U>)
            report_warning(warning::noncanonical_bool, schema_hash_memo<U>, fc.bools);
        else
            report_warning(warning::noncanonical_bool, 0, fc.bools); // a top-level bool argument
    }
    return fc.invalid == 0;
}

// G2-7 (2.1.1): decoding into a dynamic type or a std::string allocates, so those outputs make the typed readers
// potentially-throwing (bad_alloc); every other output — flat, scalar, string_view, literal — is a copy into
// storage the caller owns, and those readers stay noexcept.
template<class T>
constexpr bool is_owning_string_v = false; // std::basic_string<char, traits, any allocator>
template<class A>
constexpr bool is_owning_string_v<std::basic_string<char, std::char_traits<char>, A>> = true;
template<class... Outs>
constexpr bool decode_noexcept =
    !((dynamic_type<std::remove_cvref_t<Outs>> || is_owning_string_v<std::remove_cvref_t<Outs>>) ||
      ...);

// Output types for a string record: std::string (copy; 2.1.5: any allocator, e.g. std::pmr::string, whose
// allocator is kept) or std::string_view (view into the frame).
template<class T>
concept string_out = is_owning_string_v<T> || std::is_same_v<T, std::string_view>;

// Validate-only pass over the records of a frame for the exact signature Sig... whose record types are
// Outs... (Sig = Outs, or a form tag followed by Outs). Used by deserialize / deserialize_print.
// G2-1 (2.1.1): one cursor walks every record — variable-length ones (runtime strings, dynamic images) by their
// own length fields — and every step is checked against frame_len *before* anything is read, so no later record
// can be read past the frame and no output is written for a frame that does not fit. A record that runs past
// frame_len is `truncated`; a length field inconsistent with the type is `length_mismatch`.
template<class... Sig, class... Outs>
inline result<void> validate_frame(const frame_header& h, const std::span<const std::byte> bytes,
                                   std::type_identity<std::tuple<Outs...>>,
                                   std::uint16_t& failed) noexcept
{
    failed = 0; // 2.1.15: 0 = the frame itself, i = record i (1-based) — see decode_error
    if (h.meta())
        return std::unexpected(error::hash_mismatch);
    if (h.n_records != sizeof...(Outs))
        return std::unexpected(error::length_mismatch);
    if (!h.full() && h.call_hash != call_hash_v<Sig...>)
        return std::unexpected(error::hash_mismatch);
    const std::byte* p = bytes.data() + fmt::frame_header_size;
    const std::byte* const end = bytes.data() + h.payload_end();
    const auto left = [&]() noexcept { return static_cast<std::size_t>(end - p); };
    status e;
    std::uint16_t item = 0; // the record being checked
    if (!h.full())
    {
        auto step = [&]<class T>(std::type_identity<T>) noexcept {
            if (!e)
                return;
            failed = ++item;
            std::size_t n = 0; // bytes of this record, padding included
            if constexpr (is_literal_v<T> || is_constant_v<T>)
                n = 0; // FR-L6; 1.5: constants
            else if constexpr (is_string_arg_v<T> || is_dynamic<T>())
            {
                constexpr std::size_t prefix =
                    is_string_arg_v<T> ? fmt::string_len_prefix : fmt::image_prefix;
                if (left() < prefix)
                {
                    e = std::unexpected(error::truncated);
                    return;
                }
                std::uint32_t len;
                std::memcpy(&len, p, 4);
                n = fmt::align8(prefix + std::size_t{len});
            }
            else
                n = fmt::align8(record_payload_size<T>());
            if (left() < n)
            {
                e = std::unexpected(error::truncated);
                return;
            }
            p += n;
        };
        (step(std::type_identity<std::remove_cvref_t<Outs>>{}), ...);
        return e;
    }
    auto check = [&]<class T>(std::type_identity<T>) noexcept {
        if (!e)
            return;
        failed = ++item;
        if (left() < fmt::full_record_header_size)
        {
            e = std::unexpected(error::truncated);
            return;
        }
        std::uint64_t hash;
        std::uint32_t len;
        std::memcpy(&hash, p, 8);
        std::memcpy(&len, p + 8, 4);
        if (hash != arg_hash<T>())
        {
            e = std::unexpected(error::hash_mismatch);
            return;
        }
        if constexpr (is_constant_v<T>)
        { // 1.5: the record of the value it stands for, and the value must be the one the reader names —
            // checked here, so a different constant is refused before any output is written
            if (len != record_payload_size<T>())
            {
                e = std::unexpected(error::length_mismatch);
                return;
            }
            if (left() < fmt::full_record_header_size + fmt::align8(len))
            {
                e = std::unexpected(error::truncated);
                return;
            }
            const arg_value_t<T> want = T::value;
            if (std::memcmp(p + fmt::full_record_header_size, &want, sizeof(want)) != 0)
            {
                e = std::unexpected(error::hash_mismatch);
                return;
            }
        }
        else if constexpr (is_string_arg_v<T> || is_dynamic<T>())
        {
            constexpr std::size_t prefix =
                is_string_arg_v<T> ? fmt::string_len_prefix : fmt::image_prefix;
            if (len < prefix)
            {
                e = std::unexpected(error::length_mismatch);
                return;
            }
            std::uint32_t inner; // the record's own length field must agree with payload_len
            if (left() < fmt::full_record_header_size + prefix)
            {
                e = std::unexpected(error::truncated);
                return;
            }
            std::memcpy(&inner, p + fmt::full_record_header_size, 4);
            if (std::size_t{inner} + prefix != len)
            {
                e = std::unexpected(error::length_mismatch);
                return;
            }
        }
        else if (len != record_payload_size<T>())
        {
            e = std::unexpected(error::length_mismatch);
            return;
        }
        if (left() < fmt::full_record_header_size + fmt::align8(len))
        {
            e = std::unexpected(error::truncated);
            return;
        }
        p += fmt::full_record_header_size + fmt::align8(len);
    };
    (check(std::type_identity<std::remove_cvref_t<Outs>>{}), ...);
    return e;
}

// Reads one record at p into out (bounds against `end` for strings), advancing p.
template<class T>
inline result<void> get_record(const std::byte*& p, const std::byte* const end, T& out,
                               const bool full) noexcept(decode_noexcept<T>)
{
    using U =
        std::remove_cv_t<T>; // a literal may arrive as a const lvalue (a named constexpr lit<…>)
    if (full)
        p += fmt::full_record_header_size;
    if constexpr (is_literal_v<U>)
    {
        if (full)
        { // compact frames carry no bytes for a literal (FR-L6): the call hash already matched
            std::uint64_t id;
            std::memcpy(&id, p, 8);
            if (id != U::id)
                return std::unexpected(error::hash_mismatch);
            p += 8;
        }
    }
    else if constexpr (is_constant_v<U>)
    {
        if (full)
            // 1.5: a compact frame carries no bytes for a constant (the call hash already matched); a full-records
            // frame carries the value, which validate_frame compared with the one the reader names
            p += fmt::align8(sizeof(arg_value_t<U>));
    }
    else if constexpr (is_string_arg_v<U>)
    {
        static_assert(string_out<U>,
                      "es::serreg::deserialize: a string record is read into std::string (any "
                      "allocator) or std::string_view");
        if (static_cast<std::size_t>(end - p) < fmt::string_len_prefix)
            return std::unexpected(error::truncated);
        std::uint32_t len;
        std::memcpy(&len, p, 4);
        if (static_cast<std::size_t>(end - p) < fmt::align8(fmt::string_len_prefix + len))
            return std::unexpected(error::truncated);
        const char* const text = reinterpret_cast<const char*>(p + fmt::string_len_prefix);
        if constexpr (std::is_same_v<U, std::string_view>)
            out = U{text, len};
        else
            out.assign(text, len); // keeps the output's allocator (std::pmr::string)
        p += fmt::align8(fmt::string_len_prefix + len);
    }
    else if constexpr (is_scalar_arg_v<U>)
    {
        static_cast<void>(
            get_object(out, p)); // a bool argument is repaired like a bool member (G2-12)
        p += fmt::align8(sizeof(U));
    }
    else if constexpr (is_dynamic<U>())
    { // v2: u32 len, u32 0, image — bounds-checked, then decoded member by member
        if (static_cast<std::size_t>(end - p) < fmt::image_prefix)
            return std::unexpected(error::truncated);
        std::uint32_t len;
        std::memcpy(&len, p, 4);
        if (static_cast<std::size_t>(end - p) < fmt::align8(fmt::image_prefix + len))
            return std::unexpected(error::truncated);
        const auto r = decode_image(out, std::span<const std::byte>(p + fmt::image_prefix, len));
        p += fmt::align8(fmt::image_prefix +
                         len); // invalid_value is not fatal: the next record is read
        if (!r)
            return r;
    }
    else
    {
        const bool valid = get_object(out, p);
        p += fmt::align8(record_payload_size<U>());
        if (!valid)
            return std::unexpected(
                error::invalid_value); // 2.1.16: an enum leaf outside its range (soft)
    }
    return {};
}

// The typed reader for a signature Sig... over outputs outs... (deserialize / deserialize_print).
// 2.1.15 (decision C): one pass; a failure says how many items were expected, how many were decoded and which
// one failed first (decode_error, error.hpp).
template<class... Sig, class... Ts>
decode_result<std::size_t> deserialize_frame(const std::span<const std::byte> bytes,
                                             Ts&&... outs) noexcept(decode_noexcept<Ts...>)
{
    static_assert(
        ((is_literal_v<std::remove_cvref_t<Ts>> || is_constant_v<std::remove_cvref_t<Ts>> ||
          (std::is_lvalue_reference_v<Ts> && !std::is_const_v<std::remove_reference_t<Ts>>)) &&
         ...),
        "es::serreg::deserialize: non-literal outputs must be non-const lvalues");
    (detail::touch_arg<Sig>(), ...);
    static_cast<void>(&detail::registrar_call<Sig...>::g);
    constexpr std::uint16_t expected = 1 + sizeof...(Ts);
    const auto h = read_frame_header(bytes);
    if (!h)
        return std::unexpected(decode_error{h.error(), expected, 0, 0});
    std::uint16_t failed = 0;
    if (const auto v = detail::validate_frame<Sig...>(
            *h, bytes, std::type_identity<std::tuple<std::remove_cvref_t<Ts>...>>{}, failed);
        !v) // a record that does not fit: the frame was accepted (1), nothing has been written
        return std::unexpected(decode_error{
            v.error(), expected, static_cast<std::uint16_t>(failed == 0 ? 0 : 1), failed});
    const std::byte* p = bytes.data() + fmt::frame_header_size;
    const std::byte* const end = bytes.data() + h->payload_end();
    // soft: invalid_value (2.1.3) — that output is left as it was, the later ones are still read
    status e, soft;
    std::uint16_t item = 0, done = 1; // done: the frame, then every argument decoded completely
    failed = 0;
    auto one = [&](auto& out) noexcept(decode_noexcept<Ts...>) {
        if (!e)
            return;
        ++item;
        e = detail::get_record(p, end, out, h->full());
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
        return std::unexpected(decode_error{e.error(), expected, done, failed});
    if (!soft)
        return std::unexpected(decode_error{soft.error(), expected, done, failed});
    return h->frame_len;
}

} // namespace detail

// Style 1. Validates header, call hash (compact) or per-record hashes (full) and lengths before writing outputs.
// Literal arguments are passed as lit<...> values (their id is checked), constants as constant<V> values (42_).
// Returns bytes consumed; on failure a decode_error: the code, how many items were expected (1 for the frame + the
// arguments), how many were decoded, and which one failed first (2.1.15).
template<class... Ts>
    requires(detail::serializable_arg<std::remove_cvref_t<Ts>> && ...)
decode_result<std::size_t> deserialize(const std::span<const std::byte> bytes,
                                       Ts&&... outs) noexcept(detail::decode_noexcept<Ts...>)
{
    return detail::deserialize_frame<std::remove_cvref_t<Ts>...>(bytes, static_cast<Ts&&>(outs)...);
}
// Typed readers for print frames (1.1): the format literal first, then the argument outputs.
template<fixed_string F, class... Ts>
    requires(detail::serializable_arg<std::remove_cvref_t<Ts>> && ...)
decode_result<std::size_t> deserialize_print(const std::span<const std::byte> bytes, lit<F>,
                                             Ts&&... outs) noexcept(detail::decode_noexcept<Ts...>)
{
    return detail::deserialize_frame<print_tag, lit<F>, std::remove_cvref_t<Ts>...>(
        bytes, lit<F>{}, static_cast<Ts&&>(outs)...);
}
template<fixed_string F, class... Ts>
    requires(detail::serializable_arg<std::remove_cvref_t<Ts>> && ...)
decode_result<std::size_t> deserialize_println(
    const std::span<const std::byte> bytes, lit<F>,
    Ts&&... outs) noexcept(detail::decode_noexcept<Ts...>)
{
    return detail::deserialize_frame<println_tag, lit<F>, std::remove_cvref_t<Ts>...>(
        bytes, lit<F>{}, static_cast<Ts&&>(outs)...);
}

// Payload only (FR-S5).
template<flat_type T>
result<std::size_t> deserialize_raw(const std::span<const std::byte> bytes, T& out) noexcept
{
    detail::touch<T>();
    using U = detail::mapped_t<T>;
    if (bytes.size() < sizeof(U))
        return std::unexpected(error::truncated);
    if (!detail::get_object(out, bytes.data()))
        return std::unexpected(error::invalid_value); // 2.1.16: an enum leaf outside its range
    return sizeof(U);
}

namespace detail
{
// 2.1.9: record hash → position in dispatch<Ts...>'s pack in constant time, built at compile time. The keys are
// XXH3-64 values (uniformly distributed), so a search over table sizes m = 2^k ≥ 2·N and shifts s finds, almost
// always at the first m, a pair for which (h >> s) & (m - 1) differs for every distinct key: a perfect hash. A
// lookup is one shift, one mask, one table load and one compare against the stored key. If no pair is found (it
// needs a pathological key set), `find` falls back to a binary search over the sorted keys. A key listed twice
// (the same type twice in Ts) keeps its first position, as the linear match did.
template<std::uint64_t... Keys> struct hash_index
{
    static constexpr std::size_t n = sizeof...(Keys);
    static constexpr std::array<std::uint64_t, n> keys{Keys...};

    struct layout
    {
        std::size_t m = 0;
        unsigned shift = 0;
        bool perfect = false;
    };
    static consteval bool first_occurrence(const std::size_t i)
    {
        for (std::size_t j = 0; j < i; ++j)
            if (keys[j] == keys[i])
                return false;
        return true;
    }
    static consteval layout find_layout()
    {
        const std::size_t m0 = std::bit_ceil(std::max<std::size_t>(2 * n, 2));
        for (std::size_t m = m0; m <= 8 * m0; m *= 2)
            for (unsigned s = 0; s < 64; ++s)
            {
                std::vector<bool> used(m, false);
                bool ok = true;
                for (std::size_t i = 0; i < n && ok; ++i)
                {
                    if (!first_occurrence(i))
                        continue;
                    const std::size_t slot = static_cast<std::size_t>(keys[i] >> s) & (m - 1);
                    ok = !used[slot];
                    used[slot] = true;
                }
                if (ok)
                    return {m, s, true};
            }
        return {};
    }
    static constexpr layout lay = find_layout();
    // slot → position + 1 (0 = empty)
    static constexpr auto slots = [] {
        std::array<std::uint32_t, (lay.perfect ? lay.m : 1)> a{};
        if constexpr (lay.perfect)
            for (std::size_t i = 0; i < n; ++i)
                if (first_occurrence(i))
                    a[static_cast<std::size_t>(keys[i] >> lay.shift) & (lay.m - 1)] =
                        static_cast<std::uint32_t>(i + 1);
        return a;
    }();
    // (key, position) sorted by key, first occurrence first — the fallback
    static constexpr auto sorted = [] {
        std::array<std::pair<std::uint64_t, std::uint32_t>, n> a{};
        for (std::size_t i = 0; i < n; ++i)
            a[i] = {keys[i], static_cast<std::uint32_t>(i)};
        std::ranges::stable_sort(a, {}, &std::pair<std::uint64_t, std::uint32_t>::first);
        return a;
    }();

    // Position of `h` in Keys..., or -1.
    static constexpr int find(const std::uint64_t h) noexcept
    {
        if constexpr (lay.perfect)
        {
            const std::uint32_t i = slots[static_cast<std::size_t>(h >> lay.shift) & (lay.m - 1)];
            return i != 0 && keys[i - 1] == h ? static_cast<int>(i - 1) : -1;
        }
        else
        {
            const auto it = std::ranges::lower_bound(
                sorted, h, {}, &std::pair<std::uint64_t, std::uint32_t>::first);
            return it != sorted.end() && it->first == h ? static_cast<int>(it->second) : -1;
        }
    }
};

// Calls l(std::type_identity<T_i>{}) for the i-th type of Ts... through a table of function pointers.
template<class T, class L> void call_for_type(L& l)
{
    l(std::type_identity<T>{});
}
template<class... Ts, class L> void call_for_index(const std::size_t i, L& l)
{
    static constexpr std::array<void (*)(L&), sizeof...(Ts)> table{&call_for_type<Ts, L>...};
    table[i](l);
}

// Up to this many types dispatch compares the record hash with each (inlined constants); above it, the
// hash_index lookup and one indirect call win.
inline constexpr std::size_t dispatch_linear_max = 4;
} // namespace detail

// Style 3. For each record in the frame, call f with `const T&` for the first T in Ts... whose schema hash
// matches (zero-copy when the payload is aligned, otherwise a stack copy — the reference is valid only during
// the call), with a literal_view for literal records, or with the record_view when nothing matches. A record
// whose payload length does not equal sizeof(T) is not delivered; the call returns error::length_mismatch after
// the walk (gk 1.1). f must be callable with each of those (an overload set / overloaded lambda).
// Matching a record to its T costs a few compares for up to detail::dispatch_linear_max types and one
// compile-time perfect-hash lookup above that (detail::hash_index, 2.1.9): never a walk over Ts or the registry.
template<class... Ts, class F>
    requires((flat_type<Ts> || dynamic_type<Ts> || detail::is_scalar_arg_v<Ts>) && ...)
result<std::size_t> dispatch(
    const std::span<const std::byte> bytes, F&& f,
    const dyn::dictionary* const dict = nullptr) noexcept(detail::decode_noexcept<Ts...>)
{
    (detail::touch_arg<Ts>(), ...);
    const auto fv = inspect(bytes, dict);
    if (!fv)
        return std::unexpected(fv.error());
    status bad;
    for (const record_view r : fv->records())
    {
        if (r.is_literal)
        {
            if constexpr (std::is_invocable_v<F&, literal_view>)
                f(literal_view{r.hash, registry::instance().find_literal(r.hash)});
            continue;
        }
        if (r.ak == fmt::arg_string)
        { // 1.1: string records are delivered as std::string_view (no Ts needed)
            if constexpr (std::is_invocable_v<F&, std::string_view>)
                f(r.text());
            else if constexpr (std::is_invocable_v<F&, const record_view&>)
                f(r);
            continue;
        }
        bool matched = false;
        auto try_one = [&]<class T>(std::type_identity<T>) noexcept(
                           detail::decode_noexcept<Ts...>) {
            if (matched || r.hash != arg_hash_v<T>)
                return;
            matched = true;
            if constexpr (detail::is_scalar_arg_v<T>)
            { // 1.1: scalar records, typed by kind
                if (r.payload.size() != sizeof(T))
                {
                    bad = std::unexpected(error::length_mismatch);
                    return;
                }
                if constexpr (std::is_invocable_v<F&, T>)
                {
                    T v;
                    if (!detail::get_object(v, r.payload.data())) // bool: repaired (G2-12)
                    { // 2.1.16: an enum leaf outside its range is not delivered, as a valueless variant (G2-6)
                        if (bad)
                            bad = std::unexpected(error::invalid_value);
                        return;
                    }
                    f(v);
                }
                return;
            }
            else if constexpr (dynamic_type<T>)
            { // v2: never zero-copy — a decoded temporary, valid during the call
                if constexpr (std::is_invocable_v<F&, const T&>)
                {
                    T tmp;
                    if (const auto d = detail::decode_image(tmp, r.payload); !d)
                    {
                        bad = d; // not delivered; the next record is still dispatched
                        return;
                    }
                    f(static_cast<const T&>(tmp));
                }
                return;
            }
            else
            {
                using U = detail::mapped_t<T>;
                if (r.payload.size() != sizeof(U))
                {
                    bad = std::unexpected(error::length_mismatch);
                    return;
                }
                if constexpr (std::is_invocable_v<F&, const T&>)
                {
                    if constexpr (!has_as_flat<std::remove_cv_t<T>>)
                    {
                        if (reinterpret_cast<std::uintptr_t>(r.payload.data()) % alignof(U) == 0 &&
                            detail::bools_canonical<U>(r.payload.data()))
                        {
                            f(**view<T>(r)); // zero-copy (§6.4)
                            return;
                        }
                    }
                    T tmp;
                    if (!detail::get_object(tmp, r.payload.data()))
                    { // 2.1.16: as above
                        if (bad)
                            bad = std::unexpected(error::invalid_value);
                        return;
                    }
                    f(static_cast<const T&>(tmp));
                }
            }
        };
        if constexpr (sizeof...(Ts) <= detail::dispatch_linear_max)
            (try_one(std::type_identity<Ts>{}), ...);
        else if (const int i = detail::hash_index<arg_hash_v<Ts>...>::find(r.hash); i >= 0)
            detail::call_for_index<Ts...>(static_cast<std::size_t>(i), try_one);
        if (!matched)
            if constexpr (std::is_invocable_v<F&, const record_view&>)
                f(r);
    }
    if (!bad)
        return std::unexpected(bad.error());
    return fv->header.frame_len;
}

} // namespace es::serreg
