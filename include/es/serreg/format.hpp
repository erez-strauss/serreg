// es/serreg/format.hpp — the wire format, byte for byte (spec §6.5, §6.7). No reflection dependency.
//
// All integers are little-endian, fixed width, written/read with memcpy (no aliasing assumptions).
// "u16 name_len, name bytes" means a length prefix immediately followed by the bytes (no terminator).
//
// ---------------------------------------------------------------------------------------------------
// ENTRY  (one blob; the unit stored in the ELF section `es_serreg_dict`, in the RRI, and in dictionaries)
//   u32 total_len      whole entry including this header, multiple of 8 (zero padded)
//   u8  tag            1 = struct type, 2 = enum type, 3 = literal, 4 = call signature
//   u8  version        1
//   u16 reserved       0
//   body (by tag):
//   [1] struct:  u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 flags(bit0 = is_flat, bit1 = transparent — 1.6), u8 rsv,
//                u16 since, u16 deprecated, u16 name_len, u16 n_fields, name bytes, FIELD × n_fields
//   [2] enum:    u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 underlying_kind, u8 flags(bit0 = fixed underlying type — 1.6),
//                u16 since, u16 deprecated, u16 name_len, u16 n_enumerators, name bytes,
//                ENUMERATOR × n  where ENUMERATOR = i64 value, u16 name_len, name bytes
//   [3] literal: u64 id, u32 len, u32 rsv, bytes
//   [4] call:    u64 call_hash, u16 n_args, u16 flags(bit0 print, bit1 newline — format 1.1), u32 rsv, ARG × n
//                where ARG = u8 arg_kind(0 type, 1 literal, 2 scalar, 3 string — 2/3 since 1.1; 4 constant — 1.5),
//                u8 kind(scalar; a constant: its scalar kind or 0 for an enum), u8 rsv[6], u64 hash;
//                a constant's ARG is followed by u64 value (the object's bytes, zero-extended) — 24 bytes in all
//                (hash: schema hash | literal id | fingerprint64(scalar name) | fingerprint64("string"))
//   FIELD:       u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated,
//                u16 n_elem, u64 type_hash, u32 extent, name bytes, FIELD × n_elem   (element descriptors; recursive;
//                1.6: a map FIELD has n_elem 2 — key at offset 0, value at its canonical offset — and extent = the entry stride; a set has n_elem 1)
//
// RRI  (Raw Registry Image = header + ENTRY stream)
//   char[4] "ESRG", u16 major(1), u16 minor(7), u32 flags(bit0 = delta), u32 hash_algo(2 = XXH3-64),
//   u32 n_entries, u64 program_hash, u32 reserved, u64 abi_tag                     → 40 bytes, then ENTRY × n
//
// FILE
//   char[4] "ESSR", u16 major(1), u16 minor(7), u32 flags(bit0 LE, bit1 dict present, bit2 back-patched — 1.6 files
//   only, bit3 crc_all, bit4 checkpoints, bit5 writer info — 1.7), u32 hash_algo(2), u64 program_hash,
//   u64 dict_offset, u64 dict_len, u64 abi_tag                                       → 48 bytes
//   then the opening dictionary block at dict_offset: RRI (dict_len bytes), pad to 8, and since 1.7 the CHAIN
//   (u64 next_dictionary, u64 first_checkpoint — 0 until patched, each written once with an 8-byte pwrite when the
//   frame it points at exists), then FRAME × N. 1.6 files have no CHAIN; their dictionary may have been rewritten
//   at the end (bit2) — 1.7 writers never rewrite anything.
//
// FRAME (16-byte header)
//   u32 frame_len (whole frame incl. header, multiple of 8), u16 flags(bit0 = full records, bit1 = dictionary frame,
//   bit2 = CRC32C trailer, bit3 = checkpoint frame, bit4 = writer-info frame — 1.7),
//   u16 n_records, u64 call_hash
//   compact records: each record starts 8-aligned; flat type → raw bytes (size from dictionary); literal → nothing
//                    (1.1; u64 id in 1.0); scalar → raw bytes (1.1); string → u32 len, bytes (1.1);
//                    dynamic type → u32 len, u32 0, canonical image (1.2; docs/plans-for-v2.0.md §2)
//   full records:    u64 hash, u32 payload_len, u16 layout(0 = native), u16 rsv, payload, pad to 8
//                    (hash/payload: schema hash/object | literal id/u64 id | scalar name hash/raw bytes |
//                     "string" hash/u32 len + bytes — the string payload is the same in both modes)
//   dictionary frame: call_hash = dict_frame_sentinel, n_records = 0, payload = one RRI (may be a delta), pad to 8,
//                     and when the RRI's minor is ≥ 7: u64 next_dictionary (the forward chain; 0 = none yet)
//   CRC32C trailer (1.7, flag bit2): the last 8 bytes of the frame are u32 crc32c(frame[0, frame_len - 8)), u32 0;
//                     a frame whose CRC does not match is reported and skipped, never decoded
//   checkpoint frame (1.7, flag bit3): call_hash = checkpoint_sentinel, n_records = 0, payload =
//                     u16 version(1), u16 flags(bit0 = a full RRI follows), u32 rri_len, u64 prev_checkpoint,
//                     u64 next_checkpoint (patched), u64 data_frames_before, u64 bytes_before, u64 wall_clock_ns,
//                     u64 latest_dictionary_offset, u64 registry_version, [RRI]
//   writer-info frame (1.7, flag bit4): call_hash = info_sentinel, n_records = 0, payload = u16 version(1),
//                     u16 n_items, then ITEM × n where ITEM = u16 key (info_key), u16 len, bytes, pad to 2;
//                     unknown keys are skipped by readers
// ---------------------------------------------------------------------------------------------------
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace es::serreg::fmt
{

inline constexpr std::array<char, 4> file_magic{'E', 'S', 'S', 'R'};
inline constexpr std::array<char, 4> rri_magic{'E', 'S', 'R', 'G'};
inline constexpr std::uint16_t format_major = 1;
// Current: format 1.7; readers accept 1.6 and 1.7 (format_minor_oldest), and refuse anything older as bad_version.
// History, newest first (each minor moved because an older reader would have misread the new bytes; the
// "readers accept exactly 1.x" rule of each release is superseded by the line above):
//   1.6 (library 2.2.0): containers — FIELD kinds map_ (sorted (key, value) entries; the key and value FIELDs follow
//       as the two element descriptors) and set_ (sorted keys; one element descriptor), tail blocks laid out like a
//       vector of two-member entries; struct ENTRY flag `transparent` for the synthetic type that stands for a
//       container passed as an argument (one unnamed FIELD at offset 0); enum ENTRY flag `fixed`.
//   1.5 (library 2.1.15): a call argument can be a compile-time constant (arg_const): its ARG in the call ENTRY is
//       followed by 8 bytes holding the value, and it occupies no bytes in a compact frame.
//   1.4 (library 2.1.14, review C-03/C-04/C-05): the canonical-name rules — a class nested in a template
//       specialisation is named through its parent's arguments, std types use a frozen vocabulary (no std::__cxx11 /
//       std::__1, no allocator), a type without a linkage-unique name (unnamed namespace, local class) carries an
//       explicit name. Record bytes unchanged; the schema hashes of the affected types moved.
//   1.3 (library 2.1.0): XXH3-64 fingerprints (hash_algo 2) and the ABI tag in the FILE and RRI headers; the RRI
//       header grew to 40 bytes. 1.2 added dynamic types (canonical images), 1.1 scalar/string records and call
//       flags; everything before 1.3 used FNV-1a-64 ids (hash_algo 1), which this library no longer computes.
// 1.7 (library 2.2.1, decisions P-… of DECISIONS.md): integrity and crash safety for captures of any size, all
// of it optional per sink and zero bytes per frame when off — a CRC32C trailer (frame flag bit 2), checkpoint
// frames (bit 3; a forward-linked chain for seeking, each one naming the latest dictionary), a writer-info frame
// (bit 4; the options and versions the file was written with), and the dictionary chain: dictionary deltas are
// written inline (as streams always did) and linked forward by an 8-byte pointer patched once, so nothing is
// rewritten at close and a file is self-describing up to its last complete frame. The `log` call flag (bit 2)
// marks a logger's call signature. Readers accept 1.6 and 1.7 (the first release to accept two minors: 1.6 files
// are read as before, 1.7 features are recognised by their flags). Writers stamp 1.7.
inline constexpr std::uint16_t format_minor = 7;
inline constexpr std::uint16_t format_minor_oldest = 6; // the oldest minor readers accept
inline constexpr std::uint32_t hash_algo_xxh3_64 = 2;
inline constexpr std::uint64_t dict_frame_sentinel = 0x4553535244494354ull; // "ESSRDICT"
inline constexpr std::uint64_t checkpoint_sentinel = 0x45535352434b5054ull; // "ESSRCKPT"
inline constexpr std::uint64_t info_sentinel = 0x45535352494e464full;       // "ESSRINFO"

inline constexpr std::size_t file_header_size = 48;
// Deepest nesting of struct levels a reader follows: the typed decoder (detail::max_image_depth) and the
// dictionary walker (src/decoder.cpp) use the same number since 2.1.12 (review D-10).
inline constexpr int max_nesting = 64;
inline constexpr std::size_t rri_header_size = 40;
inline constexpr std::size_t frame_header_size = 16;
inline constexpr std::size_t entry_header_size = 8;
// literal ENTRY body: u64 id @+8, u32 len @+16, u32 rsv @+20, text @+24 (used by lit<S>::text() and the
// registry, which take the text straight from the blob instead of keeping a second copy).
inline constexpr std::size_t literal_len_offset = entry_header_size + 8;
inline constexpr std::size_t literal_text_offset = entry_header_size + 16;
inline constexpr std::size_t full_record_header_size = 16;

enum entry_tag : std::uint8_t
{
    tag_struct = 1,
    tag_enum = 2,
    tag_literal = 3,
    tag_call = 4
};
enum arg_kind : std::uint8_t
{
    arg_type = 0,
    arg_literal = 1,
    arg_scalar = 2, // 1.1: a top-level scalar argument (record = its bytes)
    arg_string = 3, // 1.1: a runtime string argument (record = u32 len, bytes)
    // 1.5: a compile-time constant (42_, k<Side::buy>). ARG: kind = the scalar kind (0 for an enum), hash = the
    // scalar name's hash or the enum's schema hash, then u64 value (the object's bytes, zero-extended). No bytes
    // in a compact frame; in a full-records frame the scalar or enum record it would be at run time.
    arg_const = 4
};
inline constexpr std::size_t const_value_size = 8; // the u64 after a constant's ARG
// 1.1 call flags: print (arg 0 is the format literal) / println (the reader appends '\n')
inline constexpr std::uint16_t call_flag_print = 1u << 0;
inline constexpr std::uint16_t call_flag_newline = 1u << 1;
inline constexpr std::size_t string_len_prefix = 4; // u32 len before the bytes of a string record
inline constexpr std::size_t image_prefix = 8; // 1.2: u32 len, u32 0 before a dynamic type's image

inline constexpr std::uint32_t file_flag_le = 1u << 0;
inline constexpr std::uint32_t file_flag_dict = 1u << 1;
inline constexpr std::uint32_t file_flag_backpatched =
    1u << 2; // 1.6 files only (never written since 1.7)
inline constexpr std::uint32_t file_flag_crc_all =
    1u << 3; // 1.7: every data frame carries a CRC32C trailer
inline constexpr std::uint32_t file_flag_checkpoints =
    1u << 4; // 1.7: the writer emits checkpoint frames
inline constexpr std::uint32_t file_flag_info =
    1u << 5; // 1.7: a writer-info frame follows the dictionary
inline constexpr std::uint16_t frame_flag_full = 1u << 0;
inline constexpr std::uint16_t frame_flag_dict = 1u << 1;
inline constexpr std::uint16_t frame_flag_crc = 1u << 2;        // 1.7: 8-byte CRC32C trailer
inline constexpr std::uint16_t frame_flag_checkpoint = 1u << 3; // 1.7
inline constexpr std::uint16_t frame_flag_info = 1u << 4;       // 1.7
inline constexpr std::size_t crc_trailer_size = 8;              // u32 crc32c, u32 0
// 1.7: the CHAIN after the opening dictionary of a FILE and the trailer of a dictionary frame
inline constexpr std::size_t chain_size = 16; // u64 next_dictionary, u64 first_checkpoint
inline constexpr std::size_t dict_frame_link_size =
    8; // u64 next_dictionary at the end of a dictionary frame
// 1.7 checkpoint payload (before an optional RRI)
inline constexpr std::size_t checkpoint_fixed_size = 2 + 2 + 4 + 8 * 7;
inline constexpr std::uint16_t checkpoint_flag_full_dictionary = 1u << 0;
// 1.7 writer-info keys (ITEM = u16 key, u16 len, bytes); values are UTF-8 text unless noted
enum info_key : std::uint16_t
{
    info_library_version = 1, // "2.2.1"
    info_format = 2,          // "1.7"
    info_program = 3,         // argv[0] or the name the writer gave
    info_hostname = 4,
    info_pid = 5,           // decimal text
    info_start_wall_ns = 6, // decimal text, system_clock since the epoch
    info_scrub_padding = 7, // "0" / "1"
    info_full_records = 8,
    info_canonical_unordered = 9,
    info_crc = 10, // "0" / "1" — crc_all
    info_checkpoint_bytes = 11,
    info_checkpoint_ns = 12,
    info_dictionary = 13, // "full" | "scoped:<scope name>"
    info_sync = 14,       // "none" | "dictionary" | "checkpoint" | "frame"
    info_clock = 15,      // a logger's clock: "system" | "steady" | "tsc" | "user"
    info_thread = 16,     // a logger's thread policy, or the writer thread's id for `once`
    info_abi = 17         // the long form of the ABI tag
};
inline constexpr std::uint32_t rri_flag_delta = 1u << 0;
inline constexpr std::uint8_t struct_flag_flat = 1u << 0;
// 1.6: the ENTRY describes a container argument (std::vector<i32>, std::map<…>, …): exactly one FIELD, unnamed, at
// offset 0, of a dynamic kind; renderers show the value, not a struct around it.
inline constexpr std::uint8_t struct_flag_transparent = 1u << 1;
// 1.6: enum ENTRY flags (the byte after the underlying kind): the enum has a fixed underlying type, so every value
// of that type is valid (decision D's range check applies to enums without the flag).
inline constexpr std::uint8_t enum_flag_fixed = 1u << 0;
// 1.7: a logger's call (level, file and line are constants in the signature; the timestamp is the last argument)
inline constexpr std::uint16_t call_flag_log = 1u << 2;

constexpr std::size_t align8(const std::size_t n) noexcept
{
    return (n + 7) & ~std::size_t{7};
}

static_assert(std::endian::native == std::endian::little,
              "es::serreg v1 targets little-endian hosts only (FR-W4)");

// ABI tag (FR-W4, format 1.3): the facts of the writer's C++ ABI that the dictionary does not already carry.
// Member sizes and offsets are in every FIELD, and `long` is spelled by width in canonical strings, so a typed
// read across ABIs already fails with hash_mismatch. What the tag adds: `wchar` has no width in a call string,
// `char` signedness decides how a `char` is rendered, and a dictionary-driven reader (srdump, walkers) learns that
// the file came from another ABI. Packed little-endian into the u64 at FILE +40 and RRI +32.
struct abi_tag
{
    std::uint8_t version = 1;
    std::uint8_t endian = 0; // 0 little, 1 big
    std::uint8_t ptr_size = 0;
    std::uint8_t long_size = 0;  // LP64 8, LLP64 4
    std::uint8_t wchar_size = 0; // Linux 4, Windows 2
    std::uint8_t char_signed = 0;
    std::uint8_t max_align = 0;
    std::uint8_t long_double_size = 0; // not a supported kind; recorded for a future port
    friend constexpr bool operator==(const abi_tag&, const abi_tag&) = default;
};
static_assert(sizeof(abi_tag) == 8);
constexpr abi_tag host_abi() noexcept
{
    return {1,
            std::endian::native == std::endian::little ? std::uint8_t{0} : std::uint8_t{1},
            static_cast<std::uint8_t>(sizeof(void*)),
            static_cast<std::uint8_t>(sizeof(long)),
            static_cast<std::uint8_t>(sizeof(wchar_t)),
            static_cast<std::uint8_t>(static_cast<char>(-1) < 0 ? 1 : 0),
            static_cast<std::uint8_t>(alignof(std::max_align_t)),
            static_cast<std::uint8_t>(sizeof(long double))};
}
constexpr std::uint64_t pack(const abi_tag t) noexcept
{
    return std::bit_cast<std::uint64_t>(t); // little-endian host (asserted above): byte 0 = version
}
constexpr abi_tag unpack_abi(const std::uint64_t v) noexcept
{
    return std::bit_cast<abi_tag>(v);
}
inline constexpr std::uint64_t host_abi_tag = pack(host_abi());

// Bounded little-endian reader used by every decoder in the library (FR-D5: no UB on corrupt input).
struct cursor
{
    const std::byte* p;
    const std::byte* end;
    constexpr cursor(const std::span<const std::byte> s) noexcept
        : p(s.data()), end(s.data() + s.size())
    {
    }
    constexpr std::size_t remaining() const noexcept
    {
        return static_cast<std::size_t>(end - p);
    }
    constexpr bool can(const std::size_t n) const noexcept
    {
        return remaining() >= n;
    }
    template<class T> bool read(T& out) noexcept
    {
        if (!can(sizeof(T)))
            return false;
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T);
        return true;
    }
    constexpr bool skip(const std::size_t n) noexcept
    {
        if (!can(n))
            return false;
        p += n;
        return true;
    }
    bool bytes(const std::size_t n, std::span<const std::byte>& out) noexcept
    {
        if (!can(n))
            return false;
        out = {p, n};
        p += n;
        return true;
    }
};

// ---- reference slots of a canonical image (format 1.2+, docs/plans-for-v2.0.md §2) ----------------------------
// A dynamic member's fixed part is a slot {u32 off, u32 n}; its bytes (the "block") are a tail of the same image.
// 2.1.3 (review G2-6): these are the rules both decoders apply — the typed one (reflect.hpp decode_value) and the
// dictionary-driven one (src/decoder.cpp walk_value) — so `verify` and `deserialize` accept exactly the same images.
//   * n == 0  ⇔  off == 0      (writers put {0, 0} for an empty string/vector, nullopt and a valueless variant)
//   * n != 0  ⇒  off ≥ fixed   (tails follow the root's fixed part: a slot never points into it)
//              and [off, off + bytes) lies inside the image
//   * the blocks of all slots together are at most image-size bytes: writers allocate disjoint tails, so an
//     honest image never exceeds it, and slots that share one tail cannot make a small image decode into a huge
//     value (the running budget; exceeding it is length_mismatch)
struct slot
{
    std::uint32_t off{}, n{};
};
inline constexpr std::size_t slot_size = 8;
inline bool read_slot(const std::span<const std::byte> image, const std::size_t at,
                      slot& s) noexcept
{
    if (at > image.size() || image.size() - at < slot_size)
        return false;
    std::memcpy(&s.off, image.data() + at, 4);
    std::memcpy(&s.n, image.data() + at + 4, 4);
    return true;
}
// Element stride of a block, in 64 bits: a (corrupt) size near 2^32 with alignment > 1 must not wrap.
constexpr std::uint64_t slot_stride(const std::uint64_t size, const std::uint64_t align) noexcept
{
    const std::uint64_t a = align ? align : 1;
    return (size + a - 1) / a * a;
}
// The image-wide state of one decode: the image, its root's fixed size, and the bytes left in the budget.
struct image_budget
{
    std::span<const std::byte> image;
    std::size_t fixed{}; // the root value's fixed part; every non-empty block starts at or after it
    std::uint64_t left{}; // bytes the remaining blocks may still reference
    constexpr image_budget(const std::span<const std::byte> img,
                           const std::size_t fixed_size) noexcept
        : image(img), fixed(fixed_size), left(img.size())
    {
    }
    // Checks the slot rules for a block of `bytes` bytes and charges the budget. n == 0 needs off == 0 (and
    // charges nothing); the caller handles what an empty slot means for its kind.
    constexpr bool take(const slot s, const std::uint64_t bytes) noexcept
    {
        if (s.n == 0)
            return s.off == 0;
        if (s.off < fixed || s.off > image.size() || bytes > image.size() - s.off || bytes > left)
            return false;
        left -= bytes;
        return true;
    }
    // A block of s.n elements of `stride` bytes each. 2.1.12 (review D-01): with sizes taken from a dictionary,
    // s.n × stride can exceed 64 bits (n < 2^32, stride up to about 2^33); a product that wraps is refused here, in
    // the one place both decoders use, instead of being charged as a small block.
    constexpr bool take_n(const slot s, const std::uint64_t stride) noexcept
    {
        std::uint64_t bytes{};
        if (__builtin_mul_overflow(std::uint64_t{s.n}, stride, &bytes))
            return false;
        return take(s, bytes);
    }
};

// Unbounded little-endian writer over caller-provided memory (caller sizes it first).
struct writer
{
    std::byte* p;
    template<class T> void put(const T v) noexcept
    {
        std::memcpy(p, &v, sizeof(T));
        p += sizeof(T);
    }
    void put_bytes(const void* const src, const std::size_t n) noexcept
    {
        if (n)
            std::memcpy(p, src, n);
        p += n;
    }
    void zero(const std::size_t n) noexcept
    {
        if (n)
            std::memset(p, 0, n);
        p += n;
    }
};

} // namespace es::serreg::fmt

namespace es::serreg::detail
{

// constexpr byte builder (std::memcpy is not usable in constant evaluation; bit_cast is).
struct cbuf
{
    std::vector<std::byte> v;
    template<class T> constexpr void put(const T x)
    {
        const auto a = std::bit_cast<std::array<std::byte, sizeof(T)>>(x);
        v.insert(v.end(), a.begin(), a.end());
    }
    constexpr void str16(const std::string_view s)
    {
        put(static_cast<std::uint16_t>(s.size()));
        for (const char c : s)
            v.push_back(static_cast<std::byte>(c));
    }
    constexpr void pad8()
    {
        while (v.size() % 8)
            v.push_back(std::byte{0});
    }
    constexpr void begin_entry(const std::uint8_t tag)
    {
        put(std::uint32_t{0});
        put(tag);
        put(std::uint8_t{1});
        put(std::uint16_t{0});
    }
    constexpr void end_entry()
    {
        pad8();
        const auto len =
            std::bit_cast<std::array<std::byte, 4>>(static_cast<std::uint32_t>(v.size()));
        for (int i = 0; i < 4; ++i)
            v[static_cast<std::size_t>(i)] = len[static_cast<std::size_t>(i)];
    }
};

// Freezes a consteval byte vector into a std::array (two-pass: size first, then contents).
template<class P> consteval auto to_blob_array()
{
    std::array<std::byte, P::bytes().size()> out{};
    const auto v = P::bytes();
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = v[i];
    return out;
}

} // namespace es::serreg::detail
