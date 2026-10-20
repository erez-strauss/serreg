// es/serreg/dictionary.hpp — runtime (heap-owned) view of a dictionary: the decoded ENTRY stream (format.hpp)
// from a registry snapshot, an RRI image, an ELF section, or dictionary frames in a capture. Used by the
// dynamic decoder, srdump and deserialize-time validation (spec §6.4, §6.9). No reflection dependency.
#pragma once
#include <es/serreg/descriptor.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace es::serreg::dyn
{

struct field
{
    std::string name;
    kind k{};
    std::uint32_t offset{}, size{};
    std::uint16_t align{}, since{}, deprecated{};
    std::uint64_t type_hash{};
    std::uint32_t extent{};
    // array: 1 element descriptor; variant: `extent` alternatives; 1.6: map_: key then value (the value's
    // `offset` is its offset inside an entry, `extent` the entry stride), set_: the key (`extent` the stride)
    std::vector<field> elem;
    bool operator==(const field&) const = default;
};

struct type
{
    std::string name;
    std::uint64_t schema_hash{}, layout_hash{};
    std::uint32_t size{}, align{};
    bool is_enum{}, is_flat{};
    bool
        transparent{}; // 1.6: a container argument's type — one unnamed field at 0 (struct_flag_transparent)
    bool enum_fixed{}; // 1.6: an enum with a fixed underlying type (enum_flag_fixed)
    kind underlying{}; // enums
    std::uint16_t since{}, deprecated{};
    std::vector<field> fields;
    std::vector<std::pair<std::string, std::int64_t>> enumerators;
    bool operator==(const type&) const =
        default; // structural: name, hashes, size/align, fields, enumerators
};

struct literal
{
    std::uint64_t id{};
    std::string text;
};

struct call_arg
{
    bool is_literal{};     // ak == fmt::arg_literal
    std::uint64_t hash{};  // schema hash | literal id | scalar name hash | "string" hash
    fmt::arg_kind ak{};    // 1.1: type / literal / scalar / string
    kind k{};              // 1.1: scalar kind when ak == arg_scalar (1.5: or a scalar arg_const)
    std::uint64_t value{}; // 1.5: arg_const — the constant's bytes, zero-extended
    bool operator==(const call_arg&) const = default;
};
struct call
{
    std::uint64_t hash{};
    std::uint16_t flags{}; // 1.1: fmt::call_flag_print / call_flag_newline
    std::vector<call_arg> args;
    bool operator==(const call&) const = default;
    bool is_print() const noexcept
    {
        return flags & fmt::call_flag_print;
    }
    bool newline() const noexcept
    {
        return flags & fmt::call_flag_newline;
    }
    bool is_log()
        const noexcept // 2.2.1 (1.7): a logger's call — the envelope precedes the format literal
    {
        return flags & fmt::call_flag_log;
    }
};

// Two ENTRYs with the same schema hash but a different body (FR-R4 seen from the tooling side). `kept` is the
// definition that stays in `types` (first seen); `dropped` is the later one. Since 2.1.12 (review D-03) both are
// summaries: name (at most 256 bytes), hashes, size and alignment; fields and enumerators are left out, so a few
// bytes of hostile input cannot make the dictionary copy a huge type once per redefinition.
struct type_conflict
{
    type kept, dropped;
};
// 2.1.3 (review G2-10): the same for a literal id or a call hash — two ENTRYs, one id, different bodies. `kept` and
// `dropped` describe the bodies (the literal's text, or the call's flags and argument hashes).
struct id_conflict
{
    bool is_call{};
    std::uint64_t id{};
    std::string kept, dropped;
};

struct dictionary
{
    std::map<std::uint64_t, type> types;
    std::map<std::uint64_t, literal> literals;
    std::map<std::uint64_t, call> calls;
    std::uint64_t program_hash{}; // as recorded in the source RRI/file header (0 if unknown)
    // ABI of the writer (format 1.3, FR-W4): the tag of the first FILE/RRI header this dictionary was read from,
    // or this process's ABI for a dictionary built in-process (registry snapshot, ELF section). `abi_mixed` is
    // set when a later source carried a different tag. Sizes and offsets are in every FIELD, so dictionary-driven
    // decoding still works across ABIs; typed decoding (deserialize, walk_values' typed half) does not, and
    // callers check `same_abi()` first.
    std::uint64_t abi{fmt::host_abi_tag};
    bool abi_known = false;
    bool abi_mixed = false;
    bool same_abi() const noexcept
    {
        return abi == fmt::host_abi_tag && !abi_mixed;
    }
    // Size of a top-level scalar argument of kind `k` as the writer laid it out: `wchar` has no width in its kind
    // name, so it comes from the writer's ABI tag (2 on Windows, 4 on Linux); every other kind is fixed-width.
    std::uint32_t scalar_size(const kind k) const noexcept
    {
        return k == kind::wchar ? fmt::unpack_abi(abi).wchar_size : kind_size(k);
    }
    std::vector<type_conflict>
        conflicts; // filled by add_entries/merge; tools::verify reports each one
    std::vector<id_conflict> id_conflicts; // 2.1.3: literals and calls, likewise
    // 2.1.12 (review D-03): at most max_conflicts entries are kept in each list; the rest are only counted here.
    static constexpr std::size_t max_conflicts = 64;
    std::uint64_t conflicts_omitted{};

    // Parse a bare ENTRY stream (ELF section content, or RRI body). Entries are appended/merged; a second
    // definition of an already-known schema hash (literal id, call hash) is kept out of `types` (`literals`,
    // `calls`) and, when it differs, recorded in `conflicts` (`id_conflicts`). Every count on the wire is checked against the remaining bytes before anything is allocated,
    // and field geometry (offset + size within the type, extent × element size within the field) is validated
    // here so the decoder never sees an inconsistent descriptor (FR-D5).
    result<void> add_entries(std::span<const std::byte> entries);
    // Parse a full RRI (header + entries).
    result<void> add_rri(std::span<const std::byte> rri);
    void merge(const dictionary& other);

    const type* find_type(std::uint64_t schema_hash) const noexcept;
    const type* find_type_by_name(std::string_view name) const noexcept;
    const literal* find_literal(std::uint64_t id) const noexcept;
    const call* find_call(std::uint64_t call_hash) const noexcept;

    // FR-H3 recomputed from the type table (sorted schema hashes) — used when the source carried no header.
    std::uint64_t computed_program_hash() const;

    // Native size in bytes of one compact record of this argument (flat type size, 0 for a literal — FR-L6 — or a
    // constant, the scalar's size). A string record is dynamic: error::unsupported — the walker reads its u32 length.
    result<std::uint32_t> record_size(const call_arg& a) const noexcept;

    // Canonical text renderings (sorted, deterministic; FR-W5).
    std::string to_json() const;
    std::string to_toml() const;
};

// Serialize a dictionary back into an RRI byte image (used by --elf --rri-out and by export paths that
// start from parsed data rather than compile-time blobs).
std::vector<std::byte> to_rri(const dictionary& d, std::uint32_t flags = 0);

} // namespace es::serreg::dyn
