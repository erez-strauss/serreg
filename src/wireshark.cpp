// src/wireshark.cpp — tools::wireshark_dissector: a Wireshark Lua dissector generated from a dictionary (2.1.9,
// `srdump --wireshark`; docs/wireshark.md).
//
// The output is one Lua file in two parts. The first part is data generated here: one ProtoField per value a
// filter can name (`serreg.corpus_Order.qty`), the types with their fields, the enumerators, the call signatures,
// the literal texts and the facts of the source (format, program hash, ABI tag). The second part (`lua_runtime`
// below) is fixed per library version: a file reader for FILE captures, the frame and record walk, and a
// descriptor-driven value walker that applies the same slot rules as src/decoder.cpp (fmt::image_budget). No C++
// type is compiled into the dissector; it is a snapshot of one dictionary, keyed by the same XXH3-64 ids as the
// wire, so a frame it does not know is shown as raw bytes and never decoded with the wrong layout.
#include "internal.hpp"

#include <es/serreg/hash.hpp>
#include <es/serreg/tools.hpp>

#include <map>
#include <set>

namespace es::serreg::tools
{
namespace
{
using internal::dec;

std::string_view lua_runtime() noexcept; // the fixed part, at the end of this file

std::string hexkey(
    const std::uint64_t v) // 16 lower-case hex digits: the runtime's table keys (UInt64:tohex())
{
    return internal::hex64(v).substr(2);
}

// A Lua string literal: printable ASCII as is, everything else as a decimal escape (valid in Lua 5.2 to 5.4).
std::string lua_str(const std::string_view s)
{
    std::string out = "\"";
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (c >= 0x20 && c < 0x7f)
            out += ch;
        else
        { // always three digits: "\12" followed by a digit would read as a longer escape
            const std::string n = dec(static_cast<unsigned>(c));
            out += "\\" + std::string(3 - n.size(), '0') + n;
        }
    }
    return out + "\"";
}

// A display-filter name segment: [A-Za-z0-9_] kept, everything else '_' ("corpus::Order" → "corpus__Order" is
// avoided by collapsing runs), never empty and never starting with a digit.
std::string filter_segment(const std::string_view s)
{
    std::string out;
    bool under = false;
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0)
        {
            out += ch;
            under = false;
        }
        else if (!under && !out.empty())
        {
            out += '_';
            under = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front())) != 0)
        out.insert(0, "t_");
    return out;
}

bool is_signed_kind(const kind k)
{
    return k == kind::i8 || k == kind::i16 || k == kind::i32 || k == kind::i64;
}

// The ProtoField constructor for a scalar leaf.
std::string scalar_ctor(const kind k, const std::string& abbr, const std::string& label)
{
    const std::string a = lua_str(abbr) + ", " + lua_str(label);
    switch (k)
    {
    case kind::i8:
        return "ProtoField.int8(" + a + ", base.DEC)";
    case kind::i16:
        return "ProtoField.int16(" + a + ", base.DEC)";
    case kind::i32:
        return "ProtoField.int32(" + a + ", base.DEC)";
    case kind::i64:
        return "ProtoField.int64(" + a + ", base.DEC)";
    case kind::u8:
        return "ProtoField.uint8(" + a + ", base.DEC)";
    case kind::u16:
        return "ProtoField.uint16(" + a + ", base.DEC)";
    case kind::u32:
        return "ProtoField.uint32(" + a + ", base.DEC)";
    case kind::u64:
        return "ProtoField.uint64(" + a + ", base.DEC)";
    case kind::f32:
        return "ProtoField.float(" + a + ")";
    case kind::f64:
        return "ProtoField.double(" + a + ")";
    case kind::boolean:
        return "ProtoField.bool(" + a + ")";
    case kind::char_:
    case kind::char8:
    case kind::byte_:
        return "ProtoField.uint8(" + a + ", base.HEX)";
    case kind::char16:
        return "ProtoField.uint16(" + a + ", base.HEX)";
    case kind::char32:
    case kind::wchar:
        return "ProtoField.uint32(" + a + ", base.HEX)";
    default:
        return "ProtoField.bytes(" + a + ")";
    }
}

struct generator
{
    const dyn::dictionary& d;
    std::vector<std::string> fields;            // ProtoField constructors; Lua index = position + 1
    std::map<std::uint64_t, std::string> tsegs; // schema hash → filter segment of the type (unique)
    std::string out;

    std::size_t add_field(std::string ctor)
    {
        fields.push_back(std::move(ctor));
        return fields.size();
    }

    // The enumerator table of an enum type, referenced by the ProtoFields of that enum (≤ 4 bytes only:
    // Wireshark's Lua value strings are 32-bit).
    static std::string enum_table(const std::uint64_t h)
    {
        return "E_" + hexkey(h);
    }
    std::string enum_ctor(const dyn::type* const t, const std::string& abbr,
                          const std::string& label, const std::uint32_t size) const
    {
        const bool sgn = t && is_signed_kind(t->underlying);
        const std::string a = lua_str(abbr) + ", " + lua_str(label);
        const std::string vals = t ? ", " + enum_table(t->schema_hash) : std::string{};
        switch (size)
        {
        case 1:
            return std::string(sgn ? "ProtoField.int8(" : "ProtoField.uint8(") + a + ", base.DEC" +
                   vals + ")";
        case 2:
            return std::string(sgn ? "ProtoField.int16(" : "ProtoField.uint16(") + a +
                   ", base.DEC" + vals + ")";
        case 4:
            return std::string(sgn ? "ProtoField.int32(" : "ProtoField.uint32(") + a +
                   ", base.DEC" + vals + ")";
        default:
            return std::string(sgn ? "ProtoField.int64(" : "ProtoField.uint64(") + a +
                   ", base.DEC)";
        }
    }

    // The Lua table of one field (or element / alternative descriptor), assigning its ProtoFields.
    std::string field_table(const dyn::field& f, const std::string& abbr, const std::string& label,
                            const int depth, const bool assign = true)
    {
        std::string s = "{ n = " + lua_str(f.name) + ", k = " + dec(static_cast<unsigned>(f.k)) +
                        ", o = " + dec(f.offset) + ", s = " + dec(f.size) + ", a = " + dec(f.align);
        if (f.type_hash != 0)
            s += ", th = \"" + hexkey(f.type_hash) + "\"";
        if (f.extent != 0)
            s += ", x = " + dec(f.extent);
        if (depth > 32)
            return s + " }"; // a (corrupt) self-nesting descriptor: the runtime reports it
        const bool str = f.k == kind::array_ && f.elem.size() == 1 &&
                         (f.elem[0].k == kind::char_ || f.elem[0].k == kind::char8);
        if (!assign)
            ; // the elements of a char array: shown through the array's string field
        else if (is_scalar_kind(f.k))
            s += ", pf = " + dec(add_field(scalar_ctor(f.k, abbr, label)));
        else if (f.k == kind::enum_)
            s += ", pf = " +
                 dec(add_field(enum_ctor(d.find_type(f.type_hash), abbr, label, f.size)));
        else if (f.k == kind::string_)
            s += ", pf = " +
                 dec(add_field("ProtoField.string(" + lua_str(abbr) + ", " + lua_str(label) + ")"));
        else if (str)
            s += ", str = true, pf = " +
                 dec(add_field("ProtoField.string(" + lua_str(abbr) + ", " + lua_str(label) + ")"));
        if (!f.elem.empty())
        {
            s += ", e = {";
            for (std::size_t i = 0; i < f.elem.size(); ++i)
            {
                const bool alt = f.k == kind::variant_;
                const bool map = f.k == kind::map_; // 1.6: the key and the value are two fields
                const std::string sub_abbr = alt   ? abbr + ".alt" + dec(i)
                                             : map ? abbr + (i == 0 ? ".key" : ".value")
                                                   : abbr;
                const std::string sub_label = alt   ? label + "<" + dec(i) + ">"
                                              : map ? label + (i == 0 ? " key" : " value")
                                                    : label;
                s += (i ? ", " : " ") +
                     field_table(f.elem[i], sub_abbr, sub_label, depth + 1, assign && !str);
            }
            s += " }";
        }
        return s + " }";
    }

    // The signature as the serreg.call.name field shows it: ("new order", corpus::Order) or
    // println("px={:.2f} qty={}", f64, u32).
    std::string call_name(const dyn::call& c) const
    {
        std::string s = c.is_log()     ? "log("
                        : c.is_print() ? (c.newline() ? "println(" : "print(")
                                       : "(";
        for (std::size_t i = 0; i < c.args.size(); ++i)
        {
            const auto& a = c.args[i];
            if (i)
                s += ", ";
            if (a.ak == fmt::arg_literal)
            {
                const dyn::literal* const l = d.find_literal(a.hash);
                s += l ? "\"" + l->text + "\"" : "lit#" + hexkey(a.hash);
            }
            else if (a.ak == fmt::arg_const)
            { // 1.5: the value's bytes (a u64) after the type it stands for
                const dyn::type* const t = kind_size(a.k) != 0 ? nullptr : d.find_type(a.hash);
                s += "const " +
                     (kind_size(a.k) != 0 ? std::string(to_string(a.k))
                      : t                 ? t->name
                                          : "0x" + hexkey(a.hash)) +
                     "=0x" + hexkey(a.value);
            }
            else if (a.ak == fmt::arg_scalar)
                s += std::string(to_string(a.k));
            else if (a.ak == fmt::arg_string)
                s += "string";
            else if (const dyn::type* const t = d.find_type(a.hash))
                s += t->name;
            else
                s += "0x" + hexkey(a.hash);
        }
        return s + ")";
    }

    void assign_segments()
    {
        std::set<std::string> used;
        for (const auto& [h, t] : d.types)
        {
            std::string seg = filter_segment(t.name);
            if (!used.insert(seg).second)
            {
                seg += "_" + hexkey(h).substr(0, 8);
                used.insert(seg);
            }
            tsegs[h] = seg;
        }
    }

    std::string run(const wireshark_options& o)
    {
        assign_segments();
        std::string types, enums;
        for (const auto& [h, t] : d.types)
        {
            const std::string base = "serreg." + tsegs[h];
            if (t.is_enum)
            {
                enums += "local " + enum_table(h) + " = {";
                bool first = true;
                for (const auto& [name, v] : t.enumerators)
                {
                    constexpr std::int64_t exact = std::int64_t{1}
                                                   << 53; // Lua 5.2 numbers are doubles
                    if (v > exact || v < -exact)
                        continue;
                    enums +=
                        std::string(first ? " " : ", ") + "[" + dec(v) + "] = " + lua_str(name);
                    first = false;
                }
                enums += " }\n";
                const std::size_t pf = add_field(enum_ctor(&t, base, t.name, t.size));
                types += "  [\"" + hexkey(h) + "\"] = { name = " + lua_str(t.name) +
                         ", enum = true, size = " + dec(t.size) +
                         ", u = " + dec(static_cast<unsigned>(t.underlying)) + ", lh = \"" +
                         hexkey(t.layout_hash) + "\", values = " + enum_table(h) +
                         ", pf = " + dec(pf) + " },\n";
                continue;
            }
            types += "  [\"" + hexkey(h) + "\"] = { name = " + lua_str(t.name) +
                     ", size = " + dec(t.size) + ", align = " + dec(t.align) +
                     ", flat = " + (t.is_flat ? "true" : "false") +
                     (t.transparent ? ", transparent = true" : "") + ", lh = \"" +
                     hexkey(t.layout_hash) + "\", fields = {\n";
            // 2.1.12 (review T-13): members whose names map to one filter segment (y and y_, a__b and a_b)
            // get the member's index appended to the later ones, so each member has its own field
            std::set<std::string> fsegs;
            for (std::size_t i = 0; i < t.fields.size(); ++i)
            {
                const auto& f = t.fields[i];
                std::string seg = filter_segment(f.name);
                if (seg.empty())
                    seg = "value"; // 1.6: the unnamed field of a transparent (container) type
                if (!fsegs.insert(seg).second)
                {
                    seg += "_" + dec(i);
                    fsegs.insert(seg);
                }
                types += "    " +
                         field_table(f, base + "." + seg, f.name.empty() ? "value" : f.name, 0) +
                         ",\n";
            }
            types += "  } },\n";
        }
        std::string calls;
        for (const auto& [h, c] : d.calls)
        {
            calls += "  [\"" + hexkey(h) + "\"] = { name = " + lua_str(call_name(c)) +
                     ", flags = " + dec(c.flags) + ", args = {";
            for (std::size_t i = 0; i < c.args.size(); ++i)
            {
                const auto& a = c.args[i];
                calls += std::string(i ? ", " : " ") +
                         "{ ak = " + dec(static_cast<unsigned>(a.ak)) + ", h = \"" +
                         hexkey(a.hash) + "\"";
                if (a.ak == fmt::arg_scalar || (a.ak == fmt::arg_const && kind_size(a.k) != 0))
                    calls += ", k = " + dec(static_cast<unsigned>(a.k));
                if (a.ak == fmt::arg_const)
                { // 1.5: the 8 value bytes in wire order, for ByteArray.new()
                    std::string v;
                    for (unsigned b = 0; b < 8; ++b)
                    {
                        static constexpr char digits[] = "0123456789abcdef";
                        const unsigned byte = static_cast<unsigned>((a.value >> (8 * b)) & 0xff);
                        v += digits[byte >> 4];
                        v += digits[byte & 15];
                    }
                    calls += ", v = \"" + v + "\"";
                }
                calls += " }";
            }
            calls += " } },\n";
        }
        std::string lits;
        for (const auto& [id, l] : d.literals)
            lits += "  [\"" + hexkey(id) + "\"] = " + lua_str(l.text) + ",\n";
        std::string scalars;
        for (unsigned k = 1; k <= static_cast<unsigned>(kind::byte_); ++k)
        {
            const kind kk = static_cast<kind>(k);
            scalars += "  [\"" + hexkey(fingerprint64(to_string(kk))) + "\"] = " + dec(k) + ",\n";
        }

        const fmt::abi_tag abi = fmt::unpack_abi(d.abi);
        // 2.1.12 (review T-03): text in a Lua comment is limited to printable ASCII; a file name with a newline
        // would otherwise end the comment and become code.
        const auto lua_comment = [](const std::string_view t) {
            std::string c(t);
            for (char& ch : c)
                if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e)
                    ch = '?';
            return c;
        };
        out = "-- es::serreg Wireshark dissector (Lua), generated by " + lua_comment(o.generator) +
              " --wireshark. Do not edit: regenerate\n-- when the program's types change "
              "(docs/wireshark.md).\n-- Dictionary sources:";
        for (const auto& s : o.sources)
            out += "\n--   " + lua_comment(s);
        if (o.sources.empty())
            out += " (not recorded)";
        out += "\n-- " + dec(d.types.size()) + " types, " + dec(d.calls.size()) +
               " call signatures, " + dec(d.literals.size()) +
               " literals.\n"
               "-- Install: copy to ~/.local/lib/wireshark/plugins/ (Wireshark then opens .ser "
               "captures), "
               "or run\n--   tshark -X lua_script:<this file> -r capture.ser -V\n\n"
               "local SERREG = {\n  generator = " +
               lua_str(o.generator + " " + std::string(library_version)) +
               ",\n  format_major = " + dec(fmt::format_major) +
               ", format_minor = " + dec(fmt::format_minor) +
               ", hash_algo = " + dec(fmt::hash_algo_xxh3_64) + ",\n  program_hash = \"" +
               hexkey(d.program_hash ? d.program_hash : d.computed_program_hash()) +
               "\",\n  abi = \"" + hexkey(d.abi) +
               "\", wchar_size = " + dec(abi.wchar_size ? abi.wchar_size : 4u) +
               ",\n  string_hash = \"" + hexkey(fingerprint64("string")) + "\",\n}\n\n" +
               "local serreg = Proto(\"serreg\", \"es::serreg\")\n\n" + enums + "\nlocal PF = {\n";
        // the ProtoFields are complete only after the tables above were built
        std::string pfs;
        for (std::size_t i = 0; i < fields.size(); ++i)
            pfs += "  [" + dec(i + 1) + "] = " + fields[i] + ",\n";
        out += pfs + "}\n\nlocal TYPES = {\n" + types + "}\n\nlocal CALLS = {\n" + calls +
               "}\n\nlocal LITERALS = {\n" + lits +
               "}\n\n-- scalar argument name hash → kind (full records)\nlocal SCALARS = {\n" +
               scalars + "}\n";
        out += lua_runtime();
        return out;
    }
};

} // namespace

std::string wireshark_dissector(const dyn::dictionary& d, const wireshark_options& o)
{
    generator g{d, {}, {}, {}};
    return g.run(o);
}

namespace
{
// ---- the fixed part of every generated dissector -------------------------------------------------------------
// Lua 5.2 compatible (Wireshark ≤ 4.4) and 5.4 compatible (4.6+): numbers are only used below 2^53, 64-bit ids
// are compared as hex strings (UInt64:tohex()), no bit32 and no integer operators.
constexpr std::string_view lua_runtime_text = R"LUA(
-- ===== fixed runtime (es::serreg) ============================================================================
local K = { I8 = 1, I16 = 2, I32 = 3, I64 = 4, U8 = 5, U16 = 6, U32 = 7, U64 = 8, F32 = 9, F64 = 10, BOOL = 11,
            CHAR = 12, CHAR8 = 13, CHAR16 = 14, CHAR32 = 15, WCHAR = 16, BYTE = 17,
            ENUM = 32, ARRAY = 33, STRUCT = 34, STRING = 48, VECTOR = 49, OPTIONAL = 50, VARIANT = 51,
            MAP = 53, SET = 54 }
local KNAME = { "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool", "char", "char8",
                "char16", "char32", "wchar", "byte" }
local KSIZE = { 1, 2, 4, 8, 1, 2, 4, 8, 4, 8, 1, 1, 1, 2, 4, SERREG.wchar_size, 1 }
local AK_TYPE, AK_LITERAL, AK_SCALAR, AK_STRING, AK_CONST = 0, 1, 2, 3, 4
local MAX_DEPTH = 64

-- header, record and summary fields
local H = {
  file_magic = ProtoField.string("serreg.file.magic", "Magic"),
  file_version = ProtoField.string("serreg.file.version", "Format"),
  file_flags = ProtoField.uint32("serreg.file.flags", "Flags", base.HEX),
  file_program = ProtoField.uint64("serreg.file.program_hash", "Program hash", base.HEX),
  file_dict_offset = ProtoField.uint64("serreg.file.dict_offset", "Dictionary offset", base.DEC),
  file_dict_len = ProtoField.uint64("serreg.file.dict_len", "Dictionary length", base.DEC),
  file_abi = ProtoField.uint64("serreg.file.abi", "ABI tag", base.HEX),
  dict_entries = ProtoField.uint32("serreg.dict.entries", "Entries", base.DEC),
  dict_known = ProtoField.uint32("serreg.dict.known", "Known to this dissector", base.DEC),
  dict_unknown = ProtoField.uint32("serreg.dict.unknown", "Unknown to this dissector", base.DEC),
  dict_program = ProtoField.uint64("serreg.dict.program_hash", "Program hash", base.HEX),
  frame_len = ProtoField.uint32("serreg.frame.len", "Frame length", base.DEC),
  frame_flags = ProtoField.uint16("serreg.frame.flags", "Flags", base.HEX),
  frame_records = ProtoField.uint16("serreg.frame.records", "Records", base.DEC),
  frame_call = ProtoField.uint64("serreg.frame.call", "Call hash", base.HEX),
  frame_kind = ProtoField.string("serreg.frame.kind", "Kind"),
  call_name = ProtoField.string("serreg.call.name", "Signature"),
  record_hash = ProtoField.uint64("serreg.record.hash", "Record hash", base.HEX),
  record_len = ProtoField.uint32("serreg.record.len", "Payload length", base.DEC),
  type = ProtoField.string("serreg.type", "Type"),
  literal = ProtoField.string("serreg.literal", "Literal"),
  string = ProtoField.string("serreg.arg.string", "string"),
  message = ProtoField.string("serreg.message", "Message"),
}
local ARGF = {}
local INTF = { ProtoField.int8, ProtoField.int16, ProtoField.int32, ProtoField.int64,
               ProtoField.uint8, ProtoField.uint16, ProtoField.uint32, ProtoField.uint64 }
for k = 1, #KNAME do
  local n = "serreg.arg." .. KNAME[k]
  if k <= 8 then ARGF[k] = INTF[k](n, KNAME[k], base.DEC)
  elseif k == 9 then ARGF[k] = ProtoField.float(n, KNAME[k])
  elseif k == 10 then ARGF[k] = ProtoField.double(n, KNAME[k])
  elseif k == 11 then ARGF[k] = ProtoField.bool(n, KNAME[k])
  elseif KSIZE[k] == 1 then ARGF[k] = ProtoField.uint8(n, KNAME[k], base.HEX)
  elseif KSIZE[k] == 2 then ARGF[k] = ProtoField.uint16(n, KNAME[k], base.HEX)
  else ARGF[k] = ProtoField.uint32(n, KNAME[k], base.HEX) end
end
local all = {}
for _, f in pairs(H) do all[#all + 1] = f end
for _, f in ipairs(ARGF) do all[#all + 1] = f end
for _, f in ipairs(PF) do all[#all + 1] = f end
serreg.fields = all

local X = {
  unknown_call = ProtoExpert.new("serreg.unknown_call", "Call hash unknown to this dissector (regenerate it with srdump --wireshark)", expert.group.UNDECODED, expert.severity.WARN),
  unknown_type = ProtoExpert.new("serreg.unknown_type", "Type unknown to this dissector (regenerate it with srdump --wireshark)", expert.group.UNDECODED, expert.severity.WARN),
  unknown_entries = ProtoExpert.new("serreg.dict.unknown_entries", "Dictionary entries unknown to this dissector", expert.group.UNDECODED, expert.severity.WARN),
  layout = ProtoExpert.new("serreg.dict.layout_mismatch", "Same schema hash, different layout than this dissector", expert.group.PROTOCOL, expert.severity.ERROR),
  program = ProtoExpert.new("serreg.dict.program_differs", "Program hash differs from the dictionary this dissector was generated from", expert.group.COMMENTS_GROUP, expert.severity.NOTE),
  abi = ProtoExpert.new("serreg.abi_differs", "Written on another ABI than the dictionary of this dissector", expert.group.PROTOCOL, expert.severity.WARN),
  malformed = ProtoExpert.new("serreg.malformed", "Malformed", expert.group.MALFORMED, expert.severity.ERROR),
  bool = ProtoExpert.new("serreg.noncanonical_bool", "bool byte other than 0/1 (read as true)", expert.group.PROTOCOL, expert.severity.WARN),
  records = ProtoExpert.new("serreg.record_count", "Record count differs from the call signature", expert.group.MALFORMED, expert.severity.ERROR),
  crc = ProtoExpert.new("serreg.crc_mismatch", "CRC32C trailer does not match the frame (not decoded)", expert.group.CHECKSUM, expert.severity.ERROR),
}
serreg.experts = { X.unknown_call, X.unknown_type, X.unknown_entries, X.layout, X.program, X.abi, X.malformed, X.bool, X.records, X.crc }

serreg.prefs.tcp_port = Pref.uint("TCP port", 0, "Decode this TCP port as a stream of es::serreg frames (0: off)")
serreg.prefs.udp_port = Pref.uint("UDP port", 0, "Decode this UDP port as es::serreg frames, one or more per datagram (0: off)")

-- ---- helpers -------------------------------------------------------------------------------------------------
local function align8(n) return n + (8 - n % 8) % 8 end
local function bit(v, b) return math.floor(v / b) % 2 == 1 end
-- 1.7: CRC32C (reflected, polynomial 0x82F63B78) over frame[0, len - 8), checked against the trailer. Lua 5.2 has
-- bit32, Lua 5.4 has operators; the 5.4 spelling is compiled at run time so this file loads under both.
local bxor, band, rshift
if bit32 then
  bxor, band, rshift = bit32.bxor, bit32.band, bit32.rshift
else
  bxor, band, rshift = load("return function(a, b) return a ~ b end, function(a, b) return a & b end, function(a, n) return a >> n end")()
end
local CRC_T = {}
for i = 0, 255 do
  local c = i
  for _ = 1, 8 do
    if band(c, 1) == 1 then c = bxor(rshift(c, 1), 0x82F63B78) else c = rshift(c, 1) end
  end
  CRC_T[i] = c
end
local function crc32c(tvb, off, len)
  local c = 0xFFFFFFFF
  for i = 0, len - 1 do
    c = bxor(CRC_T[band(bxor(c, tvb(off + i, 1):le_uint()), 0xFF)], rshift(c, 8))
  end
  return band(bxor(c, 0xFFFFFFFF), 0xFFFFFFFF)
end
local function hex(tvb, off) return string.lower(tvb(off, 8):le_uint64():tohex()) end
local function u32(tvb, off) return tvb(off, 4):le_uint() end
local function u16(tvb, off) return tvb(off, 2):le_uint() end
local function fail(msg) error({ serreg = msg }, 0) end
local function need(tvb, off, n) if off < 0 or n < 0 or off + n > tvb:len() then fail("truncated") end end
local function stride(e) local a = e.a > 0 and e.a or 1; return math.ceil(e.s / a) * a end
-- Lua 5.4 (Wireshark 4.6 and later) keeps integers and floats apart: UInt64/Int64:tonumber() returns a float, and
-- tostring(7.0) is "7.0". A whole number that becomes text goes through int() first (2.1.11). No-op on Lua 5.2.
local function int(x) if math.tointeger then return math.tointeger(x) or x end return x end
-- The value of an enum (1, 2, 4 or 8 bytes), sign-extended when the underlying kind is signed.
local function enum_value(r, t)
  local signed = t.u >= K.I8 and t.u <= K.I64
  if r:len() == 8 then return int(signed and r:le_int64():tonumber() or r:le_uint64():tonumber()) end
  return signed and r:le_int() or r:le_uint()
end
-- The exact decimal text of an enum's value: an 8-byte value through Int64/UInt64, whose digits are exact above
-- 2^53, where a Lua number is not (2.1.12, review T-02).
local function enum_text(r, t)
  local signed = t and t.u >= K.I8 and t.u <= K.I64
  if r:len() == 8 then return tostring(signed and r:le_int64() or r:le_uint64()) end
  return tostring(int(signed and r:le_int() or r:le_uint()))
end
local function alt_text(a)
  if a.k == K.STRUCT or a.k == K.ENUM then local t = TYPES[a.th or ""]; return t and t.name or ("0x" .. (a.th or "?")) end
  return KNAME[a.k] or (a.k == K.STRING and "string" or a.k == K.VECTOR and "vector" or a.k == K.OPTIONAL and "optional" or a.k == K.MAP and "map" or a.k == K.SET and "set" or "kind " .. a.k)
end

-- The slot rules of format.hpp (fmt::image_budget::take): n == 0 needs off == 0; a block lies after the root's
-- fixed part and inside the image; all blocks of one image together fit in its size.
local function take(ctx, off, n, bytes)
  if n == 0 then return off == 0 end
  if off < ctx.fixed or off > ctx.len or bytes > ctx.len - off or bytes > ctx.left then return false end
  ctx.left = ctx.left - bytes
  return true
end
local function slot(ctx, abs) need(ctx.tvb, abs, 8); return u32(ctx.tvb, abs), u32(ctx.tvb, abs + 4) end

-- srdump's inline text of a value (src/formatters.cpp text_visitor): quoted strings with \0 \n \xNN escapes.
local function quote(str)
  local out = { '"' }
  for i = 1, #str do
    local c = str:byte(i)
    if c == 34 or c == 92 then out[#out + 1] = "\\" .. string.char(c)
    elseif c == 0 then out[#out + 1] = "\\0"
    elseif c == 10 then out[#out + 1] = "\\n"
    elseif c < 32 or c >= 127 then out[#out + 1] = string.format("\\x%02x", c)
    else out[#out + 1] = string.char(c) end
  end
  out[#out + 1] = '"'
  return table.concat(out)
end
local function number_text(r, k)
  if k == K.BOOL then return r:le_uint() ~= 0 and "true" or "false" end
  if k == K.I64 then return tostring(r:le_int64()) end
  if k == K.U64 then return tostring(r:le_uint64()) end
  if k <= K.I32 then return tostring(r:le_int()) end
  return tostring(r:le_uint())
end
-- What std::format("{}", x) prints: the fewest significant digits that read back as the same value (for a float,
-- the same single-precision value), in fixed or scientific notation, whichever is shorter (a tie is fixed).
local function float_text(r)
  local v = r:le_float()
  if v ~= v then return "nan" end
  if v == math.huge then return "inf" elseif v == -math.huge then return "-inf" end
  if v == 0 then return (1 / v < 0) and "-0" or "0" end
  local single = r:len() == 4
  local half_ulp = 0
  if single then
    local e = math.floor(math.log(math.abs(v)) / math.log(2))
    half_ulp = 2 ^ (math.max(e, -126) - 24)
  end
  for p = 1, 17 do
    local sci = string.format("%." .. (p - 1) .. "e", v)
    local back = tonumber(sci)
    if (single and math.abs(back - v) <= half_ulp) or (not single and back == v) then
      local mant, exp = sci:match("^(-?[%d%.]+)e([-+]%d+)$")
      local e10 = tonumber(exp)
      local fixed
      if e10 >= 0 and e10 < 17 then fixed = string.format("%." .. math.max(0, p - 1 - e10) .. "f", back)
      elseif e10 < 0 then fixed = string.format("%." .. (p - 1 - e10) .. "f", back) end
      if mant:find("%.") then mant = mant:gsub("0+$", ""):gsub("%.$", "") end
      local s = mant .. "e" .. (e10 < 0 and "-" or "+") .. string.format("%02d", math.abs(e10))
      if fixed and #fixed <= #s then return fixed end
      return s
    end
  end
  return tostring(v)
end
local function scalar_inline(r, k)
  if k == K.CHAR or k == K.CHAR8 then return quote(r:raw()) end
  if k == K.BYTE then return string.format("0x%02x", r:le_uint()) end
  if k == K.F32 or k == K.F64 then return float_text(r) end
  return number_text(r, k)
end
local function enum_inline(r, t)
  local name = t and t.values[enum_value(r, t)]
  local tn = t and t.name or "enum"
  local v = enum_text(r, t)
  return name and (tn .. "::" .. name .. " (" .. v .. ")") or (tn .. "(" .. v .. ")")
end

local walk_struct
-- Walks one value of `f` whose fixed part starts at `abs` (absolute tvb offset): adds its tree items and returns
-- its inline text.
local function walk_value(ctx, tree, abs, f, name, depth)
  local tvb = ctx.tvb
  if depth > MAX_DEPTH then fail("nesting too deep") end
  need(tvb, abs, f.s)
  local label = name ~= "" and name or "[]"
  if f.k < 32 then
    local r = tvb(abs, f.s)
    if f.k == K.BOOL and r:le_uint() > 1 then
      local it = tree:add(PF[f.pf], r, true)
      it:add_proto_expert_info(X.bool)
      return "true"
    end
    tree:add_le(PF[f.pf], r)
    return scalar_inline(r, f.k)
  elseif f.k == K.ENUM then
    if f.s ~= 1 and f.s ~= 2 and f.s ~= 4 and f.s ~= 8 then fail("enum size " .. f.s) end
    local r = tvb(abs, f.s)
    local it = tree:add_le(PF[f.pf], r)
    local t = TYPES[f.th or ""]
    if f.s == 8 and t and t.values[enum_value(r, t)] then it:append_text(" (" .. t.values[enum_value(r, t)] .. ")") end
    return enum_inline(r, t)
  elseif f.k == K.ARRAY then
    local e = f.e and f.e[1]
    if not e then fail("array without element descriptor") end
    if f.str then
      tree:add(PF[f.pf], tvb(abs, f.s))
      return f.s > 0 and quote(tvb(abs, f.s):raw()) or "[]"
    end
    local n = f.x or 0
    if e.s == 0 then if n ~= 0 then fail("zero-size element") end
    elseif n > f.s / e.s then fail("array extent") end
    local sub = tree:add(tvb(abs, f.s), label .. " [" .. n .. "]")
    local st, items = stride(e), {}
    for i = 0, n - 1 do
      if i * st + e.s > f.s then fail("array element outside the member") end
      items[#items + 1] = walk_value(ctx, sub, abs + i * st, e, name, depth + 1)
    end
    return "[" .. table.concat(items, ", ") .. "]"
  elseif f.k == K.STRUCT then
    local t = TYPES[f.th or ""]
    if not t then
      local it = tree:add(tvb(abs, f.s), label .. ": type 0x" .. (f.th or "?"))
      it:add_proto_expert_info(X.unknown_type)
      return "<0x" .. (f.th or "?") .. ">"
    end
    return walk_struct(ctx, tree, abs, f.s, t, name, depth + 1)
  elseif f.k == K.STRING then
    local off, n = slot(ctx, abs)
    if not take(ctx, off, n, n) then fail(label .. ": string slot") end
    if n == 0 then tree:add(PF[f.pf], tvb(abs, 8), ""); return '""' end
    local r = tvb(ctx.base + off, n)
    tree:add(PF[f.pf], r)
    return quote(r:raw())
  elseif f.k == K.VECTOR then
    local e = f.e and f.e[1]
    if not e then fail("vector without element descriptor") end
    local off, n = slot(ctx, abs)
    local st, items = stride(e), {}
    if (n > 0 and e.s == 0) or not take(ctx, off, n, n * st) then fail(label .. ": vector slot") end
    local sub = tree:add(tvb(abs, 8), label .. ": vector[" .. n .. "]")
    for i = 0, n - 1 do items[#items + 1] = walk_value(ctx, sub, ctx.base + off + i * st, e, name, depth + 1) end
    return "[" .. table.concat(items, ", ") .. "]"
  elseif f.k == K.MAP or f.k == K.SET then
    -- 1.6: a block of `x`-byte entries in key order; a map entry holds the key at 0 and the value at its offset
    local kf = f.e and f.e[1]
    local vf = f.k == K.MAP and f.e and f.e[2] or nil
    if not kf or (f.k == K.MAP and not vf) or not f.x or f.x == 0 then fail(label .. ": map/set descriptor") end
    local off, n = slot(ctx, abs)
    local st, items = f.x, {}
    if not take(ctx, off, n, n * st) then fail(label .. ": map/set slot") end
    local sub = tree:add(tvb(abs, 8), label .. (f.k == K.MAP and ": map[" or ": set[") .. n .. "]")
    for i = 0, n - 1 do
      local at = ctx.base + off + i * st
      if f.k == K.MAP then
        local ent = sub:add(tvb(at, st), "[" .. i .. "]")
        local kt = walk_value(ctx, ent, at, kf, "key", depth + 1)
        local vt = walk_value(ctx, ent, at + vf.o, vf, "value", depth + 1)
        items[#items + 1] = kt .. ": " .. vt
      else
        items[#items + 1] = walk_value(ctx, sub, at, kf, name, depth + 1)
      end
    end
    return "{" .. table.concat(items, ", ") .. "}"
  elseif f.k == K.OPTIONAL then
    local e = f.e and f.e[1]
    if not e then fail("optional without element descriptor") end
    local off, n = slot(ctx, abs)
    if n > 1 or not take(ctx, off, n, e.s) then fail(label .. ": optional slot") end
    if n == 0 then tree:add(tvb(abs, 8), label .. ": null"); return "null" end
    return walk_value(ctx, tree, ctx.base + off, e, name, depth + 1)
  elseif f.k == K.VARIANT then
    local off, n = slot(ctx, abs)
    if not f.e or #f.e ~= (f.x or 0) or #f.e == 0 or n > #f.e then fail(label .. ": variant slot") end
    if n == 0 then
      if not take(ctx, off, 0, 0) then fail(label .. ": variant slot") end
      tree:add(tvb(abs, 8), label .. ": null")
      return "null"
    end
    local a = f.e[n]
    if not take(ctx, off, n, a.s) then fail(label .. ": variant slot") end
    local head = "variant<" .. (n - 1) .. ":" .. alt_text(a) .. ">"
    local sub = tree:add(tvb(abs, 8), label .. ": " .. head)
    return head .. "{value: " .. walk_value(ctx, sub, ctx.base + off, a, "value", depth + 1) .. "}"
  end
  fail("member kind " .. f.k .. " is not supported")
end

-- A struct (or enum) value of type t at abs, `avail` bytes available for its fixed part; returns its inline text.
-- `into`: add the members to `tree` itself (a top-level record, which already has its own subtree).
walk_struct = function(ctx, tree, abs, avail, t, name, depth, into)
  local tvb = ctx.tvb
  if t.enum then
    if avail ~= 1 and avail ~= 2 and avail ~= 4 and avail ~= 8 then fail(t.name .. ": enum size") end
    local r = tvb(abs, avail)
    local it = tree:add_le(PF[t.pf], r)
    if avail == 8 and t.values[enum_value(r, t)] then it:append_text(" (" .. t.values[enum_value(r, t)] .. ")") end
    return enum_inline(r, t)
  end
  if avail < t.size then fail(t.name .. ": " .. avail .. " bytes for a " .. t.size .. "-byte value") end
  need(tvb, abs, t.size)
  if t.transparent then
    -- 1.6: a container argument: the record is the container; its one field is shown without a struct around it
    local f = t.fields[1]
    if not f or f.o + f.s > avail then fail(t.name .. ": transparent type without its field") end
    return walk_value(ctx, tree, abs + f.o, f, "", depth + 1)
  end
  local sub = into and tree or tree:add(tvb(abs, t.size), (name ~= "" and (name .. ": ") or "") .. t.name)
  local parts = {}
  for _, f in ipairs(t.fields) do
    if f.o + f.s > avail then fail(t.name .. "." .. f.n .. ": outside the value") end
    parts[#parts + 1] = f.n .. ": " .. walk_value(ctx, sub, abs + f.o, f, f.n, depth + 1)
  end
  return t.name .. "{" .. table.concat(parts, ", ") .. "}"
end

-- One typed record: a flat object (`len` bytes at p) or a dynamic record (u32 len, u32 0, image). Returns the
-- text a print frame shows for it: the enumerator name of an enum, the inline text of a struct.
local function type_record(tvb, tree, p, len, t, label)
  local ctx = { tvb = tvb, base = p, len = len, fixed = t.enum and len or t.size, left = len }
  if not t.flat and not t.enum then
    if len < 8 then fail("dynamic record prefix") end
    local ilen = u32(tvb, p)
    if u32(tvb, p + 4) ~= 0 or ilen + 8 ~= len then fail("dynamic record prefix") end
    ctx.base, ctx.len, ctx.left = p + 8, ilen, ilen
  end
  local sub = tree:add(tvb(p, len), label .. " " .. t.name)
  sub:add(H.type, tvb(p, len), t.name)
  local text = walk_struct(ctx, sub, ctx.base, ctx.len, t, "", 0, true)
  if t.enum then return t.values[enum_value(tvb(p, len), t)] or enum_text(tvb(p, len), t) end
  return text
end

-- A top-level scalar as a print frame shows it (std::format "{}" of the C++ value) and, for floating point, the
-- number itself (for {:.Nf}).
local function scalar_text(r, k)
  if k == K.F32 or k == K.F64 then return float_text(r), r:le_float() end
  if k == K.CHAR or k == K.CHAR8 then return r:raw() end
  return number_text(r, k)
end

-- ---- print/println: std::format's replacement fields, rendered from the records ------------------------------
-- {[arg-id][:[[fill]align][sign][#][0][width][.precision][L][type]]} for integers, floating point, bool, char and
-- strings (literals, strings, enumerator names, structs as srdump's inline text). Not supported: nested
-- replacement fields for width/precision ({:{}}) — the field is then shown as {…}.
local INT_KINDS = { [K.I8] = true, [K.I16] = true, [K.I32] = true, [K.I64] = true, [K.U8] = true, [K.U16] = true,
                    [K.U32] = true, [K.U64] = true, [K.CHAR16] = true, [K.CHAR32] = true, [K.WCHAR] = true,
                    [K.BYTE] = true }

-- sign, magnitude (UInt64) and decimal text of an integer record
local function int_parts(r, k)
  local signed = k >= K.I8 and k <= K.I64
  if r:len() == 8 then
    local u = r:le_uint64()
    if signed and r:le_int64() < Int64(0) then return true, UInt64(0) - u, (tostring(r:le_int64()):gsub("^-", "")) end
    return false, u, tostring(u)
  end
  local n = signed and r:le_int() or r:le_uint()
  return n < 0, UInt64(math.abs(n)), string.format("%d", math.abs(n))
end
local function digits(mag, base)
  if base == 16 then
    local h = string.lower(mag:tohex()):gsub("^0+", "")
    return h == "" and "0" or h
  end
  local out = {}
  while mag:tonumber() ~= 0 do
    local d = int((mag % base):tonumber())
    out[#out + 1] = string.sub("0123456789", d + 1, d + 1)
    mag = mag / base
  end
  return #out == 0 and "0" or string.reverse(table.concat(out))
end

local function parse_spec(spec)
  local s = { fill = " ", align = nil, sign = "-", alt = false, zero = false, width = 0, prec = nil, type = "" }
  local i = 1
  local c2 = spec:sub(2, 2)
  if #spec >= 2 and (c2 == "<" or c2 == ">" or c2 == "^") then s.fill, s.align, i = spec:sub(1, 1), c2, 3
  elseif spec:sub(1, 1):match("[<>%^]") then s.align, i = spec:sub(1, 1), 2 end
  local c = spec:sub(i, i)
  if c == "+" or c == "-" or c == " " then s.sign, i = c, i + 1 end
  if spec:sub(i, i) == "#" then s.alt, i = true, i + 1 end
  if spec:sub(i, i) == "0" then s.zero, i = true, i + 1 end
  local w = spec:match("^%d+", i)
  if w then s.width, i = tonumber(w), i + #w end
  if spec:sub(i, i) == "." then
    local p = spec:match("^%d+", i + 1)
    if not p then return nil end
    s.prec, i = tonumber(p), i + 1 + #p
  end
  if spec:sub(i, i) == "L" then i = i + 1 end
  s.type = spec:sub(i)
  if #s.type > 1 or spec:find("{", 1, true) then return nil end
  return s
end

-- pads `body` (with `prefix` = sign and base prefix) to the spec's width
local function pad(s, prefix, body, numeric)
  local len = #prefix + #body
  if len >= s.width then return prefix .. body end
  local n = s.width - len
  if numeric and s.zero and not s.align then return prefix .. string.rep("0", n) .. body end
  local align = s.align or (numeric and ">" or "<")
  local text = prefix .. body
  if align == "<" then return text .. string.rep(s.fill, n) end
  if align == ">" then return string.rep(s.fill, n) .. text end
  local left = math.floor(n / 2)
  return string.rep(s.fill, left) .. text .. string.rep(s.fill, n - left)
end
local function sign_of(s, neg) return neg and "-" or (s.sign == "+" and "+" or (s.sign == " " and " " or "")) end

local function format_int(s, neg, mag, dec)
  local t = s.type
  local base, prefix = 10, ""
  if t == "x" or t == "X" then base, prefix = 16, "0" .. t
  elseif t == "b" or t == "B" then base, prefix = 2, "0" .. t
  elseif t == "o" then base, prefix = 8, "0"
  elseif t ~= "" and t ~= "d" then return nil end
  local body = base == 10 and dec or digits(mag, base)
  if t == "X" then body = body:upper() end
  if not s.alt or (t == "o" and body == "0") then prefix = "" end
  return pad(s, sign_of(s, neg) .. prefix, body, true)
end

local function format_float(s, r)
  local v = r:le_float()
  local t = s.type
  local body
  if v ~= v or v == math.huge or v == -math.huge then
    body = (v ~= v) and "nan" or "inf"
    if t == "E" or t == "F" or t == "G" or t == "A" then body = body:upper() end
    s.zero = false
  elseif t == "" and not s.prec then body = float_text(r):gsub("^-", "")
  else
    local ct = t == "" and "g" or t
    if not ct:match("^[aAeEfFgG]$") then return nil end
    local p = s.prec
    if not p and not ct:match("[aA]") then p = 6 end -- %a without a precision: the shortest exact form
    body = string.format("%" .. (s.alt and "#" or "") .. (p and ("." .. p) or "") .. ct, math.abs(v))
    if ct == "a" or ct == "A" then body = body:gsub("^0[xX]", "") end
  end
  local neg = v < 0 or (v == 0 and 1 / v < 0)
  return pad(s, sign_of(s, neg), body, true)
end

local function format_string(s, text)
  if s.type ~= "" and s.type ~= "s" then return nil end
  if s.prec then text = text:sub(1, s.prec) end
  return pad(s, "", text, false)
end

-- One replacement field for argument `a` ({ text = …, r = TvbRange, k = kind } for scalars, { text = … } else).
local function format_arg(a, spec)
  local s = parse_spec(spec)
  if not s or not a then return nil end
  if a.r and INT_KINDS[a.k] then return format_int(s, int_parts(a.r, a.k)) end
  if a.r and (a.k == K.F32 or a.k == K.F64) then return format_float(s, a.r) end
  if a.r and a.k == K.BOOL then
    local on = a.r:le_uint() ~= 0
    if s.type == "" or s.type == "s" then return format_string(s, on and "true" or "false") end
    return format_int(s, false, UInt64(on and 1 or 0), on and "1" or "0")
  end
  if a.r and (a.k == K.CHAR or a.k == K.CHAR8) then
    if s.type == "" or s.type == "c" then return pad(s, "", a.r:raw(), false) end
    local n = a.r:le_uint()
    return format_int(s, false, UInt64(n), string.format("%d", n))
  end
  return format_string(s, a.text)
end

local LOG_LEVELS = { [0] = "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" }

-- print/println frames: the format literal with each replacement field rendered from the next (or the numbered)
-- argument, as std::format does.
local function render(fmt, args)
  local auto, out = 0, {}
  local pos = 1
  while pos <= #fmt do
    local c = fmt:sub(pos, pos)
    if c == "{" and fmt:sub(pos + 1, pos + 1) == "{" then out[#out + 1] = "{"; pos = pos + 2
    elseif c == "}" and fmt:sub(pos + 1, pos + 1) == "}" then out[#out + 1] = "}"; pos = pos + 2
    elseif c == "{" then
      local close = fmt:find("}", pos, true) or #fmt
      local field = fmt:sub(pos + 1, close - 1)
      local id, spec = field:match("^(%d*):?(.*)$")
      local idx
      if id ~= "" then idx = tonumber(id) + 1 else auto = auto + 1; idx = auto end
      out[#out + 1] = format_arg(args[idx], spec or "") or ("{" .. field .. "}")
      pos = close + 1
    else out[#out + 1] = c; pos = pos + 1 end
  end
  return table.concat(out)
end

local function literal_item(tree, tvb, p, id)
  local text = LITERALS[id]
  tree:add(H.literal, tvb(p, 0), text or ("0x" .. id))
  return text or ("0x" .. id)
end

-- One frame at `off`; returns its length.
local function dissect_frame(tvb, pinfo, tree, off, info)
  need(tvb, off, 16)
  local flen, flags, n = u32(tvb, off), u16(tvb, off + 4), u16(tvb, off + 6)
  if flen < 16 or flen % 8 ~= 0 or off + flen > tvb:len() then fail("frame length " .. flen) end
  local call = hex(tvb, off + 8)
  local ft = tree:add(serreg, tvb(off, flen), "es::serreg frame")
  ft:add_le(H.frame_len, tvb(off, 4))
  ft:add_le(H.frame_flags, tvb(off + 4, 2))
  ft:add_le(H.frame_records, tvb(off + 6, 2))
  ft:add_le(H.frame_call, tvb(off + 8, 8))
  local fend = off + flen
  if bit(flags, 4) then -- 1.7: an 8-byte CRC32C trailer; the records end before it
    fend = fend - 8
    local want, got = u32(tvb, off + flen - 8), crc32c(tvb, off, flen - 8)
    local it = ft:add(tvb(off + flen - 8, 4), string.format("crc32c 0x%08x%s", want, want == got and "" or string.format(" (computed 0x%08x)", got)))
    if want ~= got then -- as every other reader: reported and skipped, never decoded
      it:add_proto_expert_info(X.crc)
      ft:add(H.frame_kind, tvb(off, 16), "crc mismatch")
      info[#info + 1] = "crc mismatch"
      return flen
    end
  end
  if bit(flags, 2) then -- a dictionary frame: payload = one RRI (1.7: then u64 next_dictionary)
    ft:add(H.frame_kind, tvb(off, 16), "dictionary")
    info[#info + 1] = "dictionary"
    return flen, "rri", off + 16
  end
  if bit(flags, 8) then -- 1.7: a checkpoint frame
    ft:add(H.frame_kind, tvb(off, 16), "checkpoint")
    local p = off + 16
    local st = ft:add(tvb(p, fend - p), "checkpoint")
    st:add(tvb(p + 8, 8), "prev checkpoint @" .. tvb(p + 8, 8):le_uint64():tonumber())
    st:add(tvb(p + 16, 8), "next checkpoint @" .. tvb(p + 16, 8):le_uint64():tonumber())
    st:add(tvb(p + 24, 8), "data frames before: " .. tvb(p + 24, 8):le_uint64():tonumber())
    st:add(tvb(p + 32, 8), "bytes before: " .. tvb(p + 32, 8):le_uint64():tonumber())
    st:add(tvb(p + 40, 8), "wall clock ns: " .. tvb(p + 40, 8):le_uint64():tonumber())
    st:add(tvb(p + 48, 8), "latest dictionary @" .. tvb(p + 48, 8):le_uint64():tonumber())
    info[#info + 1] = "checkpoint"
    return flen
  end
  if bit(flags, 16) then -- 1.7: the writer-info frame: u16 version, u16 n, (u16 key, u16 len, bytes, pad 2)*
    ft:add(H.frame_kind, tvb(off, 16), "writer info")
    local p = off + 16
    local n = u16(tvb, p + 2)
    local st = ft:add(tvb(p, fend - p), "writer info (" .. n .. " items)")
    p = p + 4
    for i = 1, n do
      if p + 4 > fend then break end
      local key, l = u16(tvb, p), u16(tvb, p + 2)
      if p + 4 + l > fend then break end
      st:add(tvb(p, 4 + l), "key " .. key .. ": " .. tvb(p + 4, l):string())
      p = p + 4 + l + (l % 2)
    end
    info[#info + 1] = "writer info"
    return flen
  end
  local sig = CALLS[call]
  if sig then ft:add(H.call_name, tvb(off + 8, 8), sig.name) end
  local texts, scal, summary = {}, {}, {}
  local p = off + 16
  local full = bit(flags, 1)
  ft:add(H.frame_kind, tvb(off, 16), (full and "full" or "compact") .. ((sig and bit(sig.flags, 1)) and (bit(sig.flags, 4) and ", log" or bit(sig.flags, 2) and ", println" or ", print") or ""))
  if not full then
    if not sig then
      ft:add_proto_expert_info(X.unknown_call)
      ft:add(tvb(p, fend - p), "records (call 0x" .. call .. " unknown; " .. (fend - p) .. " bytes)")
      info[#info + 1] = "call 0x" .. call .. " (unknown)"
      return flen
    end
    if #sig.args ~= n then ft:add_proto_expert_info(X.records) end
    for i, a in ipairs(sig.args) do
      p = off + align8(p - off)
      local label = "[" .. (i - 1) .. "]"
      local ok, err = pcall(function()
        if a.ak == AK_LITERAL then
          local text = literal_item(ft, tvb, p, a.h)
          texts[i] = text; summary[#summary + 1] = "\"" .. text .. "\""
        elseif a.ak == AK_SCALAR then
          local sz = KSIZE[a.k]
          need(tvb, p, sz)
          local r = tvb(p, sz)
          ft:add_le(ARGF[a.k], r)
          texts[i], scal[i] = scalar_text(r, a.k), { r = r, k = a.k }; summary[#summary + 1] = texts[i]
          p = p + sz
        elseif a.ak == AK_CONST then
          -- 1.5: a compile-time constant of the call: no bytes in the frame; the value comes from the call's
          -- entry and is shown (as a generated item, from its own 8-byte data source) as the scalar or enum it is
          local t = (not a.k) and TYPES[a.h] or nil
          local sz = a.k and KSIZE[a.k] or (t and (t.flat or t.enum) and t.size)
          if not sz or sz > 8 then fail("constant of unknown type 0x" .. a.h) end
          local ctvb = ByteArray.new(a.v):tvb("serreg constant " .. label)
          if a.k then
            local r = ctvb(0, sz)
            ft:add_le(ARGF[a.k], r):set_generated()
            texts[i], scal[i] = scalar_text(r, a.k), { r = r, k = a.k }; summary[#summary + 1] = texts[i]
          else
            texts[i] = type_record(ctvb, ft, 0, sz, t, label)
            summary[#summary + 1] = t.name
          end
        elseif a.ak == AK_STRING then
          need(tvb, p, 4)
          local len = u32(tvb, p)
          need(tvb, p + 4, len)
          ft:add(H.string, tvb(p + 4, len))
          texts[i] = tvb(p + 4, len):string(); summary[#summary + 1] = "\"" .. texts[i] .. "\""
          p = p + 4 + len
        else
          local t = TYPES[a.h]
          if not t then
            ft:add(tvb(p, fend - p), label .. " type 0x" .. a.h):add_proto_expert_info(X.unknown_type)
            p = fend
            fail(nil)
          end
          local len = t.size
          if not t.flat and not t.enum then need(tvb, p, 4); len = 8 + u32(tvb, p) end
          need(tvb, p, len)
          texts[i] = type_record(tvb, ft, p, len, t, label)
          summary[#summary + 1] = t.name
          p = p + len
        end
      end)
      if not ok then
        if type(err) == "table" and err.serreg == nil then break end
        ft:add(tvb(off, flen), label .. " " .. (type(err) == "table" and err.serreg or tostring(err))):add_proto_expert_info(X.malformed)
        break
      end
    end
  else
    for i = 1, n do
      local label = "[" .. (i - 1) .. "]"
      local ok, err = pcall(function()
        need(tvb, p, 16)
        local h, len = hex(tvb, p), u32(tvb, p + 8)
        need(tvb, p + 16, len)
        local q = p + 16
        if LITERALS[h] then
          local text = literal_item(ft, tvb, q, h)
          texts[i] = text; summary[#summary + 1] = "\"" .. text .. "\""
        elseif h == SERREG.string_hash then
          if len < 4 or u32(tvb, q) + 4 ~= len then fail("string record length") end
          ft:add(H.string, tvb(q + 4, len - 4))
          texts[i] = tvb(q + 4, len - 4):string(); summary[#summary + 1] = "\"" .. texts[i] .. "\""
        elseif SCALARS[h] then
          local k = SCALARS[h]
          if len ~= KSIZE[k] then fail("scalar record length") end
          ft:add_le(ARGF[k], tvb(q, len))
          texts[i], scal[i] = scalar_text(tvb(q, len), k), { r = tvb(q, len), k = k }; summary[#summary + 1] = texts[i]
        elseif TYPES[h] then
          local t = TYPES[h]
          if (t.flat or t.enum) and len ~= t.size then fail(t.name .. ": payload length " .. len) end
          texts[i] = type_record(tvb, ft, q, len, t, label)
          summary[#summary + 1] = t.name
        else
          local it = ft:add(tvb(p, 16 + len), label .. " record 0x" .. h .. " (" .. len .. " bytes)")
          it:add_le(H.record_hash, tvb(p, 8))
          it:add_le(H.record_len, tvb(p + 8, 4))
          it:add_proto_expert_info(X.unknown_type)
          summary[#summary + 1] = "0x" .. h
        end
        p = p + 16 + align8(len)
      end)
      if not ok then
        ft:add(tvb(off, flen), label .. " " .. (type(err) == "table" and (err.serreg or "?") or tostring(err))):add_proto_expert_info(X.malformed)
        break
      end
    end
  end
  -- 1.7 log calls: level, file, line, mask, time, [thread], [sequence] precede the format literal
  local first, prefix = 1, ""
  if sig and bit(sig.flags, 4) and #sig.args >= 6 then
    local mask = tonumber(texts[4]) or 0
    first = 6 + (bit(mask, 1) and 1 or 0) + (bit(mask, 2) and 1 or 0)
    local lvl = LOG_LEVELS[tonumber(texts[1]) or 0] or tostring(texts[1])
    prefix = lvl .. " " .. tostring(texts[2]) .. ":" .. tostring(texts[3]) .. " "
    if bit(mask, 1) then prefix = prefix .. "[" .. tostring(texts[6]) .. "] " end
    if bit(mask, 2) then prefix = prefix .. "#" .. tostring(texts[bit(mask, 1) and 7 or 6]) .. " " end
  end
  if sig and bit(sig.flags, 1) and type(texts[first]) == "string" then
    local args = {}
    for i = first + 1, #sig.args do
      args[i - first] = scal[i] and { text = texts[i], r = scal[i].r, k = scal[i].k } or { text = tostring(texts[i] or "") }
    end
    local msg = prefix .. render(texts[first], args)
    ft:add(H.message, tvb(off, flen), msg)
    info[#info + 1] = msg
  else
    info[#info + 1] = table.concat(summary, ", ")
  end
  return flen
end

-- An RRI (file dictionary, dictionary frame or relocated dictionary) compared with this dissector's tables.
local function dissect_rri(tvb, tree, off, len)
  need(tvb, off, 40)
  if tvb(off, 4):string() ~= "ESRG" then fail("RRI magic") end
  local dt = tree:add(serreg, tvb(off, len), "es::serreg dictionary")
  local maj, min = u16(tvb, off + 4), u16(tvb, off + 6)
  dt:add(H.file_version, tvb(off + 4, 4), maj .. "." .. min)
  local n = u32(tvb, off + 16)
  dt:add_le(H.dict_entries, tvb(off + 16, 4))
  local ph = dt:add_le(H.dict_program, tvb(off + 20, 8))
  if hex(tvb, off + 20) ~= SERREG.program_hash then ph:add_proto_expert_info(X.program) end
  if hex(tvb, off + 32) ~= SERREG.abi then dt:add_proto_expert_info(X.abi) end
  local p, known, unknown, names = off + 40, 0, 0, {}
  for _ = 1, n do
    if p + 16 > off + len then break end
    local elen, tag = u32(tvb, p), tvb(p + 4, 1):uint()
    if elen < 16 or p + elen > off + len then break end
    local h = hex(tvb, p + 8)
    local have = (tag <= 2 and TYPES[h]) or (tag == 3 and LITERALS[h]) or (tag == 4 and CALLS[h])
    if have then
      known = known + 1
      if tag <= 2 and TYPES[h].lh ~= hex(tvb, p + 16) then
        dt:add(tvb(p, elen), TYPES[h].name .. ": layout 0x" .. hex(tvb, p + 16) .. " in the file, 0x" .. TYPES[h].lh .. " here"):add_proto_expert_info(X.layout)
      end
    else
      unknown = unknown + 1
      if tag <= 2 and #names < 5 and p + 42 <= off + len then
        local nl = u16(tvb, p + 38)
        if p + 42 + nl <= off + len then names[#names + 1] = tvb(p + 42, nl):string() end
      elseif tag == 4 and #names < 5 then names[#names + 1] = "call 0x" .. h end
    end
    p = p + elen
  end
  dt:add(H.dict_known, tvb(off, 40), known)
  local ui = dt:add(H.dict_unknown, tvb(off, 40), unknown)
  if unknown > 0 then
    ui:append_text(" (" .. table.concat(names, ", ") .. (unknown > #names and ", ..." or "") .. ")")
    ui:add_proto_expert_info(X.unknown_entries)
  end
  return known, unknown
end

local function dissect_file_header(tvb, tree, info)
  local ht = tree:add(serreg, tvb(0, math.min(48, tvb:len())), "es::serreg capture file")
  ht:add(H.file_magic, tvb(0, 4))
  ht:add(H.file_version, tvb(4, 4), u16(tvb, 4) .. "." .. u16(tvb, 6))
  ht:add_le(H.file_flags, tvb(8, 4))
  ht:add_le(H.file_program, tvb(16, 8))
  ht:add_le(H.file_dict_offset, tvb(24, 8))
  ht:add_le(H.file_dict_len, tvb(32, 8))
  local at = ht:add_le(H.file_abi, tvb(40, 8))
  if hex(tvb, 40) ~= SERREG.abi then at:add_proto_expert_info(X.abi) end
  info[#info + 1] = "file header, format " .. u16(tvb, 4) .. "." .. u16(tvb, 6)
  if tvb:len() > 48 and tvb:len() >= 88 and tvb(48, 4):string() == "ESRG" then
    local known, unknown = dissect_rri(tvb, tree, 48, tvb:len() - 48)
    info[#info + 1] = "dictionary: " .. known .. " entries known, " .. unknown .. " unknown"
  end
end

local function dissect_serreg(tvb, pinfo, tree)
  pinfo.cols.protocol = "SERREG"
  local info = {}
  local ok, err = pcall(function()
    local len = tvb:len()
    if len >= 4 and tvb(0, 4):string() == "ESSR" then dissect_file_header(tvb, tree, info); return end
    if len >= 4 and tvb(0, 4):string() == "ESRG" then
      local known, unknown = dissect_rri(tvb, tree, 0, len)
      info[#info + 1] = "dictionary: " .. known .. " entries known, " .. unknown .. " unknown"
      return
    end
    local off = 0
    while off + 16 <= len do
      local flen, kind, at = dissect_frame(tvb, pinfo, tree, off, info)
      if kind == "rri" then dissect_rri(tvb, tree, at, off + flen - at) end
      off = off + flen
    end
  end)
  if not ok then
    tree:add(serreg, tvb(), "es::serreg: " .. (type(err) == "table" and (err.serreg or "?") or tostring(err))):add_proto_expert_info(X.malformed)
  end
  pinfo.cols.info = table.concat(info, "; ")
  return tvb:len()
end
serreg.dissector = dissect_serreg

-- ---- where frames come from ----------------------------------------------------------------------------------
-- 1. FILE captures (.ser): the reader below presents the header + dictionary as packet 1 and each frame as one
--    packet (a back-patched file's relocated dictionary is the last packet). Link type: USER0.
-- 2. pcap/pcapng with link type USER0 (147): one or more frames per packet.
-- 3. TCP/UDP: set the port in Preferences → Protocols → SERREG.
DissectorTable.get("wtap_encap"):add(wtap_encaps.USER0, serreg)

local function le(s, i, n) local v = 0; for k = n - 1, 0, -1 do v = v * 256 + s:byte(i + k) end; return v end

local function rri_length(file, at)
  file:seek("set", at)
  local h = file:read(40)
  if not h or #h < 40 or h:sub(1, 4) ~= "ESRG" then return nil end
  local n, pos = le(h, 17, 4), at + 40
  for _ = 1, n do
    file:seek("set", pos)
    local e = file:read(4)
    if not e or #e < 4 then return nil end
    local elen = le(e, 1, 4)
    if elen < 8 then return nil end
    pos = pos + elen
  end
  return pos - at
end

local reader = FileHandler.new("es::serreg capture", "serreg", "es::serreg capture file (FILE header, dictionary, frames)", "rs")
reader.extensions = "ser"

reader.read_open = function(file, capture)
  file:seek("set", 0)
  local h = file:read(48)
  if not h or #h < 48 or h:sub(1, 4) ~= "ESSR" then return false end
  local size = file:seek("end")
  local minor, flags, dict_off, dict_len = le(h, 7, 2), le(h, 9, 4), le(h, 25, 8), le(h, 33, 8)
  local st = { size = size, first = 48, stop = size, reloc = nil }
  if bit(flags, 2) and dict_len > 0 then
    if minor >= 7 then st.first = align8(48 + dict_len) + 16 -- 1.7: the CHAIN follows the opening dictionary
    elseif dict_off == 48 then st.first = align8(48 + dict_len)
    else
      local old = rri_length(file, 48)
      if not old then return false end
      st.first, st.stop, st.reloc, st.reloc_len = align8(48 + old), dict_off, dict_off, dict_len
    end
  end
  capture.private_table = st
  capture.encap = wtap_encaps.USER0
  capture.time_precision = wtap_tsprecs.NSEC
  file:seek("set", 0)
  return true
end

local function packet_at(file, st, pos)
  if pos == 0 then return 0, st.first end
  if st.reloc and pos == st.reloc then return pos, st.reloc_len end
  if pos + 16 > st.stop then
    if st.reloc and pos < st.reloc then pos = st.reloc; return pos, st.reloc_len end
    return nil
  end
  file:seek("set", pos)
  local fh = file:read(4)
  if not fh or #fh < 4 then return nil end
  local flen = le(fh, 1, 4)
  if flen < 16 or flen % 8 ~= 0 or pos + flen > st.stop then flen = st.stop - pos end -- show the rest as one malformed packet
  return pos, flen
end

local function fill(file, frame, pos, len)
  file:seek("set", pos)
  local data = file:read(len)
  if not data or #data ~= len then return false end
  frame.rec_type = wtap_rec_types.PACKET
  frame.flags = wtap_presence_flags.CAP_LEN
  frame.captured_length = len
  frame.original_length = len
  frame.data = data
  return true
end

reader.read = function(file, capture, frame)
  local st = capture.private_table
  local pos, len = packet_at(file, st, st.next or 0)
  if not pos or len <= 0 then return false end
  if not fill(file, frame, pos, len) then return false end
  st.next = pos + len
  if st.reloc and st.next > st.reloc then st.next = st.size end
  return pos
end

reader.seek_read = function(file, capture, frame, offset)
  local st = capture.private_table
  local pos, len = packet_at(file, st, offset)
  if not pos then return false end
  return fill(file, frame, pos, len)
end

reader.read_close = function(file, capture) return true end
reader.seq_read_close = function(file, capture) return true end
register_filehandler(reader)

-- TCP (frames may span segments) and UDP (one or more frames per datagram), on the ports set in the preferences.
local serreg_tcp = Proto("serreg_tcp", "es::serreg over TCP")
local function dissect_serreg_tcp(tvb, pinfo, tree)
  dissect_tcp_pdus(tvb, tree, 16, function(t, _, off) return t(off, 4):le_uint() end, dissect_serreg)
  return tvb:len()
end
serreg_tcp.dissector = dissect_serreg_tcp
local ports = { tcp = 0, udp = 0 }
function serreg.prefs_changed()
  if ports.tcp ~= serreg.prefs.tcp_port then
    if ports.tcp ~= 0 then DissectorTable.get("tcp.port"):remove(ports.tcp, serreg_tcp) end
    ports.tcp = serreg.prefs.tcp_port
    if ports.tcp ~= 0 then DissectorTable.get("tcp.port"):add(ports.tcp, serreg_tcp) end
  end
  if ports.udp ~= serreg.prefs.udp_port then
    if ports.udp ~= 0 then DissectorTable.get("udp.port"):remove(ports.udp, serreg) end
    ports.udp = serreg.prefs.udp_port
    if ports.udp ~= 0 then DissectorTable.get("udp.port"):add(ports.udp, serreg) end
  end
end

-- Heuristics (2.1.10): a UDP datagram or the start of a TCP stream whose first 16 bytes are a frame header with a
-- call hash of this dissector's tables (or a dictionary frame) is taken as es::serreg without any port setting.
-- A 64-bit call hash leaves no room for false positives. Disable in Analyze → Enabled Protocols (serreg_udp_heur,
-- serreg_tcp_heur) if needed.
local function looks_like_frame(tvb)
  if tvb:len() < 16 then return false end
  local flen, flags = tvb(0, 4):le_uint(), tvb(4, 2):le_uint()
  if flen < 16 or flen % 8 ~= 0 or flags > 31 then return false end
  local call = string.lower(tvb(8, 8):le_uint64():tohex())
  return CALLS[call] ~= nil or (bit(flags, 2) and call == "4553535244494354")
      or (bit(flags, 8) and call == "45535352434b5054") or (bit(flags, 16) and call == "45535352494e464f") -- 1.7 meta
end
serreg:register_heuristic("udp", function(tvb, pinfo, tree)
  if not looks_like_frame(tvb) or tvb(0, 4):le_uint() > tvb:len() then return false end
  dissect_serreg(tvb, pinfo, tree)
  return true
end)
serreg_tcp:register_heuristic("tcp", function(tvb, pinfo, tree)
  if not looks_like_frame(tvb) then return false end
  pinfo.conversation = serreg_tcp -- the rest of the stream (frames that start mid-segment) goes to it too
  dissect_serreg_tcp(tvb, pinfo, tree)
  return true
end)
)LUA";
std::string_view lua_runtime() noexcept
{
    return lua_runtime_text;
}
} // namespace

} // namespace es::serreg::tools
