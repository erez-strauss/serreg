// src/reader.cpp — capture walking shared by tools::verify and tools::dump (spec §6.5, FR-W1/W2/W6/W7,
// FR-D5): FILE header + embedded RRI, or a bare frame stream; dictionary frames are merged in order;
// compact frames are cut into records using the calls table, full frames using per-record headers.
#include "internal.hpp"

#include <es/serreg/hash.hpp>
#include <es/serreg/tools.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace es::serreg::internal
{
namespace
{
// 2.1.16 (FR-D5): the process-wide limits. A mutex, not atomics: a reader takes one copy per walk.
constinit std::mutex g_limits_mu;
constinit decode_limits g_limits{};
} // namespace
} // namespace es::serreg::internal

namespace es::serreg
{
decode_limits get_decode_limits() noexcept
{
    std::lock_guard lock(internal::g_limits_mu);
    return internal::g_limits;
}
void set_decode_limits(const decode_limits& l) noexcept
{
    std::lock_guard lock(internal::g_limits_mu);
    internal::g_limits = l;
}
} // namespace es::serreg

namespace es::serreg::internal
{
void add_problem(std::vector<std::string>& problems, std::size_t& omitted, const std::size_t limit,
                 std::string what)
{
    if (problems.size() < limit)
        problems.push_back(std::move(what));
    else
        ++omitted;
}

namespace
{

// Byte length of an RRI (header + n_entries entries) starting at `rri`; used to skip the initial dictionary
// area of a back-patched file (§11: the old area is left in place and the header points at the new one).
result<std::size_t> rri_length(const std::span<const std::byte> rri)
{
    fmt::cursor c(rri);
    std::array<char, 4> magic{};
    std::uint32_t n_entries{};
    if (!c.read(magic) || magic != fmt::rri_magic)
        return std::unexpected(error::bad_magic);
    if (!c.skip(12) || !c.read(n_entries) || !c.skip(20)) // 1.3: program hash, reserved, abi tag
        return std::unexpected(error::truncated);
    for (std::uint32_t i = 0; i < n_entries; ++i)
    {
        std::uint32_t total_len{};
        // G2-10 (2.1.3): the same rule as add_entries — an entry is a multiple of 8 bytes — so a corrupt original
        // area cannot move the start of the frames to an unaligned offset
        if (!c.read(total_len) || total_len < fmt::entry_header_size || total_len % 8 != 0 ||
            !c.skip(total_len - sizeof(std::uint32_t)))
            return std::unexpected(error::corrupt_dictionary);
    }
    return static_cast<std::size_t>(c.p - rri.data());
}

} // namespace

// G2-9 (2.1.3): the one reading of a frame header, for the in-memory walker and the block reader alike. `room` is
// how many bytes the frame may span (to the end of the frames, or the block reader's 1 GiB cap).
frame_class classify_frame(const std::span<const std::byte> header, const std::uint64_t room,
                           frame_info& f) noexcept
{
    fmt::cursor c(header.first(fmt::frame_header_size));
    c.read(f.frame_len);
    c.read(f.flags);
    c.read(f.n_records);
    c.read(f.call_hash);
    if (f.frame_len < fmt::frame_header_size || f.frame_len % 8 != 0 || f.frame_len > room)
        return frame_class::bad_len; // the walk cannot resynchronise
    f.is_dict = (f.flags & fmt::frame_flag_dict) != 0 || f.call_hash == fmt::dict_frame_sentinel;
    f.is_full = (f.flags & fmt::frame_flag_full) != 0;
    f.has_crc = (f.flags & fmt::frame_flag_crc) != 0;              // 1.7
    f.is_checkpoint = (f.flags & fmt::frame_flag_checkpoint) != 0; // 1.7
    f.is_info = (f.flags & fmt::frame_flag_info) != 0;             // 1.7
    if ((f.flags & ~(fmt::frame_flag_full | fmt::frame_flag_dict | fmt::frame_flag_crc |
                     fmt::frame_flag_checkpoint | fmt::frame_flag_info)) != 0)
        return frame_class::unknown_flags; // a newer frame layout: skipped, never guessed at
    if (f.has_crc && f.frame_len < fmt::frame_header_size + fmt::crc_trailer_size)
        return frame_class::bad_len;
    return frame_class::ok;
}

// G2-9/G2-10 (2.1.3): what the merged dictionary itself is wrong about, reported by both walkers.
void dictionary_problems(const dyn::dictionary& d, std::vector<std::string>& problems,
                         std::optional<error>& first)
{
    const auto note = [&](std::string what, const error e) {
        problems.push_back(std::move(what));
        if (!first)
            first = e;
    };
    // cc F15/F16: two definitions of one schema hash anywhere in the merged dictionary are a problem of the capture
    for (const auto& c : d.conflicts)
        note("schema conflict: " + c.kept.name + " " + hex64(c.kept.schema_hash) + " (layout " +
                 hex64(c.kept.layout_hash) + ", size " + dec(c.kept.size) + ") vs " +
                 c.dropped.name + " (layout " + hex64(c.dropped.layout_hash) + ", size " +
                 dec(c.dropped.size) + ")",
             error::schema_conflict);
    for (const auto& c : d.id_conflicts)
        note(std::string(c.is_call ? "call" : "literal") + " conflict: " + hex64(c.id) +
                 " has two bodies: \"" + c.kept + "\" vs \"" + c.dropped + "\"",
             error::schema_conflict);
    if (d.conflicts_omitted) // 2.1.12 (review D-03): only the first dictionary::max_conflicts of each kind are kept
        note(dec(d.conflicts_omitted) + " more conflicts not listed", error::schema_conflict);
    if (d.abi_mixed) // one capture, dictionaries from two writer ABIs: scalar widths (wchar) cannot both be right
        note("abi: dictionaries of different writer ABIs in one capture", error::abi_mismatch);
}

// 1.7: the CRC32C trailer covers everything before it (declared in internal.hpp).
bool check_crc(const std::span<const std::byte> frame) noexcept
{
    std::uint32_t stored{};
    std::memcpy(&stored, frame.data() + frame.size() - fmt::crc_trailer_size, 4);
    return crc32c(frame.first(frame.size() - fmt::crc_trailer_size)) == stored;
}
// 1.7: a dictionary frame whose RRI is minor ≥ 7 ends with the u64 forward link; the RRI parser ignores trailing
// bytes anyway, so the payload is passed whole and the link is read by the chain follower.
std::span<const std::byte> dict_payload(const std::span<const std::byte> body) noexcept
{
    return body;
}

namespace
{

struct walker
{
    std::span<const std::byte> cap;
    dyn::dictionary dict;
    walk_result res;
    bool stop_on_problem{};
    const frame_fn& on_frame;
    const record_fn& on_record;
    bool stopped{};                             // callback asked to stop, or a problem stopped us
    decode_limits limits = get_decode_limits(); // 2.1.16: one copy per walk
    std::uint64_t next_index = 0; // frame_info::index: every frame, meta frames included
    const dyn::dictionary* shared =
        nullptr; // 2.1.16: the windowed reader's dictionary (per-frame use)

    const dyn::dictionary& D() const noexcept
    {
        return shared ? *shared : dict;
    }

    // Records a problem; returns false when the walk must stop.
    bool problem(const frame_info& f, const std::string& what,
                 const error code = error::length_mismatch)
    {
        add_problem(res.problems, res.problems_omitted, limits.problems,
                    "frame " + dec(f.index) + " @" + dec(f.offset) + ": " + what);
        if (!res.first_error)
            res.first_error = code;
        if (stop_on_problem)
            stopped = true;
        return !stop_on_problem;
    }

    bool emit(const frame_info& f, const record_info& r)
    {
        ++res.records;
        if (!on_record(D(), f, r))
        {
            stopped = true;
            return false;
        }
        return true;
    }

    void dict_frame(const frame_info& f, const std::span<const std::byte> payload)
    {
        dyn::dictionary delta;
        if (const auto r = delta.add_rri(payload); !r)
        {
            problem(f, "dictionary frame: " + std::string(to_string(r.error())), r.error());
            return;
        }
        dict.merge(delta);
    }

    void full_frame(const frame_info& f, const std::span<const std::byte> body)
    {
        const dyn::call* const k = D().find_call(f.call_hash);
        if (k && k->args.size() != f.n_records &&
            !problem(f, "n_records " + dec(f.n_records) + " != call arity " + dec(k->args.size())))
            return;
        std::size_t pos = 0;
        for (std::uint32_t i = 0; i < f.n_records; ++i)
        {
            fmt::cursor c(body.subspan(pos));
            record_info r;
            r.index = i;
            std::uint32_t payload_len{};
            std::uint16_t layout{}, rsv{};
            if (!c.read(r.schema_hash) || !c.read(payload_len) || !c.read(layout) || !c.read(rsv))
            {
                problem(f, "record " + dec(i) + ": truncated record header");
                return;
            }
            if (!c.bytes(payload_len, r.payload))
            {
                problem(f, "record " + dec(i) + ": payload_len " + dec(payload_len) +
                               " exceeds frame");
                return;
            }
            if (layout != 0 &&
                !problem(f, "record " + dec(i) + ": non-native layout " + dec(layout),
                         error::unsupported))
                return;
            const dyn::call_arg* const arg = (k && i < k->args.size()) ? &k->args[i] : nullptr;
            // Without the call signature a literal record is recognised by its id being in the literal table,
            // a string record by the "string" hash (1.1); scalars need the signature.
            const bool literal = arg ? arg->is_literal
                                     : (payload_len == 8 && !D().find_type(r.schema_hash) &&
                                        D().find_literal(r.schema_hash));
            r.ak = arg                                        ? arg->ak
                   : literal                                  ? fmt::arg_literal
                   : r.schema_hash == fingerprint64("string") ? fmt::arg_string
                                                              : fmt::arg_type;
            if (r.ak == fmt::arg_const)
            { // 1.5: a full-records frame carries the constant as the scalar or enum record it stands for
                r.is_const = true;
                r.ak = kind_size(arg->k) != 0 ? fmt::arg_scalar : fmt::arg_type;
                if (arg->hash == r.schema_hash &&
                    (payload_len > fmt::const_value_size ||
                     std::memcmp(r.payload.data(), &arg->value, payload_len) != 0) &&
                    !problem(f,
                             "record " + dec(i) + ": constant differs from the call's value " +
                                 hex64(arg->value),
                             error::hash_mismatch))
                    return;
            }
            if (arg && arg->hash != r.schema_hash &&
                !problem(f,
                         "record " + dec(i) +
                             (arg->is_literal ? ": literal id mismatch: header " : ": schema ") +
                             hex64(r.schema_hash) +
                             (arg->is_literal ? ", call arg " : " != call arg ") + hex64(arg->hash),
                         error::hash_mismatch))
                return;
            if (r.ak == fmt::arg_scalar)
            { // 1.1
                r.k = arg->k;
                if (payload_len != D().scalar_size(r.k) &&
                    !problem(f, "record " + dec(i) + ": scalar payload_len " + dec(payload_len) +
                                    " != size of " + std::string(to_string(r.k))))
                    return;
                pos = fmt::align8(pos + fmt::full_record_header_size + payload_len);
                if (!emit(f, r))
                    return;
                continue;
            }
            if (r.ak == fmt::arg_string)
            { // 1.1: payload = u32 len + bytes; the prefix must agree with payload_len
                std::uint32_t inner = 0;
                if (payload_len >= fmt::string_len_prefix)
                    std::memcpy(&inner, r.payload.data(), 4);
                if ((payload_len < fmt::string_len_prefix ||
                     inner != payload_len - fmt::string_len_prefix) &&
                    !problem(f, "record " + dec(i) + ": string length prefix " + dec(inner) +
                                    " does not match payload_len " + dec(payload_len)))
                    return;
                r.payload = payload_len >= fmt::string_len_prefix
                                ? r.payload.subspan(fmt::string_len_prefix)
                                : std::span<const std::byte>{};
                pos = fmt::align8(pos + fmt::full_record_header_size + payload_len);
                if (!emit(f, r))
                    return;
                continue;
            }
            if (literal)
            {
                r.is_literal = true;
                if (payload_len != 8)
                {
                    if (!problem(f, "record " + dec(i) + ": literal payload_len " +
                                        dec(payload_len) + " != 8"))
                        return;
                }
                else
                    std::memcpy(&r.literal_id, r.payload.data(), 8);
                // gk 1.4: with a call signature, both the header slot and the payload must carry the signature's id.
                if (arg && payload_len == 8 && r.literal_id != arg->hash &&
                    !problem(f,
                             "record " + dec(i) + ": literal id mismatch: header " +
                                 hex64(r.schema_hash) + ", payload " + hex64(r.literal_id) +
                                 ", call arg " + hex64(arg->hash),
                             error::hash_mismatch))
                    return;
            }
            else
            {
                r.type = D().find_type(r.schema_hash);
                if (!r.type)
                {
                    if (!problem(f, "record " + dec(i) + ": unknown type " + hex64(r.schema_hash),
                                 error::unknown_type))
                        return;
                }
                else if (r.type->is_flat && r.type->size != payload_len &&
                         !problem(f, "record " + dec(i) + ": payload_len " + dec(payload_len) +
                                         " != size of " + r.type->name + " (" + dec(r.type->size) +
                                         ")"))
                    return;
                else if (!r.type->is_flat)
                { // v2: payload = u32 len, u32 0, image; the prefix must agree with payload_len
                    std::uint32_t inner = 0;
                    if (payload_len >= fmt::image_prefix)
                        std::memcpy(&inner, r.payload.data(), 4);
                    if ((payload_len < fmt::image_prefix ||
                         inner != payload_len - fmt::image_prefix) &&
                        !problem(f, "record " + dec(i) + ": image length prefix " + dec(inner) +
                                        " does not match payload_len " + dec(payload_len)))
                        return;
                    r.payload = payload_len >= fmt::image_prefix
                                    ? r.payload.subspan(fmt::image_prefix)
                                    : std::span<const std::byte>{};
                }
            }
            pos = fmt::align8(pos + fmt::full_record_header_size + payload_len);
            if (!emit(f, r))
                return;
        }
        if (pos < body.size() && body.size() - pos >= 8)
            problem(f, dec(body.size() - pos) + " trailing bytes after last record");
    }

    void compact_frame(const frame_info& f, const std::span<const std::byte> body)
    {
        const dyn::call* const k = D().find_call(f.call_hash);
        if (!k)
        {
            problem(f,
                    "unknown call " + hex64(f.call_hash) + " (" + dec(f.n_records) +
                        " records not decodable)",
                    error::unknown_call);
            return;
        }
        if (k->args.size() != f.n_records &&
            !problem(f, "n_records " + dec(f.n_records) + " != call arity " + dec(k->args.size())))
            return;
        std::size_t pos = 0;
        for (std::uint32_t i = 0; i < k->args.size(); ++i)
        {
            const dyn::call_arg& a = k->args[i];
            if (a.ak == fmt::arg_const)
            { // 1.5: no bytes in the frame; the record is the value in the call's entry, shown as the scalar or
                // enum it stands for
                record_info r;
                r.index = i;
                r.is_const = true;
                r.schema_hash = a.hash;
                std::size_t size = 0;
                if (kind_size(a.k) != 0)
                {
                    r.ak = fmt::arg_scalar;
                    r.k = a.k;
                    size = D().scalar_size(a.k);
                }
                else
                {
                    r.ak = fmt::arg_type;
                    r.type = D().find_type(a.hash);
                    if (!r.type)
                    {
                        problem(f,
                                "record " + dec(i) + ": constant of unknown type " + hex64(a.hash),
                                error::unknown_type);
                        return;
                    }
                    size = r.type->is_flat ? r.type->size : fmt::const_value_size + 1;
                }
                if (size > fmt::const_value_size ||
                    (size < fmt::const_value_size && (a.value >> (8 * size)) != 0))
                { // not a scalar or enum of at most 8 bytes, or value bytes above the type's size (parse_call
                    // checks that for scalars; an enum's size is only known here)
                    problem(f, "record " + dec(i) + ": constant does not fit its type",
                            error::corrupt_dictionary);
                    return;
                }
                r.payload = {reinterpret_cast<const std::byte*>(&a.value), size};
                if (!emit(f, r))
                    return;
                continue;
            }
            if (a.ak == fmt::arg_string)
            { // 1.1: u32 len, bytes — the length is read from the frame and bounds-checked here
                pos = fmt::align8(pos);
                if (body.size() - pos < fmt::string_len_prefix)
                {
                    problem(f, "record " + dec(i) + ": string length prefix at " + dec(pos) +
                                   " exceeds frame");
                    return;
                }
                std::uint32_t len;
                std::memcpy(&len, body.data() + pos, 4);
                if (body.size() - pos - fmt::string_len_prefix < len)
                {
                    problem(f, "record " + dec(i) + ": string of " + dec(len) + " bytes at " +
                                   dec(pos) + " exceeds frame");
                    return;
                }
                record_info r;
                r.index = i;
                r.ak = fmt::arg_string;
                r.schema_hash = a.hash;
                r.payload = body.subspan(pos + fmt::string_len_prefix, len);
                pos += fmt::string_len_prefix + len;
                if (!emit(f, r))
                    return;
                continue;
            }
            if (const dyn::type* const dt = a.ak == fmt::arg_type ? D().find_type(a.hash) : nullptr;
                dt && !dt->is_flat)
            { // v2: u32 len, u32 0, canonical image — the length is read from the frame and bounds-checked
                pos = fmt::align8(pos);
                if (body.size() - pos < fmt::image_prefix)
                {
                    problem(f, "record " + dec(i) + ": image prefix at " + dec(pos) +
                                   " exceeds frame");
                    return;
                }
                std::uint32_t len;
                std::memcpy(&len, body.data() + pos, 4);
                if (body.size() - pos - fmt::image_prefix < len)
                {
                    problem(f, "record " + dec(i) + ": image of " + dec(len) + " bytes at " +
                                   dec(pos) + " exceeds frame");
                    return;
                }
                record_info r;
                r.index = i;
                r.ak = fmt::arg_type;
                r.schema_hash = a.hash;
                r.type = dt;
                r.payload = body.subspan(pos + fmt::image_prefix, len);
                pos += fmt::image_prefix + len;
                if (!emit(f, r))
                    return;
                continue;
            }
            const auto sz = D().record_size(a);
            if (!sz)
            {
                problem(f,
                        "record " + dec(i) + ": " + hex64(a.hash) + ": " +
                            std::string(to_string(sz.error())),
                        sz.error());
                return;
            }
            pos = fmt::align8(pos);
            if (body.size() - pos < *sz)
            {
                problem(f, "record " + dec(i) + ": needs " + dec(*sz) + " bytes at " + dec(pos) +
                               ", frame has " + dec(body.size() - pos));
                return;
            }
            record_info r;
            r.index = i;
            r.payload = body.subspan(pos, *sz);
            r.is_literal = a.is_literal;
            r.ak = a.ak;
            r.k = a.k;
            if (a.ak == fmt::arg_scalar)
                r.schema_hash = a.hash; // 1.1
            else if (a.is_literal)
                r.literal_id = a.hash; // FR-L6: no bytes on the wire; the signature is the id
            else
            {
                r.schema_hash = a.hash;
                r.type = D().find_type(a.hash);
            }
            pos += *sz;
            if (!emit(f, r))
                return;
        }
        if (fmt::align8(pos) != body.size())
        {
            problem(f, "records end at " + dec(pos) + " but frame body is " + dec(body.size()) +
                           " bytes");
            return;
        }
        for (std::size_t i = pos; i < body.size(); ++i)
            if (body[i] != std::byte{0})
            {
                problem(f, "non-zero trailing padding");
                return;
            }
    }

    // Walks frames in [begin, end).
    void frames(const std::size_t begin, const std::size_t end)
    {
        std::size_t off = begin;
        while (off < end && !stopped)
        {
            frame_info f;
            f.index = next_index++; // among all frames, meta frames included (frame_ref::index)
            f.offset = off;
            if (end - off < fmt::frame_header_size)
            {
                problem(f, dec(end - off) + " stray bytes after last frame");
                return;
            }
            const frame_class fc = classify_frame(
                cap.subspan(off), std::min<std::uint64_t>(end - off, limits.frame_bytes), f);
            if (fc == frame_class::bad_len)
            {
                problem(f, "bad frame_len " + dec(f.frame_len) + " (capture has " + dec(end - off) +
                               " bytes left)");
                return; // cannot resynchronise
            }
            if (fc == frame_class::unknown_flags)
            { // G2-9: recorded and skipped, as the block reader does; never parsed or delivered
                if (!problem(f, "unknown frame flags " + dec(f.flags) + " (frame skipped)",
                             error::bad_version))
                    return;
                off += f.frame_len;
                continue;
            }
            const std::span<const std::byte> whole = cap.subspan(off, f.frame_len);
            if (f.has_crc && !check_crc(whole))
            { // 1.7: a frame whose trailer does not match is reported and skipped, never decoded
                f.crc_ok = false;
                if (!problem(f, "crc mismatch (frame skipped)", error::crc_mismatch))
                    return;
                off += f.frame_len; // not delivered either (the windowed reader does the same)
                continue;
            }
            const auto body =
                whole.subspan(fmt::frame_header_size, f.frame_len - fmt::frame_header_size -
                                                          (f.has_crc ? fmt::crc_trailer_size : 0));
            if (f.is_dict)
                dict_frame(f, dict_payload(body));
            if (!on_frame(D(), f))
            {
                stopped = true;
                return;
            }
            if (!f.is_dict && !f.is_checkpoint && !f.is_info)
            { // data frames are what `frames` and `bytes` count (2.2.1: the windowed reader always counted so)
                ++res.frames;
                res.bytes += f.frame_len;
                if (f.is_full)
                    full_frame(f, body);
                else
                    compact_frame(f, body);
            }
            off += f.frame_len;
        }
    }
};

} // namespace

result<file_header> parse_file_header(const std::span<const std::byte> h) noexcept
{
    if (h.size() < fmt::file_header_size)
        return std::unexpected(error::truncated);
    if (std::memcmp(h.data(), fmt::file_magic.data(), 4) != 0)
        return std::unexpected(error::bad_magic);
    fmt::cursor c(h.subspan(4, fmt::file_header_size - 4));
    file_header f;
    c.read(f.major);
    c.read(f.minor);
    c.read(f.flags);
    c.read(f.hash_algo);
    c.read(f.program_hash);
    c.read(f.dict_offset);
    c.read(f.dict_len);
    c.read(f.abi);
    // 1.6 and 1.7 (2.2.1): 1.6 files have no CHAIN and may be back-patched; 1.5 predates containers, 1.4 constant
    // arguments, 1.3 may carry pre-2.1.14 canonical names, earlier minors FNV-1a ids (hash_algo 1)
    if (f.major != fmt::format_major || f.minor < fmt::format_minor_oldest ||
        f.minor > fmt::format_minor)
        return std::unexpected(error::bad_version);
    if ((f.flags & fmt::file_flag_le) == 0)
        return std::unexpected(error::endianness_mismatch);
    if (f.hash_algo != fmt::hash_algo_xxh3_64)
        return std::unexpected(error::unsupported);
    return f;
}

result<walk_result> for_each_record(const std::span<const std::byte> capture,
                                    const dyn::dictionary* const extra, const bool stop_on_problem,
                                    const frame_fn& on_frame, const record_fn& on_record)
{
    walker w{capture, {}, {}, stop_on_problem, on_frame, on_record, false};
    std::size_t frames_begin = 0, frames_end = capture.size();
    // "ESSR" is also a legal frame_len (0x52535345, gk §2): when the header fields do not read as a v1 FILE and
    // the capture is large enough for such a frame, it is a bare frame stream. Smaller inputs with the magic are
    // FILEs (truncated or of another version), so they keep their precise diagnostic.
    bool is_file =
        capture.size() >= 4 && std::memcmp(capture.data(), fmt::file_magic.data(), 4) == 0;
    if (is_file && capture.size() >= fmt::file_header_size)
    {
        std::uint16_t major{};
        std::uint32_t hash_algo{}, magic_as_len{};
        std::memcpy(&major, capture.data() + 4, 2);
        std::memcpy(&hash_algo, capture.data() + 12, 4);
        std::memcpy(&magic_as_len, fmt::file_magic.data(), 4);
        if ((major != fmt::format_major || hash_algo != fmt::hash_algo_xxh3_64) &&
            capture.size() >= magic_as_len)
            is_file = false;
    }
    if (is_file)
    {
        const auto fh = parse_file_header(capture);
        if (!fh)
            return std::unexpected(fh.error());
        const std::uint32_t flags = fh->flags;
        const std::uint64_t dict_offset = fh->dict_offset, dict_len = fh->dict_len;
        w.dict.program_hash = fh->program_hash;
        w.res.program_hash = fh->program_hash;
        w.dict.abi = fh->abi;
        w.dict.abi_known = true;
        frames_begin = fmt::file_header_size;
        if ((flags & fmt::file_flag_dict) != 0 && dict_len != 0)
        {
            if (dict_offset > capture.size() || capture.size() - dict_offset < dict_len)
                return std::unexpected(error::truncated);
            if (const auto r = w.dict.add_rri(capture.subspan(dict_offset, dict_len)); !r)
                return std::unexpected(r.error());
            { // 2.2.6: the opening block is delivered as a frame_info (is_opening): dump --verbose lists it
                frame_info o;
                o.offset = static_cast<std::size_t>(dict_offset);
                o.frame_len = static_cast<std::uint32_t>(dict_len);
                o.is_dict = o.is_opening = true;
                if (!on_frame(w.dict, o))
                    w.stopped = true;
            }
            if (fh->minor >= 7)
            { // 1.7: the CHAIN follows the opening dictionary; deltas are inline frames, nothing is relocated
                frames_begin = fmt::align8(dict_offset + dict_len) + fmt::chain_size;
            }
            else if (dict_offset == fmt::file_header_size)
            {
                frames_begin = fmt::align8(dict_offset + dict_len);
            }
            else
            { // back-patched: skip the original dictionary area, stop before the relocated one
                const auto old_len = rri_length(capture.subspan(fmt::file_header_size));
                if (!old_len)
                    return std::unexpected(old_len.error());
                frames_begin = fmt::align8(fmt::file_header_size + *old_len);
                frames_end = dict_offset;
            }
        }
        if (frames_begin > frames_end)
            return std::unexpected(error::truncated);
    }
    if (extra)
        w.dict.merge(*extra);
    if (!w.stopped)
        w.frames(frames_begin, frames_end);
    dictionary_problems(w.dict, w.res.problems, w.res.first_error);
    w.res.dict = std::move(w.dict);
    return std::move(w.res);
}

std::string decode_problem(const frame_info& f, const record_info& r, const error e)
{
    return "frame " + dec(f.index) + " @" + dec(f.offset) + ": record " + dec(r.index) +
           ": decode error: " + std::string(to_string(e));
}

} // namespace es::serreg::internal

namespace es::serreg::tools
{
namespace
{

// G2-12: the warning text for a bool byte other than 0/1 (read as true by every reader)
std::string bool_warning(const std::byte raw)
{
    return "bool byte 0x" + internal::hex8(std::to_integer<std::uint8_t>(raw)) + " read as true";
}

// Visitor that only counts (cc F2 / gk 1.4: verify must exercise the same walker dump uses).
struct counting_visitor final : decoded_value_visitor
{
    std::uint64_t values{};
    std::vector<std::string>
        bools; // G2-12: "field: bool byte 0x02 read as true", flushed per record by verify
    void begin_struct(std::string_view, std::string_view) override
    {
    }
    void end_struct() override
    {
    }
    void begin_array(std::string_view, std::uint32_t) override
    {
    }
    void end_array() override
    {
    }
    void scalar(std::string_view, kind, std::span<const std::byte>) override
    {
        ++values;
    }
    void enumerator(std::string_view, std::string_view, std::string_view, std::int64_t) override
    {
        ++values;
    }
    void literal(std::string_view, std::uint64_t, std::string_view) override
    {
        ++values;
    }
    // G2-6: one event per string, not one virtual call per byte (the default string() emits an array of chars)
    void string(std::string_view, std::string_view) override
    {
        ++values;
    }
    void noncanonical_bool(const std::string_view field, const std::byte raw) override
    {
        bools.push_back((field.empty() ? std::string("value") : std::string(field)) + ": " +
                        bool_warning(raw));
    }
};

} // namespace

result<frame_walk> for_each_frame(const std::span<const std::byte> capture, const frame_callback& f,
                                  const dyn::dictionary* const extra)
{
    std::uint64_t delivered = 0;
    const internal::frame_fn on_frame = [&](const dyn::dictionary& d,
                                            const internal::frame_info& fi) {
        if (fi.is_dict || fi.is_checkpoint || fi.is_info)
            return true; // consumed by the walker: a dictionary is already merged into `d`; 1.7 meta frames
        ++delivered;
        return f(d, frame_ref{.index = fi.index,
                              .offset = fi.offset,
                              .bytes = capture.subspan(fi.offset, fi.frame_len),
                              .call_hash = fi.call_hash,
                              .n_records = fi.n_records,
                              .full_records = fi.is_full,
                              .call = d.find_call(fi.call_hash)});
    };
    const internal::record_fn on_record = [](const dyn::dictionary&, const internal::frame_info&,
                                             const internal::record_info&) { return true; };
    auto w = internal::for_each_record(capture, extra, false, on_frame, on_record);
    if (!w)
        return std::unexpected(w.error());
    return frame_walk{.dict = std::move(w->dict),
                      .frames = delivered,
                      .problems = std::move(w->problems),
                      .problems_omitted = w->problems_omitted};
}

// ---- 2.0.6: block reader and mapped files ---------------------------------------------------------------
namespace
{
struct fd_guard
{
    int fd = -1;
    ~fd_guard()
    {
        if (fd > STDIN_FILENO)
            ::close(fd);
    }
};
// Reads up to n bytes at the current position (pipes) or at `off` (files); returns the count, 0 at EOF.
result<std::size_t> read_some(const int fd, const bool seekable, std::byte* const p,
                              const std::size_t n, const std::uint64_t off)
{
    for (;;)
    {
        const ssize_t r = seekable ? ::pread(fd, p, n, static_cast<off_t>(off)) : ::read(fd, p, n);
        if (r >= 0)
            return static_cast<std::size_t>(r);
        if (errno != EINTR)
            return std::unexpected(error::io_error);
    }
}
// Fills exactly n bytes (or fewer at EOF); returns the count.
result<std::size_t> read_full(const int fd, const bool seekable, std::byte* const p,
                              const std::size_t n, const std::uint64_t off)
{
    std::size_t got = 0;
    while (got < n)
    {
        const auto r = read_some(fd, seekable, p + got, n - got, off + got);
        if (!r)
            return r;
        if (*r == 0)
            break;
        got += *r;
    }
    return got;
}
// Length of the RRI at `off` (header + entries), read entry header by entry header: the original dictionary area
// of a back-patched FILE, which the frames follow.
result<std::size_t> rri_length_at(const int fd, const std::uint64_t off)
{
    std::byte h[fmt::rri_header_size];
    if (const auto r = read_full(fd, true, h, sizeof h, off); !r || *r != sizeof h)
        return std::unexpected(r ? error::truncated : r.error());
    if (std::memcmp(h, fmt::rri_magic.data(), 4) != 0)
        return std::unexpected(error::bad_magic);
    std::uint32_t n_entries{};
    std::memcpy(&n_entries, h + 16, 4);
    std::uint64_t pos = off + fmt::rri_header_size;
    for (std::uint32_t i = 0; i < n_entries; ++i)
    {
        std::byte l[4];
        if (const auto r = read_full(fd, true, l, 4, pos); !r || *r != 4)
            return std::unexpected(error::truncated);
        std::uint32_t total{};
        std::memcpy(&total, l, 4);
        if (total < fmt::entry_header_size || total % 8 != 0) // G2-10, as rri_length
            return std::unexpected(error::corrupt_dictionary);
        pos += total;
    }
    return static_cast<std::size_t>(pos - off);
}
} // namespace

result<frame_walk> for_each_frame_in_file(const std::string_view path, const frame_callback& f,
                                          const block_options& o,
                                          const dyn::dictionary* const extra)
{
    fd_guard g;
    if (path == "-")
        g.fd = STDIN_FILENO;
    else
    {
        const std::string p(path);
        g.fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (g.fd < 0)
            return std::unexpected(errno == ENOENT ? error::not_found : error::io_error);
    }
    struct stat st = {};
    const bool seekable = ::fstat(g.fd, &st) == 0 && S_ISREG(st.st_mode);
    const std::uint64_t file_size =
        seekable ? static_cast<std::uint64_t>(st.st_size) : ~std::uint64_t{0};
    if (seekable)
        ::posix_fadvise(g.fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Nothing hostile may make us allocate more than the input can hold: for files, every length is checked
    // against the file size; everywhere, a dictionary above decode_limits::dictionary_bytes and a frame above
    // decode_limits::frame_bytes are refused (2.1.16, FR-D5; before: 1 GiB for pipes only).
    const decode_limits limits = o.limits ? *o.limits : get_decode_limits();
    const std::uint64_t max_dict = limits.dictionary_bytes;
    const std::uint64_t max_frame = limits.frame_bytes;
    frame_walk w;
    // buf[pos, have) holds unconsumed bytes; buf[pos] is at file offset buf_off. Consuming a frame only moves
    // pos; the tail is moved to the front only when a read needs the room (once per window, not per frame).
    const std::size_t window = o.block_bytes ? o.block_bytes : limits.window_bytes;
    std::vector<std::byte> buf(std::max<std::size_t>(window, 4096));
    std::size_t pos = 0, have = 0;
    std::uint64_t buf_off = 0;
    std::uint64_t end =
        file_size; // frames end here (a back-patched FILE: at the relocated dictionary)
    bool eof = false;
    const auto need =
        [&](const std::size_t n) -> result<bool> { // ensures n unconsumed bytes at buf[pos]
        while (have - pos < n && !eof)
        {
            if (buf.size() - pos < n || have == buf.size())
            { // make room: move the tail to the front, grow when one frame is larger than the buffer
                std::memmove(buf.data(), buf.data() + pos, have - pos);
                have -= pos;
                pos = 0;
                // 2.1.12 (review D-07): grow by doubling as bytes arrive, not to the claimed length at once
                // (a 16-byte header on a pipe can claim 1 GiB it never sends)
                if (buf.size() < n)
                    buf.resize(std::min(n, std::max(buf.size() * 2, have + 4096)));
            }
            const auto r = read_some(g.fd, seekable, buf.data() + have, buf.size() - have,
                                     buf_off + (have - pos));
            if (!r)
                return std::unexpected(r.error());
            if (*r == 0)
                eof = true;
            have += *r;
        }
        return have - pos >= n;
    };
    const auto at = [&](const std::size_t i) { return buf.data() + pos + i; };
    const auto consume = [&](const std::size_t n) {
        pos += n;
        buf_off += n;
    };

    // ---- FILE header and embedded dictionary
    if (const auto r = need(fmt::file_header_size); !r)
        return std::unexpected(r.error());
    std::uint64_t frames_begin = 0;
    if (have - pos >= 4 && std::memcmp(at(0), fmt::file_magic.data(), 4) == 0)
    {
        if (have - pos < fmt::file_header_size)
            return std::unexpected(error::truncated);
        const auto fh =
            internal::parse_file_header(std::span<const std::byte>(at(0), fmt::file_header_size));
        if (!fh)
            return std::unexpected(fh.error());
        const std::uint32_t flags = fh->flags;
        const std::uint64_t dict_offset = fh->dict_offset, dict_len = fh->dict_len;
        w.dict.program_hash = fh->program_hash;
        w.dict.abi = fh->abi;
        w.dict.abi_known = true;
        frames_begin = fmt::file_header_size;
        if ((flags & fmt::file_flag_dict) != 0 && dict_len != 0)
        {
            if (seekable && (dict_offset > file_size || dict_len > file_size - dict_offset))
                return std::unexpected(error::truncated);
            if (dict_len > max_dict)
                return std::unexpected(error::unsupported);
            if (dict_offset == fmt::file_header_size)
            { // the usual case: the dictionary follows the header — read it through the block buffer
                if (const auto r = need(fmt::file_header_size + dict_len); !r || !*r)
                    return std::unexpected(r ? error::truncated : r.error());
                if (const auto a =
                        w.dict.add_rri(std::span<const std::byte>(at(dict_offset), dict_len));
                    !a)
                    return std::unexpected(a.error());
                if (o.meta_frames && // 2.2.6: the opening block, once, before frame 0
                    !f(w.dict,
                       frame_ref{.offset = static_cast<std::size_t>(dict_offset),
                                 .bytes = std::span<const std::byte>(at(dict_offset), dict_len),
                                 .kind = frame_kind::opening}))
                    return w;
                frames_begin = fmt::align8(dict_offset + dict_len) +
                               (fh->minor >= 7 ? fmt::chain_size : 0); // 1.7: skip the CHAIN
            }
            else
            { // back-patched (1.6 files only): the complete dictionary is at the end; frames run from after the
                // original one to it
                if (!seekable)
                    return std::unexpected(error::unsupported); // a pipe cannot reach the end first
                if (dict_offset > file_size || file_size - dict_offset < dict_len)
                    return std::unexpected(error::truncated);
                std::vector<std::byte> d(dict_len);
                if (const auto r = read_full(g.fd, true, d.data(), d.size(), dict_offset);
                    !r || *r != d.size())
                    return std::unexpected(error::truncated);
                if (const auto a = w.dict.add_rri(d); !a)
                    return std::unexpected(a.error());
                const auto old_len = rri_length_at(g.fd, fmt::file_header_size);
                if (!old_len)
                    return std::unexpected(old_len.error());
                frames_begin = fmt::align8(fmt::file_header_size + *old_len);
                end = dict_offset;
            }
        }
    }
    if (extra)
        w.dict.merge(*extra);

    // ---- frames, block by block: consume every complete frame in the buffer, keep the partial tail
    if (frames_begin > end)
        return std::unexpected(error::truncated);
    if (seekable && frames_begin > buf_off + (have - pos))
    { // a back-patched FILE: jump over the original dictionary area instead of reading it
        pos = have = 0;
        buf_off = frames_begin;
    }
    else
    {
        if (const auto r = need(static_cast<std::size_t>(frames_begin - buf_off)); !r)
            return std::unexpected(r.error());
        consume(
            static_cast<std::size_t>(std::min<std::uint64_t>(frames_begin - buf_off, have - pos)));
    }
    std::uint64_t index = 0;
    const auto problem = [&](const std::uint64_t where, const std::string& what) {
        internal::add_problem(w.problems, w.problems_omitted, limits.problems,
                              "frame " + internal::dec(index) + " @" + internal::dec(where) + ": " +
                                  what);
    };
    // 2.2.6 (block_options::meta_frames): a dictionary / checkpoint / writer-info frame handed to the callback
    const auto deliver_meta = [&](const frame_kind k, const std::uint64_t off,
                                  const std::span<const std::byte> frame, const std::uint64_t idx) {
        return f(w.dict, frame_ref{.index = idx,
                                   .offset = static_cast<std::size_t>(off),
                                   .bytes = frame,
                                   .call_hash = 0,
                                   .n_records = 0,
                                   .full_records = false,
                                   .call = nullptr,
                                   .kind = k});
    };
    for (;;)
    {
        const std::uint64_t off = buf_off;
        if (off >= end)
            break;
        const auto h = need(fmt::frame_header_size);
        if (!h)
            return std::unexpected(h.error());
        if (!*h)
        {
            if (have != pos)
                problem(off, internal::dec(have - pos) + " stray bytes after last frame");
            break;
        }
        internal::frame_info fi;
        const internal::frame_class fc =
            internal::classify_frame(std::span<const std::byte>(at(0), fmt::frame_header_size),
                                     std::min<std::uint64_t>(end - off, max_frame), fi);
        const std::uint32_t frame_len = fi.frame_len;
        if (fc == internal::frame_class::bad_len)
        {
            if (!o.resync)
            {
                problem(off, "bad frame_len " + internal::dec(frame_len));
                break; // cannot resynchronise
            }
            // 1.7 (--resync): scan forward, 8 bytes at a time, for a checkpoint frame header (its flag and its
            // sentinel together are 10 bytes of signature); the skipped range is one problem line
            std::uint64_t skipped = 0;
            bool found = false;
            for (;;)
            {
                consume(8);
                skipped += 8;
                const auto hh = need(fmt::frame_header_size);
                if (!hh)
                    return std::unexpected(hh.error());
                if (!*hh || buf_off >= end)
                    break;
                internal::frame_info cand;
                if (internal::classify_frame(
                        std::span<const std::byte>(at(0), fmt::frame_header_size),
                        std::min<std::uint64_t>(end - buf_off, max_frame),
                        cand) == internal::frame_class::ok &&
                    cand.is_checkpoint && cand.call_hash == fmt::checkpoint_sentinel)
                {
                    found = true;
                    break;
                }
            }
            problem(off, "bad frame_len " + internal::dec(frame_len) + ": skipped " +
                             internal::dec(skipped) + " bytes to " +
                             (found ? "the next checkpoint" : "the end of the capture"));
            if (!found)
                break;
            ++index;
            continue;
        }
        const auto body = need(frame_len);
        if (!body)
            return std::unexpected(body.error());
        if (!*body)
        {
            problem(off, "truncated: frame_len " + internal::dec(frame_len) + ", " +
                             internal::dec(have - pos) + " bytes left");
            break;
        }
        const std::span<const std::byte> frame(at(0), frame_len);
        if (fc == internal::frame_class::unknown_flags)
            problem(off, "unknown frame flags " + internal::dec(fi.flags) + " (frame skipped)");
        else if (fi.has_crc && !internal::check_crc(frame))
            problem(off, "crc mismatch (frame skipped)"); // 1.7: never decoded
        else if (fi.is_dict)
        {
            dyn::dictionary delta;
            if (frame_len > max_dict)
                problem(off, "dictionary frame above decode_limits::dictionary_bytes (skipped)");
            else if (const auto a = delta.add_rri(frame.subspan(fmt::frame_header_size)); !a)
                problem(off, "dictionary frame: " + std::string(to_string(a.error())));
            else
                w.dict.merge(delta);
            if (o.meta_frames && !deliver_meta(frame_kind::dictionary, off, frame, index))
                break;
        }
        else if (fi.is_checkpoint || fi.is_info)
        {
            // 1.7: meta frames are consumed; counted in `index` like dictionary frames, not delivered — unless
            // 2.2.6's block_options::meta_frames asks for them
            if (o.meta_frames &&
                !deliver_meta(fi.is_checkpoint ? frame_kind::checkpoint : frame_kind::info, off,
                              frame, index))
                break;
        }
        else
        {
            ++w.frames;
            if (!f(w.dict, frame_ref{.index = index,
                                     .offset = static_cast<std::size_t>(off),
                                     .bytes = frame,
                                     .call_hash = fi.call_hash,
                                     .n_records = fi.n_records,
                                     .full_records = fi.is_full,
                                     .call = w.dict.find_call(fi.call_hash)}))
                break;
        }
        ++index;
        consume(frame_len);
    }
    // G2-9: the dictionary's own conflicts are problems here too, as in for_each_frame
    std::optional<error> first;
    internal::dictionary_problems(w.dict, w.problems, first);
    return w;
}

// ---- 1.7 (2.2.1): tools::info — header, dictionary chain, checkpoint chain, writer info, by seeks ------------
namespace
{
// Reads one whole frame at `off`: header first (bounded by the frame limit and the file), then the rest.
result<std::vector<std::byte>> read_frame_at(const int fd, const std::uint64_t off,
                                             const std::uint64_t file_size,
                                             const std::uint64_t max_frame,
                                             internal::frame_info& fi)
{
    if (off + fmt::frame_header_size > file_size)
        return std::unexpected(error::truncated);
    std::vector<std::byte> buf(fmt::frame_header_size);
    if (const auto r = read_full(fd, true, buf.data(), buf.size(), off); !r || *r != buf.size())
        return std::unexpected(error::truncated);
    fi = {};
    fi.offset = static_cast<std::size_t>(off);
    if (internal::classify_frame(buf, std::min(file_size - off, max_frame), fi) !=
        internal::frame_class::ok)
        return std::unexpected(error::length_mismatch);
    buf.resize(fi.frame_len);
    if (const auto r =
            read_full(fd, true, buf.data() + fmt::frame_header_size,
                      fi.frame_len - fmt::frame_header_size, off + fmt::frame_header_size);
        !r || *r != fi.frame_len - fmt::frame_header_size)
        return std::unexpected(error::truncated);
    if (fi.has_crc && !internal::check_crc(buf))
        return std::unexpected(error::crc_mismatch);
    return buf;
}
std::uint64_t u64_at(const std::span<const std::byte> b, const std::size_t at) noexcept
{
    std::uint64_t v{};
    std::memcpy(&v, b.data() + at, 8);
    return v;
}
} // namespace

std::string_view writer_info::get(const std::uint16_t key) const noexcept
{
    for (const auto& [k, v] : items)
        if (k == key)
            return v;
    return {};
}
std::string_view capture_info::writer_item(const std::uint16_t key) const noexcept
{
    for (const auto& [k, v] : writer)
        if (k == key)
            return v;
    return {};
}

result<capture_info> info(const std::string_view path)
{
    fd_guard g;
    const std::string p(path);
    g.fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (g.fd < 0)
        return std::unexpected(errno == ENOENT ? error::not_found : error::io_error);
    struct stat st = {};
    if (::fstat(g.fd, &st) != 0 || !S_ISREG(st.st_mode))
        return std::unexpected(error::unsupported); // a pipe: walk it instead
    const decode_limits limits = get_decode_limits();
    capture_info ci;
    ci.file_size = static_cast<std::uint64_t>(st.st_size);
    std::byte hdr[fmt::file_header_size];
    if (const auto r = read_full(g.fd, true, hdr, sizeof hdr, 0); !r || *r != sizeof hdr)
        return std::unexpected(error::truncated);
    const auto fh = internal::parse_file_header(hdr);
    if (!fh)
        return std::unexpected(fh.error());
    ci.minor = fh->minor;
    ci.flags = fh->flags;
    ci.program_hash = fh->program_hash;
    ci.abi = fh->abi;
    ci.dict.program_hash = fh->program_hash;
    ci.dict.abi = fh->abi;
    ci.dict.abi_known = true;
    if ((fh->flags & fmt::file_flag_dict) == 0 || fh->dict_len == 0)
        return ci; // nothing more a header-only file can say
    if (fh->dict_offset > ci.file_size || fh->dict_len > ci.file_size - fh->dict_offset)
        return std::unexpected(error::truncated);
    if (fh->dict_len > limits.dictionary_bytes)
        return std::unexpected(error::unsupported);
    {
        std::vector<std::byte> d(fh->dict_len);
        if (const auto r = read_full(g.fd, true, d.data(), d.size(), fh->dict_offset);
            !r || *r != d.size())
            return std::unexpected(error::truncated);
        if (const auto a = ci.dict.add_rri(d); !a)
            return std::unexpected(a.error());
    }
    ci.dictionary_offsets.push_back(fh->dict_offset);
    if (fh->minor < 7)
        return ci; // 1.6: no chains, no writer info
    // the CHAIN after the opening dictionary
    const std::uint64_t chain = fmt::align8(fh->dict_offset + fh->dict_len);
    std::byte cb[fmt::chain_size];
    if (chain + sizeof cb > ci.file_size)
        return std::unexpected(error::truncated);
    if (const auto r = read_full(g.fd, true, cb, sizeof cb, chain); !r || *r != sizeof cb)
        return std::unexpected(error::truncated);
    std::uint64_t next_dict = u64_at(cb, 0), next_ckpt = u64_at(cb, 8);
    // the writer-info frame, right after the CHAIN
    if (fh->flags & fmt::file_flag_info)
    {
        internal::frame_info fi;
        const auto f = read_frame_at(g.fd, chain + sizeof cb, ci.file_size, limits.frame_bytes, fi);
        if (!f)
            return std::unexpected(f.error());
        if (fi.is_info)
        {
            const std::span<const std::byte> b(*f);
            const std::size_t end = b.size() - (fi.has_crc ? fmt::crc_trailer_size : 0);
            std::size_t at = fmt::frame_header_size;
            std::uint16_t ver{}, n{};
            if (end - at >= 4)
            {
                std::memcpy(&ver, b.data() + at, 2);
                std::memcpy(&n, b.data() + at + 2, 2);
                at += 4;
            }
            for (std::uint16_t i = 0; i < n && end - at >= 4; ++i)
            {
                std::uint16_t key{}, len{};
                std::memcpy(&key, b.data() + at, 2);
                std::memcpy(&len, b.data() + at + 2, 2);
                at += 4;
                if (end - at < len)
                    break;
                ci.writer.emplace_back(
                    key, std::string(reinterpret_cast<const char*>(b.data() + at), len));
                at += static_cast<std::size_t>(len) + (len & 1u);
            }
        }
    }
    // 2.2.6: a link that cannot be followed is a warning (the chain ends there; `verify` reports the rest)
    const auto dangling = [&](const char* const which, const std::uint64_t from,
                              const std::uint64_t to, const std::uint64_t prev) {
        std::string w = std::string(which) + " chain: link @" + internal::dec(from) + " -> " +
                        internal::dec(to);
        if (to + fmt::frame_header_size > ci.file_size)
            w += " points past the end of the file (" + internal::dec(ci.file_size) + " bytes)";
        else if (to <= prev)
            w += " does not go forward";
        else
            w += " is not a " + std::string(which) + " frame";
        ci.warnings.push_back(std::move(w));
    };
    // the dictionary chain: offsets must increase (a loop in a corrupt file ends the chain)
    std::uint64_t last = chain;
    std::uint64_t link_at = chain; // where the link being followed was read
    if (next_dict != 0 && next_dict <= last)
        dangling("dictionary", link_at, next_dict, last);
    for (std::size_t n = 0; next_dict != 0 && next_dict > last && n < limits.dictionary_entries;
         ++n)
    {
        internal::frame_info fi;
        const auto f = read_frame_at(g.fd, next_dict, ci.file_size, limits.frame_bytes, fi);
        if (!f || !fi.is_dict)
        {
            dangling("dictionary", link_at, next_dict, last);
            break;
        }
        const std::span<const std::byte> b(*f);
        const std::size_t end = b.size() - (fi.has_crc ? fmt::crc_trailer_size : 0);
        if (end < fmt::frame_header_size + fmt::dict_frame_link_size)
            break;
        dyn::dictionary delta;
        if (const auto a = delta.add_rri(b.subspan(
                fmt::frame_header_size, end - fmt::frame_header_size - fmt::dict_frame_link_size));
            !a)
            break;
        ci.dict.merge(delta);
        ci.dictionary_offsets.push_back(next_dict);
        last = next_dict;
        link_at = next_dict + end - fmt::dict_frame_link_size;
        next_dict = u64_at(b, end - fmt::dict_frame_link_size);
        if (next_dict != 0 && next_dict <= last)
            dangling("dictionary", link_at, next_dict, last);
    }
    // the checkpoint chain
    last = chain;
    link_at = chain + 8;
    if (next_ckpt != 0 && next_ckpt <= last)
        dangling("checkpoint", link_at, next_ckpt, last);
    for (std::size_t n = 0; next_ckpt != 0 && next_ckpt > last && n < limits.dictionary_entries;
         ++n)
    {
        internal::frame_info fi;
        const auto f = read_frame_at(g.fd, next_ckpt, ci.file_size, limits.frame_bytes, fi);
        if (!f || !fi.is_checkpoint ||
            f->size() < fmt::frame_header_size + fmt::checkpoint_fixed_size)
        {
            dangling("checkpoint", link_at, next_ckpt, last);
            break;
        }
        const std::span<const std::byte> b(*f);
        const std::size_t at = fmt::frame_header_size;
        std::uint16_t ver{}, flags{};
        std::memcpy(&ver, b.data() + at, 2);
        std::memcpy(&flags, b.data() + at + 2, 2);
        checkpoint_info c;
        c.offset = next_ckpt;
        c.full_dictionary = (flags & fmt::checkpoint_flag_full_dictionary) != 0;
        c.prev = u64_at(b, at + 8);
        c.next = u64_at(b, at + 16);
        c.data_frames_before = u64_at(b, at + 24);
        c.bytes_before = u64_at(b, at + 32);
        c.wall_clock_ns = u64_at(b, at + 40);
        c.dictionary_offset = u64_at(b, at + 48);
        c.registry_version = u64_at(b, at + 56);
        ci.checkpoints.push_back(c);
        link_at = next_ckpt + at + 16;
        if (c.next != 0 && c.next <= next_ckpt)
            dangling("checkpoint", link_at, c.next, next_ckpt);
        last = next_ckpt;
        next_ckpt = c.next;
    }
    return ci;
}

std::string to_text(const capture_info& i)
{
    std::string out = "format 1." + internal::dec(i.minor) + "  program_hash " +
                      internal::hex64(i.program_hash) + "  size " + internal::dec(i.file_size) +
                      "  flags";
    if (i.flags & fmt::file_flag_crc_all)
        out += " crc";
    if (i.flags & fmt::file_flag_checkpoints)
        out += " checkpoints";
    if (i.flags & fmt::file_flag_info)
        out += " info";
    if (i.flags & fmt::file_flag_backpatched)
        out += " back-patched";
    out += "\n";
    out += "dictionary  " + internal::dec(i.dict.types.size()) + " types, " +
           internal::dec(i.dict.literals.size()) + " literals, " +
           internal::dec(i.dict.calls.size()) + " calls; " +
           internal::dec(i.dictionary_offsets.size()) + " block(s) at";
    for (const auto o : i.dictionary_offsets)
        out += " " + internal::dec(o);
    out += "\n";
    out += "checkpoints " + internal::dec(i.checkpoints.size()) + "\n";
    for (const auto& c : i.checkpoints)
        out += "  @" + internal::dec(c.offset) + "  frames " + internal::dec(c.data_frames_before) +
               "  bytes " + internal::dec(c.bytes_before) + "  dictionary @" +
               internal::dec(c.dictionary_offset) + (c.full_dictionary ? " (full copy)" : "") +
               "  wall " + internal::dec(c.wall_clock_ns) + "\n";
    static constexpr std::pair<std::uint16_t, const char*> names[] = {
        {fmt::info_library_version, "library"},
        {fmt::info_format, "format"},
        {fmt::info_program, "program"},
        {fmt::info_hostname, "host"},
        {fmt::info_pid, "pid"},
        {fmt::info_start_wall_ns, "start_wall_ns"},
        {fmt::info_scrub_padding, "scrub_padding"},
        {fmt::info_full_records, "full_records"},
        {fmt::info_canonical_unordered, "canonical_unordered"},
        {fmt::info_crc, "crc"},
        {fmt::info_checkpoint_bytes, "checkpoint_bytes"},
        {fmt::info_checkpoint_ns, "checkpoint_ns"},
        {fmt::info_dictionary, "dictionary"},
        {fmt::info_sync, "sync"},
        {fmt::info_clock, "clock"},
        {fmt::info_thread, "thread"},
        {fmt::info_abi, "abi"}};
    if (i.writer.empty())
        out += "writer      none\n";
    for (const auto& [k, v] : i.writer)
    {
        std::string name = "key" + internal::dec(k);
        for (const auto& [kk, nm] : names)
            if (kk == k)
                name = nm;
        out += "writer      " + name + " = " + v + "\n";
    }
    for (const auto& w : i.warnings) // 2.2.6
        out += "warning     " + w + "\n";
    return out;
}

std::string to_json(const capture_info& i)
{
    std::string out = "{\"format\":\"1." + internal::dec(i.minor) + "\",\"program_hash\":\"" +
                      internal::hex64(i.program_hash) +
                      "\",\"size\":" + internal::dec(i.file_size) +
                      ",\"crc\":" + ((i.flags & fmt::file_flag_crc_all) ? "true" : "false") +
                      ",\"dictionary_offsets\":[";
    for (std::size_t k = 0; k < i.dictionary_offsets.size(); ++k)
        out += (k ? "," : "") + internal::dec(i.dictionary_offsets[k]);
    out += "],\"types\":" + internal::dec(i.dict.types.size()) +
           ",\"literals\":" + internal::dec(i.dict.literals.size()) +
           ",\"calls\":" + internal::dec(i.dict.calls.size()) + ",\"checkpoints\":[";
    for (std::size_t k = 0; k < i.checkpoints.size(); ++k)
    {
        const auto& c = i.checkpoints[k];
        out += std::string(k ? "," : "") + "{\"offset\":" + internal::dec(c.offset) +
               ",\"frames\":" + internal::dec(c.data_frames_before) +
               ",\"bytes\":" + internal::dec(c.bytes_before) +
               ",\"dictionary\":" + internal::dec(c.dictionary_offset) +
               ",\"wall_ns\":" + internal::dec(c.wall_clock_ns) + "}";
    }
    out += "],\"writer\":{";
    for (std::size_t k = 0; k < i.writer.size(); ++k)
    {
        out += std::string(k ? "," : "") + "\"" + internal::dec(i.writer[k].first) + "\":";
        internal::append_quoted(out, i.writer[k].second);
    }
    out += "},\"warnings\":["; // 2.2.6
    for (std::size_t k = 0; k < i.warnings.size(); ++k)
    {
        out += k ? "," : "";
        internal::append_quoted(out, i.warnings[k]);
    }
    out += "]}\n";
    return out;
}

result<mapped_file> mapped_file::open(const std::string_view path, const bool populate)
{
    fd_guard g;
    const std::string p(path);
    g.fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (g.fd < 0)
        return std::unexpected(errno == ENOENT ? error::not_found : error::io_error);
    struct stat st = {};
    if (::fstat(g.fd, &st) != 0 || !S_ISREG(st.st_mode))
        return std::unexpected(error::unsupported); // pipes and devices: use for_each_frame_in_file
    mapped_file m;
    m.n_ = static_cast<std::size_t>(st.st_size);
    if (m.n_ == 0)
        return m; // mmap of 0 bytes is invalid; an empty span is the right answer
    void* const q =
        ::mmap(nullptr, m.n_, PROT_READ, MAP_PRIVATE | (populate ? MAP_POPULATE : 0), g.fd, 0);
    if (q == MAP_FAILED)
        return std::unexpected(error::io_error);
    ::madvise(q, m.n_, MADV_SEQUENTIAL);
    m.p_ = static_cast<const std::byte*>(q);
    return m;
}
result<frame_walk> for_each_frame(const std::string_view path, const frame_callback& f,
                                  const file_options& o, const dyn::dictionary* const extra)
{
    if (!o.blocks && path != "-")
    { // 2.1.16: mapping the whole file is the opt-in; windows are the default (A6)
        const auto m = mapped_file::open(path, o.populate);
        if (m)
            return for_each_frame(m->bytes(), f, extra);
        if (m.error() != error::unsupported)
            return std::unexpected(m.error());
        // not a regular file (a pipe, a device): read it in windows
    }
    return for_each_frame_in_file(path, f, block_options{o.block_bytes, o.limits}, extra);
}

} // namespace es::serreg::tools

// ---- 2.1.16 (A6): the record walk over a file, window by window --------------------------------------------
namespace es::serreg::internal
{
result<walk_result> for_each_record_in_file(const std::string_view path,
                                            const dyn::dictionary* const extra,
                                            const bool stop_on_problem, const frame_fn& on_frame,
                                            const record_fn& on_record,
                                            const tools::block_options& o)
{
    const frame_fn nop_frame = [](const dyn::dictionary&, const frame_info&) { return true; };
    walker wk{{}, {}, {}, stop_on_problem, nop_frame, on_record, false};
    if (o.limits)
        wk.limits = *o.limits;
    auto fw = tools::for_each_frame_in_file(
        path,
        [&](const dyn::dictionary& d, const tools::frame_ref& fr) {
            frame_info f;
            f.index = fr.index;
            f.offset = fr.offset;
            f.frame_len = static_cast<std::uint32_t>(fr.bytes.size());
            std::memcpy(&f.flags, fr.bytes.data() + 4, 2);
            f.n_records = fr.n_records;
            f.call_hash = fr.call_hash;
            f.is_full = fr.full_records;
            f.is_dict =
                fr.kind == tools::frame_kind::dictionary || fr.kind == tools::frame_kind::opening;
            f.is_opening = fr.kind == tools::frame_kind::opening;
            f.is_checkpoint = fr.kind == tools::frame_kind::checkpoint;
            f.is_info = fr.kind == tools::frame_kind::info;
            f.has_crc = (f.flags & fmt::frame_flag_crc) != 0; // 1.7: checked by the windowed reader
            wk.shared = &d;
            if (fr.kind != tools::frame_kind::data)
            { // 2.2.6: only with block_options::meta_frames; delivered, never counted or decoded
                if (f.is_opening)
                    f.flags = 0;
                return on_frame(d, f);
            }
            ++wk.res.frames;
            wk.res.bytes += f.frame_len;
            if (!on_frame(d, f))
                return false;
            const auto body = fr.bytes.subspan(fmt::frame_header_size,
                                               fr.bytes.size() - fmt::frame_header_size -
                                                   (f.has_crc ? fmt::crc_trailer_size : 0));
            if (f.is_full)
                wk.full_frame(f, body);
            else
                wk.compact_frame(f, body);
            return !wk.stopped;
        },
        o, extra);
    if (!fw)
        return std::unexpected(fw.error());
    walk_result res = std::move(wk.res);
    res.program_hash = fw->dict.program_hash;
    // moved, not copied: record_info::type pointers handed to callbacks (a print frame rendered after the walk)
    // point into this dictionary's nodes, which a move keeps where they are
    res.dict = std::move(fw->dict);
    // the windowed reader's own structural problems (bad lengths, truncation, stray bytes) first, then ours
    std::vector<std::string> problems = fw->problems;
    std::size_t omitted = fw->problems_omitted;
    for (auto& p : res.problems)
        add_problem(problems, omitted, wk.limits.problems, std::move(p));
    res.problems = std::move(problems);
    res.problems_omitted = omitted + res.problems_omitted;
    return res;
}
} // namespace es::serreg::internal

namespace es::serreg::tools
{

mapped_file::mapped_file(mapped_file&& o) noexcept
    : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0))
{
}
mapped_file& mapped_file::operator=(mapped_file&& o) noexcept
{
    if (this != &o)
    {
        if (p_)
            ::munmap(const_cast<std::byte*>(p_), n_);
        p_ = std::exchange(o.p_, nullptr);
        n_ = std::exchange(o.n_, 0);
    }
    return *this;
}
mapped_file::~mapped_file()
{
    if (p_)
        ::munmap(const_cast<std::byte*>(p_), n_);
}

namespace
{
// verify over either source (2.1.16, A6): the span walker or the windowed file walker.
result<verify_report> verify_source(const internal::record_source& src,
                                    const std::size_t problem_limit)
{
    std::vector<std::string> decode_problems, warnings;
    std::size_t decode_omitted = 0;
    counting_visitor v;
    const auto r = src(
        false, [](const dyn::dictionary&, const internal::frame_info&) { return true; },
        [&](const dyn::dictionary& d, const internal::frame_info& f,
            const internal::record_info& rec) {
            const std::string where = "frame " + internal::dec(f.index) + " @" +
                                      internal::dec(f.offset) + ": record " +
                                      internal::dec(rec.index) + ": ";
            if (rec.ak == fmt::arg_scalar && rec.k == kind::boolean && rec.payload.size() == 1 &&
                std::to_integer<unsigned>(rec.payload[0]) > 1)
                warnings.push_back(where + bool_warning(rec.payload[0]));
            if (rec.is_literal || !rec.type)
                return true; // unknown types are already structural problems
            if (const auto res = visit_record(rec.payload, *rec.type, d, v); !res)
                internal::add_problem(decode_problems, decode_omitted, problem_limit,
                                      internal::decode_problem(f, rec, res.error()));
            for (auto& b : v.bools)
                internal::add_problem(warnings, decode_omitted, problem_limit,
                                      where + std::move(b));
            v.bools.clear();
            return true;
        });
    if (!r)
        return std::unexpected(r.error());
    verify_report rep;
    rep.frames = r->frames;
    rep.records = r->records;
    rep.bytes = r->bytes;
    rep.program_hash = r->program_hash;
    rep.problems = r->problems;
    rep.problems_omitted = r->problems_omitted;
    for (auto& p : decode_problems)
        internal::add_problem(rep.problems, rep.problems_omitted, problem_limit, std::move(p));
    rep.problems_omitted += decode_omitted;
    rep.warnings = std::move(warnings);
    return rep;
}
} // namespace

result<verify_report> verify(const std::span<const std::byte> capture,
                             const dyn::dictionary* const extra)
{
    return verify_source(
        [&](const bool stop, const internal::frame_fn& on_frame,
            const internal::record_fn& on_record) {
            return internal::for_each_record(capture, extra, stop, on_frame, on_record);
        },
        get_decode_limits().problems);
}

result<verify_report> verify(const std::string_view path, const dyn::dictionary* const extra,
                             const block_options& o)
{
    const decode_limits limits = o.limits ? *o.limits : get_decode_limits();
    return verify_source(
        [&](const bool stop, const internal::frame_fn& on_frame,
            const internal::record_fn& on_record) {
            return internal::for_each_record_in_file(path, extra, stop, on_frame, on_record, o);
        },
        limits.problems);
}

} // namespace es::serreg::tools
