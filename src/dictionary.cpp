// src/dictionary.cpp — dyn::dictionary: bounded ENTRY/RRI parsing, merge, lookups, record sizes, canonical
// JSON/TOML renderings and re-encoding to an RRI image (spec §6.7, §6.9; FR-W3, FR-W5, FR-D5).
#include "internal.hpp"

#include <es/serreg/dictionary.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/tools.hpp>

namespace es::serreg::dyn
{

std::uint64_t dictionary::computed_program_hash() const
{
    std::vector<std::uint64_t> hs;
    hs.reserve(types.size());
    for (const auto& [h, t] : types)
        hs.push_back(h); // std::map iterates in sorted key order
    return fingerprint64_u64s(hs);
}

namespace
{

using internal::append_quoted;
using internal::dec;
using internal::hex64;

constexpr int max_field_depth = 32;

bool read_name(fmt::cursor& c, const std::uint16_t len, std::string& out)
{
    std::span<const std::byte> s;
    if (!c.bytes(len, s))
        return false;
    out.assign(reinterpret_cast<const char*>(s.data()), s.size());
    return true;
}

// Minimum on-wire size of each repeated element; an on-wire count must fit in the remaining bytes at this
// rate before anything is allocated from it (cc F17 / gk 1.15: an 80-byte ENTRY must not reserve 6 MiB).
constexpr std::size_t min_field_bytes = 32, min_enumerator_bytes = 10, arg_bytes = 16,
                      min_entry_bytes = 8;

bool count_fits(const fmt::cursor& c, const std::size_t count, const std::size_t min_bytes)
{
    return count <= c.remaining() / min_bytes;
}

// FIELD: u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated,
//        u16 n_elem, u64 type_hash, u32 extent, name bytes, FIELD × n_elem
bool parse_field(fmt::cursor& c, field& f, const int depth)
{
    if (depth > max_field_depth)
        return false;
    std::uint16_t name_len{}, n_elem{};
    std::uint8_t k{}, rsv{};
    if (!c.read(name_len) || !c.read(k) || !c.read(rsv) || !c.read(f.offset) || !c.read(f.size) ||
        !c.read(f.align) || !c.read(f.since) || !c.read(f.deprecated) || !c.read(n_elem) ||
        !c.read(f.type_hash) || !c.read(f.extent) || !read_name(c, name_len, f.name))
        return false;
    f.k = static_cast<kind>(k);
    if (!count_fits(c, n_elem, min_field_bytes))
        return false;
    f.elem.resize(n_elem);
    for (auto& e : f.elem)
        if (!parse_field(c, e, depth + 1))
            return false;
    if (f.k == kind::array_)
    { // cc F2: exactly one element descriptor, non-empty elements, extent × elem within the field
        if (f.elem.size() != 1)
            return false;
        const field& e = f.elem[0];
        if (e.size == 0 && f.extent > 0)
            return false;
        if (static_cast<std::uint64_t>(e.size) * f.extent > f.size)
            return false;
    }
    else if (f.k == kind::string_ || f.k == kind::vector_ || f.k == kind::optional_ ||
             f.k == kind::variant_ || f.k == kind::map_ || f.k == kind::set_)
    { // v2: a reference slot {u32 off, u32 n}; element descriptors as the kind requires
        if (f.size != 8)
            return false;
        const std::size_t want = f.k == kind::string_    ? 0
                                 : f.k == kind::variant_ ? f.extent
                                 : f.k == kind::map_     ? 2
                                                         : 1;
        if (f.elem.size() != want || (f.k == kind::variant_ && f.extent == 0))
            return false;
        for (const field& e : f.elem)
            if (e.size == 0 &&
                e.k != kind::struct_) // an empty element image only for empty structs
                return false;
        if (f.k == kind::map_ || f.k == kind::set_)
        { // 1.6: `extent` is the entry stride: it must hold the key (and the value at its offset), be a
            // multiple of the entry alignment, and not be zero
            const field& key = f.elem[0];
            const std::uint64_t end =
                f.k == kind::map_ ? std::max<std::uint64_t>(
                                        key.size, std::uint64_t{f.elem[1].offset} + f.elem[1].size)
                                  : key.size;
            std::uint32_t al = key.align ? key.align : 1;
            if (f.k == kind::map_ && f.elem[1].align > al)
                al = f.elem[1].align;
            if (f.extent == 0 || f.extent < end || f.extent % al != 0 || key.offset != 0 ||
                (f.k == kind::map_ && f.elem[1].offset < key.size))
                return false;
        }
    }
    else if (f.k == kind::literal_ ||
             static_cast<std::uint8_t>(f.k) > static_cast<std::uint8_t>(kind::set_) ||
             (static_cast<std::uint8_t>(f.k) > static_cast<std::uint8_t>(kind::struct_) &&
              static_cast<std::uint8_t>(f.k) < static_cast<std::uint8_t>(kind::string_)) ||
             (static_cast<std::uint8_t>(f.k) > static_cast<std::uint8_t>(kind::byte_) &&
              static_cast<std::uint8_t>(f.k) < static_cast<std::uint8_t>(kind::enum_)) ||
             f.k == kind{})
        return false; // unknown kind values are rejected rather than guessed at
    // 2.1.12 (review D-01): sizes and alignments a writer can produce, nothing else. A scalar is exactly its
    // kind's size (wchar_t 2 or 4), an enum 1, 2, 4 or 8 bytes, and an alignment a power of two up to 4096
    // (0 counts as 1): a forged size or alignment would otherwise reach the decoders' stride arithmetic and the
    // visitors.
    if (is_scalar_kind(f.k) && f.size != kind_size(f.k) &&
        !(f.k == kind::wchar && (f.size == 2 || f.size == 4)))
        return false;
    if (f.k == kind::enum_ && f.size != 1 && f.size != 2 && f.size != 4 && f.size != 8)
        return false;
    if (f.align > 4096 || (f.align & (f.align - 1)) != 0)
        return false;
    return true;
}

bool parse_type(fmt::cursor& c, const bool is_enum, type& t)
{
    std::uint8_t flags_or_underlying{}, rsv{};
    std::uint16_t name_len{}, n{};
    if (!c.read(t.schema_hash) || !c.read(t.layout_hash) || !c.read(t.size) || !c.read(t.align) ||
        !c.read(flags_or_underlying) || !c.read(rsv) || !c.read(t.since) || !c.read(t.deprecated) ||
        !c.read(name_len) || !c.read(n) || !read_name(c, name_len, t.name))
        return false;
    t.is_enum = is_enum;
    if (is_enum)
    {
        t.underlying = static_cast<kind>(flags_or_underlying);
        t.is_flat = true;
        if (rsv & ~fmt::enum_flag_fixed)
            return false;
        t.enum_fixed = (rsv & fmt::enum_flag_fixed) != 0; // 1.6
        if (!count_fits(c, n, min_enumerator_bytes))
            return false;
        t.enumerators.reserve(n);
        for (std::uint16_t i = 0; i < n; ++i)
        {
            std::int64_t value{};
            std::uint16_t len{};
            std::string name;
            if (!c.read(value) || !c.read(len) || !read_name(c, len, name))
                return false;
            t.enumerators.emplace_back(std::move(name), value);
        }
    }
    else
    {
        if (flags_or_underlying & ~(fmt::struct_flag_flat | fmt::struct_flag_transparent))
            return false;
        t.is_flat = (flags_or_underlying & fmt::struct_flag_flat) != 0;
        t.transparent = (flags_or_underlying & fmt::struct_flag_transparent) != 0; // 1.6
        if (t.transparent && (t.is_flat || n != 1))
            return false; // a container argument: one dynamic field, see below
        if (!count_fits(c, n, min_field_bytes))
            return false;
        t.fields.resize(n);
        for (auto& f : t.fields)
        {
            if (!parse_field(c, f, 0))
                return false;
            // gk 1.4 / cc F2: every direct field lies inside [0, size). Nested types are checked when they are
            // themselves parsed; the decoder re-checks against the actual payload length.
            if (static_cast<std::uint64_t>(f.offset) + f.size > t.size)
                return false;
        }
        if (t.transparent)
        { // 1.6: the one field is the container itself — unnamed, at 0, a slot kind, the type's size its slot
            const field& f = t.fields[0];
            const bool slot_kind = f.k == kind::vector_ || f.k == kind::optional_ ||
                                   f.k == kind::variant_ || f.k == kind::map_ || f.k == kind::set_;
            if (!f.name.empty() || f.offset != 0 || !slot_kind || t.size != 8)
                return false;
        }
    }
    return true;
}

bool parse_literal(fmt::cursor& c, literal& l)
{
    std::uint32_t len{}, rsv{};
    std::span<const std::byte> s;
    if (!c.read(l.id) || !c.read(len) || !c.read(rsv) || !c.bytes(len, s))
        return false;
    l.text.assign(reinterpret_cast<const char*>(s.data()), s.size());
    return true;
}

const char* arg_kind_name(const call_arg& a)
{
    switch (a.ak)
    {
    case fmt::arg_literal:
        return "literal";
    case fmt::arg_scalar:
        return "scalar";
    case fmt::arg_string:
        return "string";
    case fmt::arg_const:
        return "const";
    default:
        return "type";
    }
}

bool parse_call(fmt::cursor& c, call& k)
{
    std::uint16_t n{};
    std::uint32_t rsv32{};
    if (!c.read(k.hash) || !c.read(n) || !c.read(k.flags) || !c.read(rsv32))
        return false;
    if (k.flags & ~(fmt::call_flag_print | fmt::call_flag_newline | fmt::call_flag_log))
        return false;
    if ((k.flags & fmt::call_flag_log) &&
        (k.flags & (fmt::call_flag_print | fmt::call_flag_newline)) !=
            (fmt::call_flag_print | fmt::call_flag_newline))
        return false; // 1.7: a log call is a println with an envelope
    if (!count_fits(c, n, arg_bytes))
        return false;
    k.args.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i)
    {
        std::uint8_t ak{}, kb{};
        call_arg a;
        if (!c.read(ak) || !c.read(kb) || !c.skip(6) || !c.read(a.hash))
            return false;
        if (ak > fmt::arg_const)
            return false;
        a.ak = static_cast<fmt::arg_kind>(ak);
        a.is_literal = a.ak == fmt::arg_literal;
        if (a.ak == fmt::arg_scalar)
        {
            a.k = static_cast<kind>(kb);
            if (kind_size(a.k) == 0 || a.hash != fingerprint64(to_string(a.k)))
                return false; // an unknown scalar kind, or a hash that is not its name's
        }
        else if (a.ak == fmt::arg_const)
        { // 1.5: kind = the scalar kind, or 0 for an enum (hash = its schema hash); then the u64 value, whose
            // bytes above the scalar's size are zero
            a.k = static_cast<kind>(kb);
            if (kb != 0 && (kind_size(a.k) == 0 || a.hash != fingerprint64(to_string(a.k))))
                return false;
            if (!c.read(a.value))
                return false;
            if (const std::uint32_t sz = kind_size(a.k);
                sz != 0 && sz < 8 && (a.value >> (8 * sz)) != 0)
                return false;
            if (a.k == kind::boolean && a.value > 1)
                return false;
        }
        else if (kb != 0)
            return false;
        if (a.ak == fmt::arg_string && a.hash != fingerprint64("string"))
            return false;
        k.args.push_back(a);
    }
    if (k.is_log())
    { // 1.7: level(const u8), file(literal), line(const u32), mask(const u8), time(u64), [thread u32],
        // [sequence u64], format(literal) — log.hpp's log_layout
        if (k.args.size() < 6)
            return false;
        const auto is_const = [&](const std::size_t i, const kind want) {
            return k.args[i].ak == fmt::arg_const && k.args[i].k == want;
        };
        if (!is_const(0, kind::u8) || k.args[1].ak != fmt::arg_literal || !is_const(2, kind::u32) ||
            !is_const(3, kind::u8) || k.args[3].value > 15 || k.args[4].ak != fmt::arg_scalar ||
            k.args[4].k != kind::u64)
            return false;
        std::size_t at = 5;
        if (k.args[3].value & 1)
        {
            if (at >= k.args.size() || k.args[at].ak != fmt::arg_scalar ||
                k.args[at].k != kind::u32)
                return false;
            ++at;
        }
        if (k.args[3].value & 2)
        {
            if (at >= k.args.size() || k.args[at].ak != fmt::arg_scalar ||
                k.args[at].k != kind::u64)
                return false;
            ++at;
        }
        return at < k.args.size() && k.args[at].ak == fmt::arg_literal;
    }
    // a print call: arg 0 is the format literal
    if (k.is_print() && (k.args.empty() || k.args[0].ak != fmt::arg_literal))
        return false;
    return true;
}

// ---- encoding (to_rri) ----------------------------------------------------------------------------------
std::size_t field_size(const field& f)
{
    std::size_t n = 32 + f.name.size();
    for (const auto& e : f.elem)
        n += field_size(e);
    return n;
}

void put_name(fmt::writer& w, const std::string& s)
{
    w.put(static_cast<std::uint16_t>(s.size()));
    w.put_bytes(s.data(), s.size());
}

void put_field(fmt::writer& w, const field& f)
{
    w.put(static_cast<std::uint16_t>(f.name.size()));
    w.put(static_cast<std::uint8_t>(f.k));
    w.put(std::uint8_t{0});
    w.put(f.offset);
    w.put(f.size);
    w.put(f.align);
    w.put(f.since);
    w.put(f.deprecated);
    w.put(static_cast<std::uint16_t>(f.elem.size()));
    w.put(f.type_hash);
    w.put(f.extent);
    w.put_bytes(f.name.data(), f.name.size());
    for (const auto& e : f.elem)
        put_field(w, e);
}

std::size_t type_body_size(const type& t)
{
    std::size_t n = 34 + t.name.size();
    if (t.is_enum)
        for (const auto& [name, value] : t.enumerators)
            n += 10 + name.size();
    else
        for (const auto& f : t.fields)
            n += field_size(f);
    return n;
}

// Every ENTRY: 8-byte header, body, zero padding to a multiple of 8.
void put_entry(std::vector<std::byte>& out, const std::uint8_t tag, const std::size_t body_size,
               const std::function<void(fmt::writer&)>& body)
{
    const std::size_t total = fmt::align8(fmt::entry_header_size + body_size);
    const std::size_t start = out.size();
    out.resize(start + total);
    fmt::writer w{out.data() + start};
    w.put(static_cast<std::uint32_t>(total));
    w.put(tag);
    w.put(std::uint8_t{1});
    w.put(std::uint16_t{0});
    body(w);
    w.zero(total - fmt::entry_header_size - body_size);
}

// ---- canonical text -------------------------------------------------------------------------------------
std::string indent(const int n)
{
    return std::string(static_cast<std::size_t>(n) * 2, ' ');
}

void json_field(std::string& out, const field& f, const int ind)
{
    const std::string i1 = indent(ind + 1);
    out += indent(ind) + "{\n";
    out += i1 + "\"align\": " + dec(f.align) + ",\n";
    out += i1 + "\"deprecated\": " + dec(f.deprecated) + ",\n";
    out += i1 + "\"elem\": [";
    for (std::size_t i = 0; i < f.elem.size(); ++i)
    {
        out += i ? ",\n" : "\n";
        json_field(out, f.elem[i], ind + 2);
    }
    out += f.elem.empty() ? "],\n" : "\n" + i1 + "],\n";
    out += i1 + "\"extent\": " + dec(f.extent) + ",\n";
    out += i1 + "\"kind\": \"" + std::string(to_string(f.k)) + "\",\n";
    out += i1 + "\"name\": ";
    append_quoted(out, f.name);
    out += ",\n";
    out += i1 + "\"offset\": " + dec(f.offset) + ",\n";
    out += i1 + "\"since\": " + dec(f.since) + ",\n";
    out += i1 + "\"size\": " + dec(f.size) + ",\n";
    out += i1 + "\"type_hash\": \"" + hex64(f.type_hash) + "\"\n";
    out += indent(ind) + "}";
}

void toml_field(std::string& out, const field& f, const std::string& table)
{
    out += "\n[[" + table + "]]\n";
    out += "align = " + dec(f.align) + "\n";
    out += "deprecated = " + dec(f.deprecated) + "\n";
    out += "extent = " + dec(f.extent) + "\n";
    out += "kind = \"" + std::string(to_string(f.k)) + "\"\n";
    out += "name = ";
    append_quoted(out, f.name);
    out += "\n";
    out += "offset = " + dec(f.offset) + "\n";
    out += "since = " + dec(f.since) + "\n";
    out += "size = " + dec(f.size) + "\n";
    out += "type_hash = \"" + hex64(f.type_hash) + "\"\n";
    for (const auto& e : f.elem)
        toml_field(out, e, table + ".elem");
}

// 2.1.12 (review D-03): what a conflict keeps of a definition — the facts the reports print, never the body.
constexpr std::size_t conflict_text_max = 256;
std::string clipped(std::string_view s)
{
    return s.size() <= conflict_text_max ? std::string(s)
                                         : std::string(s.substr(0, conflict_text_max)) + "...";
}
type conflict_summary(const type& t)
{
    type s;
    s.schema_hash = t.schema_hash;
    s.layout_hash = t.layout_hash;
    s.size = t.size;
    s.align = t.align;
    s.is_enum = t.is_enum;
    s.is_flat = t.is_flat;
    s.transparent = t.transparent;
    s.enum_fixed = t.enum_fixed;
    s.underlying = t.underlying;
    s.name = clipped(t.name);
    return s;
}
template<class C> bool room_for(dictionary& d, const std::vector<C>& list)
{
    if (list.size() < dictionary::max_conflicts)
        return true;
    ++d.conflicts_omitted;
    return false;
}

// First definition wins; a later, different body for the same schema hash is recorded (cc F16 / gk 1.10).
void add_type(dictionary& d, type&& t)
{
    const std::uint64_t h = t.schema_hash;
    const auto [it, inserted] = d.types.try_emplace(h, std::move(t));
    if (inserted)
        return;
    if (it->second != t && room_for(d, d.conflicts))
        d.conflicts.push_back({conflict_summary(it->second), conflict_summary(t)});
}
// G2-10 (2.1.3): literals and calls follow the same rule, and a differing body is recorded too.
std::string call_text(const call& k)
{
    std::string s = "flags " + dec(k.flags) + " (";
    for (std::size_t i = 0; i < k.args.size(); ++i)
        s += (i ? ", " : "") + std::to_string(static_cast<unsigned>(k.args[i].ak)) + ":" +
             hex64(k.args[i].hash);
    return s + ")";
}
void add_literal(dictionary& d, literal&& l)
{
    const std::uint64_t id = l.id;
    const auto [it, inserted] = d.literals.try_emplace(id, std::move(l));
    if (!inserted && it->second.text != l.text && room_for(d, d.id_conflicts))
        d.id_conflicts.push_back({false, id, clipped(it->second.text), clipped(l.text)});
}
void add_call(dictionary& d, call&& k)
{
    const std::uint64_t h = k.hash;
    const auto [it, inserted] = d.calls.try_emplace(h, std::move(k));
    if (!inserted && it->second != k && room_for(d, d.id_conflicts))
        d.id_conflicts.push_back({true, h, clipped(call_text(it->second)), clipped(call_text(k))});
}

} // namespace

// ---- parsing --------------------------------------------------------------------------------------------
result<void> dictionary::add_entries(const std::span<const std::byte> entries)
{
    fmt::cursor c(entries);
    while (c.remaining() > 0)
    {
        std::uint32_t total_len{};
        std::uint8_t tag{}, version{};
        std::uint16_t rsv{};
        if (!c.read(total_len))
            return std::unexpected(error::corrupt_dictionary);
        if (total_len == 0)
        { // trailing zero padding (e.g. section alignment) is tolerated
            for (const std::byte* p = c.p; p != c.end; ++p)
                if (*p != std::byte{0})
                    return std::unexpected(error::corrupt_dictionary);
            return {};
        }
        if (total_len < fmt::entry_header_size || total_len % 8 != 0 ||
            total_len - sizeof(std::uint32_t) > c.remaining())
            return std::unexpected(error::corrupt_dictionary);
        if (!c.read(tag) || !c.read(version) || !c.read(rsv))
            return std::unexpected(error::corrupt_dictionary);
        std::span<const std::byte> body;
        if (!c.bytes(total_len - fmt::entry_header_size, body))
            return std::unexpected(error::corrupt_dictionary);
        if (version != 1)
            continue; // unknown entry version: skip (forward compatibility)
        fmt::cursor b(body);
        switch (tag)
        {
        case fmt::tag_struct:
        case fmt::tag_enum: {
            type t;
            if (!parse_type(b, tag == fmt::tag_enum, t))
                return std::unexpected(error::corrupt_dictionary);
            add_type(*this, std::move(t));
            break;
        }
        case fmt::tag_literal: {
            literal l;
            if (!parse_literal(b, l))
                return std::unexpected(error::corrupt_dictionary);
            add_literal(*this, std::move(l));
            break;
        }
        case fmt::tag_call: {
            call k;
            if (!parse_call(b, k))
                return std::unexpected(error::corrupt_dictionary);
            add_call(*this, std::move(k));
            break;
        }
        default:
            break; // alias/image entries (§6.7) and future tags are skipped
        }
    }
    return {};
}

result<void> dictionary::add_rri(const std::span<const std::byte> rri)
{
    fmt::cursor c(rri);
    std::array<char, 4> magic{};
    std::uint16_t major{}, minor{};
    std::uint32_t flags{}, hash_algo{}, n_entries{}, reserved{};
    std::uint64_t prog{}, abi_v{};
    if (!c.read(magic))
        return std::unexpected(error::truncated);
    if (magic != fmt::rri_magic)
        return std::unexpected(error::bad_magic);
    if (!c.read(major) || !c.read(minor) || !c.read(flags) || !c.read(hash_algo) ||
        !c.read(n_entries) || !c.read(prog) || !c.read(reserved) || !c.read(abi_v))
        return std::unexpected(error::truncated);
    // 1.6 and 1.7 (format_minor_oldest): 1.5 lacks the map/set shapes, 1.4 constant arguments, 1.3 may carry
    // pre-2.1.14 canonical names, older minors FNV-1a ids this library no longer computes;
    // newer ones are not guessed at.
    if (major != fmt::format_major || minor < fmt::format_minor_oldest || minor > fmt::format_minor)
        return std::unexpected(error::bad_version);
    if (hash_algo != fmt::hash_algo_xxh3_64)
        return std::unexpected(error::unsupported);
    // The entry stream is self-delimiting; n_entries bounds how far we read so trailing bytes are ignored.
    if (n_entries > get_decode_limits().dictionary_entries) // 2.1.16 (FR-D5)
        return std::unexpected(error::corrupt_dictionary);
    if (!count_fits(c, n_entries, min_entry_bytes))
        return std::unexpected(error::corrupt_dictionary);
    const std::byte* const begin = c.p;
    for (std::uint32_t i = 0; i < n_entries; ++i)
    {
        std::uint32_t total_len{};
        if (!c.read(total_len) || total_len < fmt::entry_header_size || total_len % 8 != 0 ||
            !c.skip(total_len - sizeof(std::uint32_t)))
            return std::unexpected(error::corrupt_dictionary);
    }
    const auto r = add_entries({begin, static_cast<std::size_t>(c.p - begin)});
    if (!r)
        return r;
    if (program_hash == 0)
        program_hash = prog;
    if (!abi_known)
    { // the first source decides; a merged source with another ABI is a problem the walkers report
        abi = abi_v;
        abi_known = true;
    }
    else if (abi != abi_v)
        abi_mixed = true;
    return {};
}

void dictionary::merge(const dictionary& other)
{
    for (const auto& [h, t] : other.types)
        add_type(*this, type(t));
    for (const auto& [id, l] : other.literals)
        add_literal(*this, literal(l));
    for (const auto& [h, k] : other.calls)
        add_call(*this, call(k));
    for (const auto& c : other.conflicts)
        if (room_for(*this, conflicts))
            conflicts.push_back(c);
    for (const auto& c : other.id_conflicts)
        if (room_for(*this, id_conflicts))
            id_conflicts.push_back(c);
    conflicts_omitted += other.conflicts_omitted;
    if (program_hash == 0)
        program_hash = other.program_hash;
    if (other.abi_known)
    {
        if (!abi_known)
        {
            abi = other.abi;
            abi_known = true;
        }
        else if (abi != other.abi)
            abi_mixed = true;
    }
    abi_mixed = abi_mixed || other.abi_mixed;
}

// ---- lookups --------------------------------------------------------------------------------------------
const type* dictionary::find_type(const std::uint64_t schema_hash) const noexcept
{
    const auto it = types.find(schema_hash);
    return it == types.end() ? nullptr : &it->second;
}
const type* dictionary::find_type_by_name(const std::string_view name) const noexcept
{
    for (const auto& [h, t] : types)
        if (t.name == name)
            return &t;
    return nullptr;
}
const literal* dictionary::find_literal(const std::uint64_t id) const noexcept
{
    const auto it = literals.find(id);
    return it == literals.end() ? nullptr : &it->second;
}
const call* dictionary::find_call(const std::uint64_t call_hash) const noexcept
{
    const auto it = calls.find(call_hash);
    return it == calls.end() ? nullptr : &it->second;
}

result<std::uint32_t> dictionary::record_size(const call_arg& a) const noexcept
{
    if (a.is_literal || a.ak == fmt::arg_const)
        return 0; // FR-L6: a literal occupies no bytes in a compact frame; 1.5: nor does a constant
    if (a.ak == fmt::arg_scalar)
        return scalar_size(a.k);
    if (a.ak == fmt::arg_string)
        return std::unexpected(error::unsupported); // dynamic: u32 len + bytes, read by the walker
    const type* const t = find_type(a.hash);
    if (!t)
        return std::unexpected(error::unknown_type);
    if (!t->is_flat)
        return std::unexpected(error::unsupported); // dynamic members: v2
    return t->size;
}

// ---- 2.1.16 (B7): a constant's value as the scalar or enumerator it stands for --------------------------
// Integers and finite floating values as numbers, bool as true/false, characters and enumerator names as
// quoted strings, a non-finite float or an unknown enumerator as a quoted string (the same spellings are valid
// JSON and TOML). The exact bits stay next to it ("bits"), so a reader that needs them never parses text.
namespace
{
std::string const_value_text(const dictionary& d, const call_arg& a)
{
    const auto quoted = [](const std::string& s) { // the text renderer quotes characters already
        return s.starts_with('"') ? s : "\"" + s + "\"";
    };
    if (kind_size(a.k) != 0)
    {
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(&a.value),
                                               kind_size(a.k));
        const std::string text = tools::value_text(a.k, bytes);
        switch (a.k)
        {
        case kind::boolean:
            return a.value ? "true" : "false";
        case kind::f32:
        case kind::f64: {
            const bool finite =
                text.find("nan") == std::string::npos && text.find("inf") == std::string::npos;
            if (!finite)
                return quoted(text);
            return text.find_first_of(".eE") == std::string::npos ? text + ".0" : text;
        }
        case kind::char_:
        case kind::char8:
        case kind::char16:
        case kind::char32:
        case kind::wchar:
        case kind::byte_:
            return quoted(text);
        default:
            return text; // integers
        }
    }
    const type* const t = d.find_type(a.hash); // an enum constant: hash = the enum's schema hash
    if (t && t->is_enum)
        for (const auto& [name, value] : t->enumerators)
            if (static_cast<std::uint64_t>(value) == a.value ||
                (value < 0 && kind_size(t->underlying) < 8 &&
                 (static_cast<std::uint64_t>(value) &
                  ((std::uint64_t{1} << (8 * kind_size(t->underlying))) - 1)) == a.value))
                return quoted(name);
    return quoted(dec(a.value)); // an unknown enumerator (or an unknown enum): the number, as text
}
} // namespace

// ---- canonical JSON (keys sorted, 2-space indent, arrays in hash order) ----------------------------------
std::string dictionary::to_json() const
{
    std::string out = "{\n  \"calls\": [";
    bool first = true;
    for (const auto& [h, k] : calls)
    {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\n      \"args\": [";
        for (std::size_t i = 0; i < k.args.size(); ++i)
        {
            out += i ? ",\n" : "\n";
            out += "        {\"hash\": \"" + hex64(k.args[i].hash) + "\", \"kind\": \"" +
                   arg_kind_name(k.args[i]) + "\"";
            if (k.args[i].ak == fmt::arg_scalar ||
                (k.args[i].ak == fmt::arg_const && kind_size(k.args[i].k) != 0))
                out += ", \"scalar\": \"" + std::string(to_string(k.args[i].k)) + "\"";
            if (k.args[i].ak ==
                fmt::arg_const) // 1.5: the value; 2.1.16: typed, plus the exact bits
                out += ", \"value\": " + const_value_text(*this, k.args[i]) + ", \"bits\": \"" +
                       hex64(k.args[i].value) + "\"";
            out += "}";
        }
        out += k.args.empty() ? "],\n" : "\n      ],\n";
        if (k.flags) // 1.1: only print calls carry it, so 1.0 renderings are unchanged
            out += std::string("      \"form\": \"") +
                   (k.is_log()    ? "log"
                    : k.newline() ? "println"
                                  : "print") +
                   "\",\n";
        out += "      \"hash\": \"" + hex64(h) + "\"\n    }";
    }
    out += calls.empty() ? "],\n" : "\n  ],\n";
    out += "  \"literals\": [";
    first = true;
    for (const auto& [id, l] : literals)
    {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\"id\": \"" + hex64(id) + "\", \"text\": ";
        append_quoted(out, l.text);
        out += "}";
    }
    out += literals.empty() ? "],\n" : "\n  ],\n";
    out += "  \"program_hash\": \"" + hex64(program_hash) + "\",\n";
    out += "  \"types\": [";
    first = true;
    for (const auto& [h, t] : types)
    {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\n";
        out += "      \"align\": " + dec(t.align) + ",\n";
        out += "      \"deprecated\": " + dec(t.deprecated) + ",\n";
        if (t.is_enum)
        {
            out += "      \"enumerators\": [";
            for (std::size_t i = 0; i < t.enumerators.size(); ++i)
            {
                out += i ? ",\n" : "\n";
                out += "        {\"name\": ";
                append_quoted(out, t.enumerators[i].first);
                out += ", \"value\": " + dec(t.enumerators[i].second) + "}";
            }
            out += t.enumerators.empty() ? "],\n" : "\n      ],\n";
        }
        else
        {
            out += "      \"fields\": [";
            for (std::size_t i = 0; i < t.fields.size(); ++i)
            {
                out += i ? ",\n" : "\n";
                json_field(out, t.fields[i], 4);
            }
            out += t.fields.empty() ? "],\n" : "\n      ],\n";
        }
        if (t.is_enum) // 1.6
            out += std::string("      \"fixed\": ") + (t.enum_fixed ? "true" : "false") + ",\n";
        out += std::string("      \"is_enum\": ") + (t.is_enum ? "true" : "false") + ",\n";
        out += std::string("      \"is_flat\": ") + (t.is_flat ? "true" : "false") + ",\n";
        out += "      \"layout_hash\": \"" + hex64(t.layout_hash) + "\",\n";
        out += "      \"name\": ";
        append_quoted(out, t.name);
        out += ",\n";
        out += "      \"schema_hash\": \"" + hex64(h) + "\",\n";
        out += "      \"since\": " + dec(t.since) + ",\n";
        out += "      \"size\": " + dec(t.size);
        if (t.transparent) // 1.6
            out += ",\n      \"transparent\": true";
        if (t.is_enum)
            out += ",\n      \"underlying\": \"" + std::string(to_string(t.underlying)) + "\"";
        out += "\n    }";
    }
    out += types.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

// ---- canonical TOML (same content; arrays of tables) ----------------------------------------------------
std::string dictionary::to_toml() const
{
    std::string out = "program_hash = \"" + hex64(program_hash) + "\"\n";
    for (const auto& [h, t] : types)
    {
        out += "\n[[types]]\n";
        out += "align = " + dec(t.align) + "\n";
        out += "deprecated = " + dec(t.deprecated) + "\n";
        if (t.is_enum) // 1.6
            out += std::string("fixed = ") + (t.enum_fixed ? "true" : "false") + "\n";
        out += std::string("is_enum = ") + (t.is_enum ? "true" : "false") + "\n";
        out += std::string("is_flat = ") + (t.is_flat ? "true" : "false") + "\n";
        out += "layout_hash = \"" + hex64(t.layout_hash) + "\"\n";
        out += "name = ";
        append_quoted(out, t.name);
        out += "\n";
        out += "schema_hash = \"" + hex64(h) + "\"\n";
        out += "since = " + dec(t.since) + "\n";
        out += "size = " + dec(t.size) + "\n";
        if (t.transparent) // 1.6
            out += "transparent = true\n";
        if (t.is_enum)
        {
            out += "underlying = \"" + std::string(to_string(t.underlying)) + "\"\n";
            for (const auto& [name, value] : t.enumerators)
            {
                out += "\n[[types.enumerators]]\nname = ";
                append_quoted(out, name);
                out += "\nvalue = " + dec(value) + "\n";
            }
        }
        else
        {
            for (const auto& f : t.fields)
                toml_field(out, f, "types.fields");
        }
    }
    for (const auto& [id, l] : literals)
    {
        out += "\n[[literals]]\nid = \"" + hex64(id) + "\"\ntext = ";
        append_quoted(out, l.text);
        out += "\n";
    }
    for (const auto& [h, k] : calls)
    {
        out += "\n[[calls]]\nhash = \"" + hex64(h) + "\"\n";
        if (k.flags)
            out += std::string("form = \"") +
                   (k.is_log()    ? "log"
                    : k.newline() ? "println"
                                  : "print") +
                   "\"\n";
        for (const auto& a : k.args)
        {
            out += "\n[[calls.args]]\nhash = \"" + hex64(a.hash) + "\"\nkind = \"";
            out += arg_kind_name(a);
            out += "\"\n";
            if (a.ak == fmt::arg_scalar || (a.ak == fmt::arg_const && kind_size(a.k) != 0))
                out += "scalar = \"" + std::string(to_string(a.k)) + "\"\n";
            if (a.ak == fmt::arg_const) // 2.1.16: typed, plus the exact bits
                out += "value = " + const_value_text(*this, a) + "\nbits = \"" + hex64(a.value) +
                       "\"\n";
        }
    }
    return out;
}

// ---- re-encoding ----------------------------------------------------------------------------------------
std::vector<std::byte> to_rri(const dictionary& d, const std::uint32_t flags)
{
    std::vector<std::byte> out(fmt::rri_header_size);
    const std::uint32_t n_entries =
        static_cast<std::uint32_t>(d.types.size() + d.literals.size() + d.calls.size());
    {
        fmt::writer w{out.data()};
        w.put(fmt::rri_magic);
        w.put(fmt::format_major);
        w.put(fmt::format_minor);
        w.put(flags);
        w.put(fmt::hash_algo_xxh3_64);
        w.put(n_entries);
        w.put(d.program_hash);
        w.put(std::uint32_t{0});
        w.put(
            d.abi); // the source's ABI, not this process's: a re-encoded dictionary keeps its origin
    }
    for (const auto& [h, t] : d.types)
    {
        put_entry(out, t.is_enum ? fmt::tag_enum : fmt::tag_struct, type_body_size(t),
                  [&t](fmt::writer& w) {
                      w.put(t.schema_hash);
                      w.put(t.layout_hash);
                      w.put(t.size);
                      w.put(t.align);
                      w.put(t.is_enum ? static_cast<std::uint8_t>(t.underlying)
                                      : static_cast<std::uint8_t>(
                                            (t.is_flat ? fmt::struct_flag_flat : 0) |
                                            (t.transparent ? fmt::struct_flag_transparent : 0)));
                      w.put(static_cast<std::uint8_t>(
                          t.is_enum && t.enum_fixed ? fmt::enum_flag_fixed : 0)); // 1.6
                      w.put(t.since);
                      w.put(t.deprecated);
                      w.put(static_cast<std::uint16_t>(t.name.size()));
                      w.put(static_cast<std::uint16_t>(t.is_enum ? t.enumerators.size()
                                                                 : t.fields.size()));
                      w.put_bytes(t.name.data(), t.name.size());
                      if (t.is_enum)
                          for (const auto& [name, value] : t.enumerators)
                          {
                              w.put(value);
                              put_name(w, name);
                          }
                      else
                          for (const auto& f : t.fields)
                              put_field(w, f);
                  });
    }
    for (const auto& [id, l] : d.literals)
    {
        put_entry(out, fmt::tag_literal, 16 + l.text.size(), [&l](fmt::writer& w) {
            w.put(l.id);
            w.put(static_cast<std::uint32_t>(l.text.size()));
            w.put(std::uint32_t{0});
            w.put_bytes(l.text.data(), l.text.size());
        });
    }
    for (const auto& [h, k] : d.calls)
    {
        std::size_t n_const = 0;
        for (const auto& a : k.args)
            n_const += a.ak == fmt::arg_const ? 1 : 0;
        put_entry(out, fmt::tag_call, 16 + 16 * k.args.size() + fmt::const_value_size * n_const,
                  [&k](fmt::writer& w) {
                      w.put(k.hash);
                      w.put(static_cast<std::uint16_t>(k.args.size()));
                      w.put(k.flags);
                      w.put(std::uint32_t{0});
                      for (const auto& a : k.args)
                      {
                          w.put(static_cast<std::uint8_t>(a.ak));
                          w.put(static_cast<std::uint8_t>(
                              a.ak == fmt::arg_scalar || a.ak == fmt::arg_const ? a.k : kind{}));
                          w.zero(6);
                          w.put(a.hash);
                          if (a.ak == fmt::arg_const)
                              w.put(a.value);
                      }
                  });
    }
    return out;
}

} // namespace es::serreg::dyn

// ---- tools:: dictionary sources that need no ELF/file code beyond read_file ------------------------------
namespace es::serreg::tools
{

result<dyn::dictionary> load_dictionary_rri(const std::span<const std::byte> rri)
{
    dyn::dictionary d;
    const auto r = d.add_rri(rri);
    if (!r)
        return std::unexpected(r.error());
    return d;
}

result<dyn::dictionary> load_dictionary_file(const std::string_view path)
{
    const auto bytes = read_file(path);
    if (!bytes)
        return std::unexpected(bytes.error());
    return load_dictionary_rri(*bytes);
}

} // namespace es::serreg::tools
