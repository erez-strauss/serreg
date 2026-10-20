// src/formatters.cpp — text and JSON decoded_value_visitor implementations, tools::dump and
// tools::dump_dictionary (spec §6.9, FR-D1/D2/D8, FR-L3, FR-W8 rendering rules).
// Records are emitted as JSON when fmt == toml (TOML record rendering is not part of v1).
#include "internal.hpp"

#include <es/serreg/deserialize.hpp> // read_frame_header
#include <es/serreg/registry.hpp>
#include <es/serreg/tools.hpp>

#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace es::serreg::tools
{
namespace
{

using internal::append_quoted;
using internal::dec;
using internal::hex64;
using internal::hex8;

// ---- scalar rendering -----------------------------------------------------------------------------------
template<class T> T load(const std::span<const std::byte> b)
{
    T v{};
    std::memcpy(&v, b.data(), sizeof(T));
    return v;
}

template<class F> std::string float_text(const F v)
{
    if (std::isnan(v) || std::isinf(v))
        return "null";
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    if (r.ec != std::errc{})
        return "null"; // gk §2: never use r.ptr unchecked
    return std::string(buf, r.ptr);
}

// kind_size(): descriptor.hpp (1.1: shared with the argument model)

std::string raw_hex(const std::span<const std::byte> b, const char* const sep)
{
    std::string s;
    for (std::size_t i = 0; i < b.size(); ++i)
    {
        if (i)
            s += sep;
        s += hex8(static_cast<std::uint8_t>(b[i]));
    }
    return s;
}

// gk 1.12: JSON numbers are IEEE doubles in most parsers; 64-bit integers go out as quoted decimal strings.
std::string json_int64(const std::string& digits, const bool json)
{
    return json ? '"' + digits + '"' : digits;
}
bool fits_double_exactly(const std::int64_t v)
{
    return v >= -(std::int64_t{1} << 53) && v <= (std::int64_t{1} << 53);
}

// Number-ish rendering shared by text and JSON (JSON: chars/bytes are numbers too; i64/u64 quoted).
std::string scalar_number(const kind k, const std::span<const std::byte> b, const bool json)
{
    if (b.size() != kind_size(k) &&
        !(k == kind::wchar && b.size() == 2)) // 1.3: a 2-byte (Windows) wchar
        return "\"<" + raw_hex(b, " ") + ">\"";
    switch (k)
    {
    case kind::i8:
        return dec(load<std::int8_t>(b));
    case kind::i16:
        return dec(load<std::int16_t>(b));
    case kind::i32:
        return dec(load<std::int32_t>(b));
    case kind::i64:
        return json_int64(dec(load<std::int64_t>(b)), json);
    case kind::u8:
    case kind::char8:
    case kind::byte_:
        return dec(load<std::uint8_t>(b));
    case kind::char_:
        return dec(load<signed char>(b));
    case kind::u16:
    case kind::char16:
        return dec(load<std::uint16_t>(b));
    case kind::u32:
    case kind::char32:
        return dec(load<std::uint32_t>(b));
    case kind::wchar:
        return b.size() == 2 ? dec(load<std::uint16_t>(b)) : dec(load<std::uint32_t>(b));
    case kind::u64:
        return json_int64(dec(load<std::uint64_t>(b)), json);
    case kind::f32:
        return float_text(load<float>(b));
    case kind::f64:
        return float_text(load<double>(b));
    case kind::boolean:
        return load<std::uint8_t>(b) ? "true" : "false";
    default:
        return "null";
    }
}

// Text-mode quoting of char data: printable ASCII as-is, NUL as \0, others as \xNN.
std::string quote_chars(const std::string_view s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (u == 0)
            out += "\\0";
        else if (u == '\n')
            out += "\\n";
        else if (u < 0x20 || u >= 0x7f)
            out += "\\x" + hex8(u);
        else
            out += c;
    }
    return out + '"';
}

bool is_char_kind(const kind k)
{
    return k == kind::char_ || k == kind::char8;
}

// Text-mode value of one scalar (1.1 top-level scalar records): the same rendering text_visitor::scalar gives
// a struct member — chars quoted, bytes as 0xNN, everything else as a number/bool.
std::string scalar_text(const kind k, const std::span<const std::byte> bytes)
{
    if (is_char_kind(k) && bytes.size() == 1)
        return quote_chars({reinterpret_cast<const char*>(bytes.data()), 1});
    if (k == kind::byte_ && bytes.size() == 1)
        return "0x" + hex8(static_cast<std::uint8_t>(bytes[0]));
    return scalar_number(k, bytes, false);
}

// ---- text visitor ----------------------------------------------------------------------------------------
// Block structs print one field per line; anything inside an array (or inside such a struct) prints inline.
struct text_visitor final : decoded_value_visitor
{
    struct ctx
    {
        bool is_array{}, block{};
        bool is_map{}, is_set{}, is_entry{}; // 1.6
        std::string name, buf, indent, close, chars, key, value;
        std::vector<std::string> items;
        bool all_chars{true};
    };
    std::string& out;
    std::string field_indent, close_indent;
    std::uint32_t limit;
    bool full;
    std::vector<ctx> stack;

    text_visitor(std::string& o, const std::string fi, const std::string ci,
                 const std::uint32_t lim, const bool f)
        : out(o), field_indent(fi), close_indent(ci), limit(lim), full(f)
    {
    }

    void emit(const std::string_view name, const std::string& value)
    {
        if (stack.empty())
        {
            out += value;
            out += '\n';
            return;
        }
        ctx& t = stack.back();
        if (t.is_entry)
        { // 1.6: a map entry collects its key and its value, rendered as `key: value` by end_entry
            (name == "key" ? t.key : t.value) = value;
            return;
        }
        if (t.is_array)
        {
            t.items.push_back(value);
            return;
        }
        if (t.block)
        {
            out += t.indent;
            out += name;
            out += ": ";
            out += value;
            out += '\n';
            return;
        }
        if (!t.buf.empty() && t.buf.back() != '{')
            t.buf += ", "; // gk §2: guard an empty inline buffer
        t.buf += name;
        t.buf += ": ";
        t.buf += value;
    }
    void not_chars()
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
    }
    void begin_struct(const std::string_view type_name, const std::string_view field_name) override
    {
        not_chars();
        ctx c;
        c.name = std::string(field_name);
        if (stack.empty())
        {
            c.block = true;
            c.indent = field_indent;
            c.close = close_indent;
            out += type_name;
            out += " {\n";
        }
        else if (stack.back().block && !stack.back().is_array)
        {
            const ctx& p = stack.back();
            c.block = true;
            c.indent = p.indent + "  ";
            c.close = p.indent;
            out += p.indent;
            out += field_name;
            out += ": ";
            out += type_name;
            out += " {\n";
        }
        else
        {
            c.buf = std::string(type_name) + "{";
        }
        stack.push_back(std::move(c));
    }
    void end_struct() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        if (c.block)
        {
            out += c.close;
            out += "}\n";
        }
        else
            emit(c.name, c.buf + "}");
    }
    void begin_array(const std::string_view field_name, std::uint32_t) override
    {
        not_chars();
        ctx c;
        c.is_array = true;
        c.name = std::string(field_name);
        stack.push_back(std::move(c));
    }
    void end_array() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        if (c.all_chars && !c.items.empty())
        {
            emit(c.name, quote_chars(c.chars));
            return;
        }
        std::string s = "[";
        const std::size_t n = full ? c.items.size() : std::min<std::size_t>(c.items.size(), limit);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i)
                s += ", ";
            s += c.items[i];
        }
        if (n < c.items.size())
            s += ", … (+" + dec(c.items.size() - n) + " more)";
        emit(c.name, s + "]");
    }
    void scalar(const std::string_view field_name, const kind k,
                const std::span<const std::byte> bytes) override
    {
        if (!stack.empty() && stack.back().is_array)
        {
            ctx& a = stack.back();
            if (is_char_kind(k) && bytes.size() == 1)
                a.chars += static_cast<char>(bytes[0]);
            else
                a.all_chars = false;
        }
        if (is_char_kind(k) && bytes.size() == 1)
        {
            emit(field_name, quote_chars({reinterpret_cast<const char*>(bytes.data()), 1}));
            return;
        }
        if (k == kind::byte_ && bytes.size() == 1)
        {
            emit(field_name, "0x" + hex8(static_cast<std::uint8_t>(bytes[0])));
            return;
        }
        emit(field_name, scalar_number(k, bytes, false));
    }
    void enumerator(const std::string_view field_name, const std::string_view enum_type,
                    const std::string_view name, const std::int64_t value) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        std::string s(enum_type.empty() ? "enum" : enum_type);
        if (!name.empty())
        {
            s += "::";
            s += name;
            s += " (" + dec(value) + ")";
        }
        else
            s += "(" + dec(value) + ")";
        emit(field_name, s);
    }
    void literal(const std::string_view field_name, const std::uint64_t id,
                 const std::string_view text) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        emit(field_name, text.empty() ? "lit#" + hex64(id).substr(2) : quote_chars(text));
    }
    // v2
    void string(const std::string_view field_name, const std::string_view s) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        emit(field_name, quote_chars(s));
    }
    void null_value(const std::string_view field_name) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        emit(field_name, "null");
    }
    // 1.6: maps as {key: value, …}, sets as {a, b, …}; a container argument's record is the value alone
    void begin_map(const std::string_view field_name, std::uint32_t) override
    {
        not_chars();
        ctx c;
        c.is_array = c.is_map = true;
        c.name = std::string(field_name);
        stack.push_back(std::move(c));
    }
    void begin_entry(std::uint32_t) override
    {
        ctx c;
        c.is_entry = true;
        stack.push_back(std::move(c));
    }
    void end_entry() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        emit({}, c.key + ": " + c.value);
    }
    void end_map() override
    {
        end_braced();
    }
    void begin_set(const std::string_view field_name, std::uint32_t) override
    {
        not_chars();
        ctx c;
        c.is_array = c.is_set = true;
        c.name = std::string(field_name);
        stack.push_back(std::move(c));
    }
    void end_set() override
    {
        end_braced();
    }
    void end_braced()
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        std::string s = "{";
        const std::size_t n = full ? c.items.size() : std::min<std::size_t>(c.items.size(), limit);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i)
                s += ", ";
            s += c.items[i];
        }
        if (n < c.items.size())
            s += ", … (+" + dec(c.items.size() - n) + " more)";
        emit(c.name, s + "}");
    }
    void begin_transparent(std::string_view, std::string_view) override
    {
    }
    void end_transparent() override
    {
    }
};

// ---- JSON visitor ----------------------------------------------------------------------------------------
struct json_visitor final : decoded_value_visitor
{
    struct ctx
    {
        bool is_array{};
        std::string name, buf, chars;
        std::size_t n{};
        bool all_chars{true};
    };
    std::string root;
    std::vector<ctx> stack;

    void emit(const std::string_view name, const std::string& json)
    {
        if (stack.empty())
        {
            root = json;
            return;
        }
        ctx& t = stack.back();
        if (t.n++)
            t.buf += ',';
        if (!t.is_array)
        {
            append_quoted(t.buf, name);
            t.buf += ':';
        }
        t.buf += json;
    }
    void not_chars()
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
    }
    void begin_struct(std::string_view, const std::string_view field_name) override
    {
        not_chars();
        ctx c;
        c.name = std::string(field_name);
        c.buf = "{";
        stack.push_back(std::move(c));
    }
    void end_struct() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        emit(c.name, c.buf + "}");
    }
    void begin_array(const std::string_view field_name, std::uint32_t) override
    {
        not_chars();
        ctx c;
        c.is_array = true;
        c.name = std::string(field_name);
        c.buf = "[";
        stack.push_back(std::move(c));
    }
    void end_array() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        if (c.all_chars && c.n)
        {
            std::string s;
            append_quoted(s, c.chars);
            emit(c.name, s);
            return;
        }
        emit(c.name, c.buf + "]");
    }
    void scalar(const std::string_view field_name, const kind k,
                const std::span<const std::byte> bytes) override
    {
        if (!stack.empty() && stack.back().is_array)
        {
            ctx& a = stack.back();
            if (is_char_kind(k) && bytes.size() == 1)
                a.chars += static_cast<char>(bytes[0]);
            else
                a.all_chars = false;
        }
        emit(field_name, scalar_number(k, bytes, true));
    }
    void enumerator(const std::string_view field_name, std::string_view,
                    const std::string_view name, const std::int64_t value) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        // Enumerator values that a double cannot hold exactly (64-bit underlying types) are quoted like i64/u64.
        std::string s = "{\"name\":";
        append_quoted(s, name);
        s += ",\"value\":" + json_int64(dec(value), !fits_double_exactly(value)) + "}";
        emit(field_name, s);
    }
    void literal(const std::string_view field_name, const std::uint64_t id,
                 const std::string_view text) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        std::string s = "{\"lit\":";
        if (text.empty())
            s += "null";
        else
            append_quoted(s, text);
        s += ",\"id\":\"" + hex64(id) + "\"}";
        emit(field_name, s);
    }
    // v2
    void string(const std::string_view field_name, const std::string_view s) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        std::string q;
        append_quoted(q, s);
        emit(field_name, q);
    }
    void null_value(const std::string_view field_name) override
    {
        if (!stack.empty() && stack.back().is_array)
            stack.back().all_chars = false;
        emit(field_name, "null");
    }
    void begin_variant(const std::string_view field_name, const std::uint32_t index,
                       const std::string_view alt) override
    { // {"alt":i,"type":"…","value":…}
        ctx c;
        c.name = std::string(field_name);
        c.buf = "{\"alt\":" + dec(index) + ",\"type\":";
        const std::size_t colon = alt.find(':');
        append_quoted(c.buf, colon == std::string_view::npos
                                 ? alt
                                 : alt.substr(colon + 1, alt.size() - colon - 2));
        c.n = 1; // emit() separates the value with a comma
        not_chars();
        stack.push_back(std::move(c));
    }
    void end_variant() override
    {
        ctx c = std::move(stack.back());
        stack.pop_back();
        emit(c.name, c.buf + "}");
    }
    // 1.6: a map is an array of {"key":…,"value":…} objects and a set an array (the defaults); a container
    // argument's record is the value alone
    void begin_transparent(std::string_view, std::string_view) override
    {
    }
    void end_transparent() override
    {
    }
};

// ---- dictionary text form -------------------------------------------------------------------------------
std::string kind_text(const dyn::field& f, const dyn::dictionary& d)
{
    if (f.k == kind::array_)
        return "array<" + (f.elem.empty() ? std::string("?") : kind_text(f.elem[0], d)) + "," +
               dec(f.extent) + ">";
    if (f.k == kind::vector_ || f.k == kind::optional_) // v2
        return std::string(f.k == kind::vector_ ? "vector<" : "optional<") +
               (f.elem.empty() ? std::string("?") : kind_text(f.elem[0], d)) + ">";
    if (f.k == kind::variant_)
    {
        std::string s = "variant<";
        for (std::size_t i = 0; i < f.elem.size(); ++i)
            s += (i ? "|" : "") + kind_text(f.elem[i], d);
        return s + ">";
    }
    if (f.k == kind::map_) // 1.6
        return "map<" +
               (f.elem.size() == 2 ? kind_text(f.elem[0], d) + "," + kind_text(f.elem[1], d)
                                   : std::string("?")) +
               ">";
    if (f.k == kind::set_)
        return "set<" + (f.elem.empty() ? std::string("?") : kind_text(f.elem[0], d)) + ">";
    if (f.k == kind::struct_ || f.k == kind::enum_)
    {
        const dyn::type* const t = d.find_type(f.type_hash);
        return std::string(to_string(f.k)) + "<" + (t ? t->name : hex64(f.type_hash)) + ">";
    }
    return std::string(to_string(f.k));
}

// 2.2.5: pahole-style layout accounting for flat types. `payload_bytes` is the number of bytes of a field (or a
// whole type) that carry information — leaf scalars, enums and bools, recursing through nested flat structs and
// arrays of them — so size - payload is padding: alignment holes between members, tail padding, and the holes
// inside nested members. Dynamic kinds (vector, string, map …) have no fixed image; their fixed part is counted
// as payload, which keeps the figure conservative. `depth` guards against a malformed dictionary that nests a
// type inside itself.
std::uint64_t payload_bytes(const dyn::type& t, const dyn::dictionary& d, int depth);
std::uint64_t payload_bytes(const dyn::field& f, const dyn::dictionary& d, const int depth)
{
    if (depth > 64)
        return f.size;
    if (f.k == kind::array_ && !f.elem.empty())
        return std::uint64_t{f.extent} * payload_bytes(f.elem[0], d, depth + 1);
    if (f.k == kind::struct_)
        if (const dyn::type* const t = d.find_type(f.type_hash); t && t->is_flat && !t->is_enum)
            return payload_bytes(*t, d, depth + 1);
    return f.size;
}
std::uint64_t payload_bytes(const dyn::type& t, const dyn::dictionary& d, const int depth)
{
    std::uint64_t n = 0;
    for (const dyn::field& f : t.fields)
        n += payload_bytes(f, d, depth);
    return n;
}
std::string percent(const std::uint64_t part, const std::uint64_t whole)
{
    if (whole == 0)
        return "0%";
    const std::uint64_t tenths = (part * 1000 + whole / 2) / whole;
    return dec(tenths / 10) + "." + dec(tenths % 10) + "%";
}

void dict_text(const dyn::dictionary& d, std::string& out)
{
    std::uint64_t total_size = 0, total_payload = 0, flat_types = 0;
    out += "dictionary  program_hash " + hex64(d.program_hash) + "  (" + dec(d.types.size()) +
           " types, " + dec(d.literals.size()) + " literals, " + dec(d.calls.size()) + " calls)\n";
    if (!d.same_abi())
        out += "abi         " + internal::abi_text(d.abi) +
               (d.abi_mixed ? "  (MIXED: sources with different ABIs)"
                            : "  (differs from this host: ") +
               (d.abi_mixed ? std::string() : internal::abi_text(fmt::host_abi_tag) + ")") + "\n";
    for (const auto& [h, t] : d.types)
    {
        out += t.is_enum ? "enum " : "struct ";
        out += t.name;
        if (t.is_enum)
            out += " : " + std::string(to_string(t.underlying));
        out += "  schema " + hex64(h) + "  layout " + hex64(t.layout_hash) + "  size " +
               dec(t.size) + "  align " + dec(t.align);
        if (!t.is_enum && t.is_flat)
            out += "  flat";
        else if (!t.is_enum)
            out += "  dynamic"; // v2: canonical image; size/align are the fixed part's
        if (t.transparent)
            out += "  transparent"; // 1.6: a container passed as an argument; renders as its value
        if (t.is_enum && !t.enum_fixed)
            out +=
                "  unfixed"; // 1.6: no fixed underlying type (decision D: kind from the enumerators)
        if (t.since)
            out += "  since " + dec(t.since);
        if (t.deprecated)
            out += "  deprecated " + dec(t.deprecated);
        out += '\n';
        for (const auto& [name, value] : t.enumerators)
            out += "  " + name + " = " + dec(value) + "\n";
        // Flat structs get pahole's accounting: a hole line after a member when the next one does not start
        // where it ends, and a summary of size / payload / holes / padding (tail padding and holes inside nested
        // members included). Transparent wrappers and dynamic types are not images, so they get none.
        const bool account = !t.is_enum && t.is_flat && !t.transparent && !t.fields.empty();
        std::uint32_t holes = 0, hole_bytes = 0, cursor = 0;
        for (std::size_t i = 0; i < t.fields.size(); ++i)
        {
            const auto& f = t.fields[i];
            out += "  " + f.name + ": " + kind_text(f, d) + " @" + dec(f.offset) + " [" +
                   dec(f.size) + "," + dec(f.align) + "]";
            if (f.since)
                out += " since " + dec(f.since);
            if (f.deprecated)
                out += " deprecated " + dec(f.deprecated);
            out += '\n';
            if (!account)
                continue;
            const std::uint32_t end = f.offset + f.size;
            cursor = end > cursor ? end : cursor;
            const std::uint32_t next = i + 1 < t.fields.size() ? t.fields[i + 1].offset : t.size;
            if (next > cursor)
            {
                const std::uint32_t gap = next - cursor;
                out += (i + 1 < t.fields.size() ? "  /* " : "  /* tail: ") + dec(gap) + " byte" +
                       (gap == 1 ? "" : "s") +
                       (i + 1 < t.fields.size() ? " hole */\n" : " padding */\n");
                if (i + 1 < t.fields.size())
                {
                    ++holes;
                    hole_bytes += gap;
                }
            }
        }
        if (account)
        {
            const std::uint64_t payload = payload_bytes(t, d, 0);
            const std::uint64_t padding = t.size > payload ? t.size - payload : 0;
            const std::uint32_t tail = t.size > cursor ? t.size - cursor : 0;
            const std::uint64_t nested =
                padding > hole_bytes + tail ? padding - hole_bytes - tail : 0;
            out += "  /* size " + dec(t.size) + ": payload " + dec(payload) + " (" +
                   percent(payload, t.size) + "), padding " + dec(padding) + " (" +
                   percent(padding, t.size) + ")";
            if (padding)
            {
                out += " = holes " + dec(holes) + "/" + dec(hole_bytes) + " B, tail " + dec(tail) +
                       " B";
                if (nested)
                    out += ", inside nested members " + dec(nested) + " B";
            }
            out += " */\n";
            total_size += t.size;
            total_payload += payload;
            ++flat_types;
        }
    }
    if (flat_types && total_size)
        out += "layout      " + dec(flat_types) +
               (flat_types == 1 ? " flat type, " : " flat types, ") + dec(total_size) +
               " bytes: payload " + dec(total_payload) + " (" + percent(total_payload, total_size) +
               "), padding " + dec(total_size - total_payload) + " (" +
               percent(total_size - total_payload, total_size) + ")\n";
    for (const auto& [id, l] : d.literals)
        out += "literal " + hex64(id) + " " + quote_chars(l.text) + "\n";
    for (const auto& [h, k] : d.calls)
    {
        // 1.1: print calls are labelled by their flags; a plain call prints exactly as in 1.0.
        out += (k.is_log()     ? "log "
                : k.is_print() ? (k.newline() ? "println " : "print ")
                               : "call ") +
               hex64(h) + "  (";
        for (std::size_t i = 0; i < k.args.size(); ++i)
        {
            if (i)
                out += ", ";
            const dyn::call_arg& a = k.args[i];
            if (a.is_literal)
                out += "lit";
            else if (a.ak == fmt::arg_const)
            { // 1.5: const i32=0x…2a / const corpus::Side=0x…01 (the value's bytes as a u64)
                const dyn::type* const t = kind_size(a.k) != 0 ? nullptr : d.find_type(a.hash);
                out += "const ";
                out += kind_size(a.k) != 0 ? std::string(to_string(a.k))
                       : t                 ? t->name
                                           : hex64(a.hash);
                out += "=" + hex64(a.value);
            }
            else if (a.ak == fmt::arg_scalar)
                out += to_string(a.k); // 1.1: the scalar kind name (i32, f64, ...)
            else if (a.ak == fmt::arg_string)
                out += "string"; // 1.1
            else
            {
                const dyn::type* const t = d.find_type(a.hash);
                out += t ? t->name : hex64(a.hash);
            }
        }
        out += ")\n";
    }
    for (const auto& c :
         d.conflicts) // cc F15/F16: both definitions, so the reader can see what was dropped
        out += "conflict " + hex64(c.kept.schema_hash) + "  kept " + c.kept.name + " (layout " +
               hex64(c.kept.layout_hash) + ", size " + dec(c.kept.size) + ")  dropped " +
               c.dropped.name + " (layout " + hex64(c.dropped.layout_hash) + ", size " +
               dec(c.dropped.size) + ")\n";
}

void hex_lines(std::string& out, const std::span<const std::byte> b, const std::string& indent)
{
    for (std::size_t i = 0; i < b.size(); i += 16)
        out += indent + "hex " + dec(i) + ": " +
               raw_hex(b.subspan(i, std::min<std::size_t>(16, b.size() - i)), " ") + "\n";
}

// ---- print rendering (1.1, FR-P4) ----------------------------------------------------------------------
// Value of an enum record read through its underlying kind; nullopt when the payload size does not fit.
std::optional<std::int64_t> enum_value(const kind underlying, const std::span<const std::byte> b)
{
    if (b.size() != kind_size(underlying) || b.empty())
        return std::nullopt;
    switch (underlying)
    {
    case kind::i8:
        return load<std::int8_t>(b);
    case kind::i16:
        return load<std::int16_t>(b);
    case kind::i32:
        return load<std::int32_t>(b);
    case kind::i64:
        return load<std::int64_t>(b);
    case kind::u8:
    case kind::boolean:
    case kind::char_:
    case kind::char8:
    case kind::byte_:
        return load<std::uint8_t>(b);
    case kind::u16:
    case kind::char16:
        return load<std::uint16_t>(b);
    case kind::u32:
    case kind::char32:
    case kind::wchar:
        return load<std::uint32_t>(b);
    case kind::u64:
        return static_cast<std::int64_t>(load<std::uint64_t>(b));
    default:
        return std::nullopt;
    }
}

// One-line text form of a struct record: `Type{field: value, ...}` — the text_visitor's inline rendering,
// obtained by starting it inside a pseudo array context so the record is an "item" rather than a block.
result<std::string> struct_inline_text(const dyn::dictionary& d, const internal::record_info& r)
{
    std::string scratch;
    text_visitor v(scratch, {}, {}, UINT32_MAX, true);
    text_visitor::ctx root;
    root.is_array = true;
    v.stack.push_back(std::move(root));
    if (const auto res = visit_record(r.payload, *r.type, d, v); !res)
        return std::unexpected(res.error());
    if (v.stack.size() != 1 || v.stack.back().items.empty())
        return std::unexpected(error::length_mismatch);
    return std::move(v.stack.back().items.front());
}

// The text an argument is rendered as when std::format's argument type is std::string_view (literal,
// string, enum, struct, unknown type): what the writer's compile-time check assumed for these categories.
result<std::string> arg_as_text(const dyn::dictionary& d, const internal::record_info& r)
{
    if (r.ak == fmt::arg_string)
        return std::string(r.text());
    if (r.is_literal)
    {
        const dyn::literal* const l = d.find_literal(r.literal_id);
        return l ? l->text : hex64(r.literal_id);
    }
    if (!r.type)
        return "<" + hex64(r.schema_hash) + ">";
    if (r.type->is_enum)
    {
        const auto v = enum_value(r.type->underlying, r.payload);
        if (!v)
            return std::unexpected(error::length_mismatch);
        for (const auto& [name, value] : r.type->enumerators)
            if (value == *v)
                return name;
        return dec(*v);
    }
    return struct_inline_text(d, r);
}

// Formats one placeholder: `{:<spec>}` applied to the record, whose std::format argument type is chosen
// at run time from its category/kind. std::vformat reports a bad spec by throwing; that becomes
// error::invalid_argument here and nothing escapes the library (AGENTS.md §4).
result<std::string> render_arg(const dyn::dictionary& d, const internal::record_info& r,
                               const std::string_view spec)
{
    const std::string fs = spec.empty() ? std::string("{}") : "{:" + std::string(spec) + "}";
    try
    {
        if (r.ak == fmt::arg_scalar)
        {
            const auto b = r.payload;
            if (b.size() != d.scalar_size(r.k))
                return std::unexpected(error::length_mismatch);
            switch (r.k)
            {
            case kind::i8:
            case kind::i16:
            case kind::i32:
            case kind::i64: {
                const long long v = r.k == kind::i8    ? load<std::int8_t>(b)
                                    : r.k == kind::i16 ? load<std::int16_t>(b)
                                    : r.k == kind::i32 ? load<std::int32_t>(b)
                                                       : load<std::int64_t>(b);
                return std::vformat(fs, std::make_format_args(v));
            }
            // 2.1.12 (review T-04): a float stays a float ("{}" of 0.1f is 0.1, not 0.10000000149…)
            case kind::f32: {
                const float v = load<float>(b);
                return std::vformat(fs, std::make_format_args(v));
            }
            case kind::f64: {
                const double v = load<double>(b);
                return std::vformat(fs, std::make_format_args(v));
            }
            case kind::boolean: {
                const bool v = load<std::uint8_t>(b) != 0;
                return std::vformat(fs, std::make_format_args(v));
            }
            case kind::char_: {
                const char v = static_cast<char>(b[0]);
                return std::vformat(fs, std::make_format_args(v));
            }
            default: { // u8..u64, and byte/char8/char16/char32/wchar as unsigned numbers
                const unsigned long long v = b.size() == 1   ? load<std::uint8_t>(b)
                                             : b.size() == 2 ? load<std::uint16_t>(b)
                                             : b.size() == 4 ? load<std::uint32_t>(b)
                                                             : load<std::uint64_t>(b);
                return std::vformat(fs, std::make_format_args(v));
            }
            }
        }
        const auto text = arg_as_text(d, r);
        if (!text)
            return std::unexpected(text.error());
        const std::string_view sv = *text;
        return std::vformat(fs, std::make_format_args(sv));
    }
    catch (...)
    {
        return std::unexpected(error::invalid_argument);
    }
}

std::string render_problem(const internal::frame_info& f, const error e)
{
    return "frame " + dec(f.index) + " @" + dec(f.offset) + ": print: " + std::string(to_string(e));
}

} // namespace
} // namespace es::serreg::tools

namespace es::serreg::internal
{

// 2.2.1 (1.7): the fixed envelope positions of a log call — level, file, line, mask, time, [thread], [sequence],
// format — mirror log.hpp's log_layout.
result<log_view> decode_log(const dyn::dictionary& d, const dyn::call& k,
                            const std::span<const record_info> records)
{
    static_cast<void>(d);
    log_view v;
    if (!k.is_log() || records.size() != k.args.size() || records.size() < 6)
        return std::unexpected(error::invalid_argument);
    const auto u8_of = [](const record_info& r) {
        return r.payload.empty() ? std::uint8_t{0} : static_cast<std::uint8_t>(r.payload[0]);
    };
    const auto u32_of = [](const record_info& r) {
        std::uint32_t x{};
        std::memcpy(&x, r.payload.data(), std::min<std::size_t>(4, r.payload.size()));
        return x;
    };
    const auto u64_of = [](const record_info& r) {
        std::uint64_t x{};
        std::memcpy(&x, r.payload.data(), std::min<std::size_t>(8, r.payload.size()));
        return x;
    };
    v.level = u8_of(records[0]);
    if (!records[1].is_literal)
        return std::unexpected(error::invalid_argument);
    if (const dyn::literal* const l = d.find_literal(records[1].literal_id))
        v.file = l->text;
    v.line = u32_of(records[2]);
    v.mask = u8_of(records[3]);
    v.time = u64_of(records[4]);
    std::size_t at = 5;
    if (v.mask & 1)
    {
        if (at >= records.size())
            return std::unexpected(error::invalid_argument);
        v.thread = u32_of(records[at++]);
        v.has_thread = true;
    }
    if (v.mask & 2)
    {
        if (at >= records.size())
            return std::unexpected(error::invalid_argument);
        v.sequence = u64_of(records[at++]);
        v.has_sequence = true;
    }
    if (at >= records.size() || !records[at].is_literal)
        return std::unexpected(error::invalid_argument);
    v.format_index = at;
    return v;
}
std::string level_name(const std::uint8_t level)
{
    static constexpr const char* names[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    return level < 6 ? names[level] : dec(level);
}
std::string time_text(const log_view& v)
{
    if (((v.mask >> 2) & 3) != 0)
        return dec(v.time); // steady, tsc or a user clock: the number is the only honest rendering
    const std::chrono::sys_time<std::chrono::nanoseconds> tp{
        std::chrono::nanoseconds{static_cast<std::int64_t>(v.time)}};
    return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
}

result<std::string> render_message(const dyn::dictionary& d, const dyn::call& k,
                                   const std::span<const record_info> records, const bool envelope)
{
    if (!k.is_print() || records.empty() || records.size() != k.args.size())
        return std::unexpected(error::invalid_argument);
    std::size_t format_at = 0;
    std::string out;
    if (k.is_log())
    { // 2.2.1 (1.7): the envelope first — "<time> <LEVEL> <file>:<line> [thread] #seq " (JSON: fields instead)
        const auto v = decode_log(d, k, records);
        if (!v)
            return std::unexpected(v.error());
        format_at = v->format_index;
        if (envelope)
        {
            std::string lvl = level_name(v->level);
            for (char& c : lvl)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            out += time_text(*v) + " " + lvl;
            out.append(lvl.size() < 5 ? 5 - lvl.size() + 1 : 1, ' ');
            if (v->file != "-")
            {
                const std::size_t slash = v->file.rfind('/');
                out += std::string(slash == std::string_view::npos ? v->file
                                                                   : v->file.substr(slash + 1)) +
                       ":" + dec(v->line) + " ";
            }
            if (v->has_thread)
                out += "[" + dec(v->thread) + "] ";
            if (v->has_sequence)
                out += "#" + dec(v->sequence) + " ";
        }
    }
    if (!records[format_at].is_literal)
        return std::unexpected(error::invalid_argument);
    const dyn::literal* const l = d.find_literal(records[format_at].literal_id);
    if (!l)
        return std::unexpected(error::unknown_literal);
    const std::string_view f = l->text;
    const auto args = records.subspan(format_at + 1);
    std::size_t next = 0; // next automatic index
    enum : std::uint8_t
    {
        unset,
        automatic,
        manual
    } indexing = unset;
    for (std::size_t i = 0; i < f.size();)
    {
        const char c = f[i];
        if (c == '}')
        {
            if (i + 1 < f.size() && f[i + 1] == '}')
            {
                out += '}';
                i += 2;
                continue;
            }
            return std::unexpected(error::invalid_argument); // lone '}'
        }
        if (c != '{')
        {
            out += c;
            ++i;
            continue;
        }
        if (i + 1 < f.size() && f[i + 1] == '{')
        {
            out += '{';
            i += 2;
            continue;
        }
        std::size_t j = i + 1, idx = 0;
        bool has_idx = false;
        for (; j < f.size() && f[j] >= '0' && f[j] <= '9'; ++j)
        {
            if (idx > 0xffff)
                return std::unexpected(error::invalid_argument);
            idx = idx * 10 + static_cast<std::size_t>(f[j] - '0');
            has_idx = true;
        }
        std::string_view spec;
        if (j < f.size() && f[j] == ':')
        {
            const std::size_t s = j + 1;
            std::size_t e = s;
            while (e < f.size() && f[e] != '}' && f[e] != '{')
                ++e;
            if (e >= f.size() || f[e] == '{') // nested {} width/precision fields are not supported
                return std::unexpected(error::invalid_argument);
            spec = f.substr(s, e - s);
            j = e;
        }
        if (j >= f.size() || f[j] != '}')
            return std::unexpected(error::invalid_argument);
        if (has_idx ? indexing == automatic : indexing == manual)
            return std::unexpected(error::invalid_argument); // std::format forbids mixing
        indexing = has_idx ? manual : automatic;
        if (!has_idx)
            idx = next++;
        if (idx >= args.size())
            return std::unexpected(error::length_mismatch);
        const auto piece = tools::render_arg(d, args[idx], spec);
        if (!piece)
            return std::unexpected(piece.error());
        out += *piece;
        i = j + 1;
    }
    if (k.newline())
        out += '\n';
    return out;
}

} // namespace es::serreg::internal

namespace es::serreg::tools
{

result<std::string> render_frame(const std::span<const std::byte> frame, const dyn::dictionary& d)
{
    const auto h = read_frame_header(frame);
    if (!h)
        return std::unexpected(h.error());
    if (h->dictionary())
        return std::unexpected(error::invalid_argument);
    std::vector<internal::record_info> recs;
    const auto w = internal::for_each_record(
        frame.first(h->frame_len), &d, true,
        [](const dyn::dictionary&, const internal::frame_info&) { return true; },
        [&](const dyn::dictionary&, const internal::frame_info&, const internal::record_info& r) {
            recs.push_back(r);
            return true;
        });
    if (!w)
        return std::unexpected(w.error());
    if (w->first_error)
        return std::unexpected(*w->first_error);
    const dyn::call* const k = w->dict.find_call(h->call_hash);
    if (!k)
        return std::unexpected(error::unknown_call);
    if (!k->is_print())
        return std::unexpected(error::invalid_argument);
    return internal::render_message(w->dict, *k, recs);
}

result<log_record> render_log(const std::span<const std::byte> frame, const dyn::dictionary& d)
{
    const auto h = read_frame_header(frame);
    if (!h)
        return std::unexpected(h.error());
    if (h->dictionary())
        return std::unexpected(error::invalid_argument);
    std::vector<internal::record_info> recs;
    const auto w = internal::for_each_record(
        frame.first(h->frame_len), &d, true,
        [](const dyn::dictionary&, const internal::frame_info&) { return true; },
        [&](const dyn::dictionary&, const internal::frame_info&, const internal::record_info& r) {
            recs.push_back(r);
            return true;
        });
    if (!w)
        return std::unexpected(w.error());
    if (w->first_error)
        return std::unexpected(*w->first_error);
    const dyn::call* const k = w->dict.find_call(h->call_hash);
    if (!k)
        return std::unexpected(error::unknown_call);
    if (!k->is_log())
        return std::unexpected(error::invalid_argument);
    const auto v = internal::decode_log(w->dict, *k, recs);
    if (!v)
        return std::unexpected(v.error());
    auto text = internal::render_message(w->dict, *k, recs, false);
    if (!text)
        return std::unexpected(text.error());
    if (text->ends_with('\n'))
        text->pop_back();
    log_record r;
    r.level = v->level;
    r.file = std::string(v->file);
    r.line = v->line;
    r.mask = v->mask;
    r.time_ns = v->time;
    r.thread = v->thread;
    r.sequence = v->sequence;
    r.has_thread = v->has_thread;
    r.has_sequence = v->has_sequence;
    if (const dyn::literal* const l = w->dict.find_literal(recs[v->format_index].literal_id))
        r.format = l->text;
    r.message = std::move(*text);
    return r;
}

result<void> dump_dictionary(const dyn::dictionary& d, const format fmt, std::string& out)
{
    switch (fmt)
    {
    case format::text:
        dict_text(d, out);
        break;
    case format::json:
        out += d.to_json();
        break;
    case format::toml:
        out += d.to_toml();
        break;
    }
    return {};
}

// Streaming dump (cc F19). Two structural passes: the dictionary is printed first and is "as of the end of the
// capture" (dictionary frames may appear anywhere), so pass 1 walks frame headers without decoding a record to
// collect it; pass 2 decodes and flushes one frame at a time through `out`. Decode errors are problems, not stops.
namespace
{
// dump over either source (2.1.16, A6): the span walker, or the windowed file walker.
result<void> dump_source(const internal::record_source& src, const dump_options& opt,
                         const write_fn out, void* const ctx, const std::size_t problem_limit)
{
    const bool json = opt.fmt != format::text; // records are JSON for --json and --toml
    const bool filtered = opt.type_filter != 0 || !opt.type_name_filter.empty();
    const auto nop_frame = [](const dyn::dictionary&, const internal::frame_info&) { return true; };
    const auto nop_record = [](const dyn::dictionary&, const internal::frame_info&,
                               const internal::record_info&) { return true; };

    // 2.2.6 (--verbose): the dictionary blocks — the opening block and every dictionary frame — with what each
    // added. Pass 1 sees the dictionary "as of" each frame, so a block's contribution is the growth since the
    // previous callback. The windowed source delivers meta frames only when asked (block_options::meta_frames).
    struct dict_block
    {
        std::uint64_t index{}, offset{};
        bool opening{};
        std::size_t types{}, literals{}, calls{};
    };
    std::vector<dict_block> blocks;
    std::size_t seen_types = 0, seen_literals = 0, seen_calls = 0;
    const auto note_block = [&](const dyn::dictionary& d, const internal::frame_info& f) {
        if (f.is_dict)
            blocks.push_back({f.index, f.offset, f.is_opening, d.types.size() - seen_types,
                              d.literals.size() - seen_literals, d.calls.size() - seen_calls});
        seen_types = d.types.size();
        seen_literals = d.literals.size();
        seen_calls = d.calls.size();
        return true;
    };
    const auto block_line = [](const dict_block& b, const bool as_json) {
        if (as_json)
            return std::string("{\"frame\":") + (b.opening ? "null" : dec(b.index)) +
                   ",\"offset\":" + dec(b.offset) + ",\"dictionary\":{\"block\":\"" +
                   (b.opening ? "opening" : "frame") + "\",\"types\":" + dec(b.types) +
                   ",\"literals\":" + dec(b.literals) + ",\"calls\":" + dec(b.calls) + "}}\n";
        return (b.opening ? "  opening block" : "  frame " + dec(b.index) + "  dictionary frame") +
               "  @" + dec(b.offset) + "  +" + dec(b.types) + " types, +" + dec(b.literals) +
               " literals, +" + dec(b.calls) + " calls\n";
    };

    std::string head;
    const auto pre =
        src(false, opt.verbose ? internal::frame_fn(note_block) : nop_frame, nop_record);
    if (!pre)
        return std::unexpected(pre.error());
    if (!opt.logs_only)
    { // 2.2.1: --level is a log view — the lines only (JSON: one object per line), no dictionary
        if (const auto r = dump_dictionary(pre->dict, opt.fmt, head); !r)
            return r;
        if (opt.verbose)
        { // 2.2.6: the blocks, after the merged dictionary (text: a short table; JSON: one object per block)
            if (!json)
                head += "dictionary blocks  " + dec(blocks.size()) + "\n";
            for (const auto& b : blocks)
                head += block_line(b, json);
        }
        if (!out(head, ctx))
            return std::unexpected(error::io_error);
        if (opt.dict_only)
            return pre->dict.conflicts.empty()
                       ? result<void>{}
                       : std::unexpected(error::schema_conflict); // cc F18: no record walk
        if (!json && !out("\n", ctx))
            return std::unexpected(error::io_error);
    }

    std::string records, pending_header;
    std::vector<std::string> decode_problems;
    std::optional<error>
        first_decode; // G2-10 (2.1.3): dump returns the walker's own code, not length_mismatch
    bool io_failed = false;
    // 1.1 print frames: records are collected and rendered when the frame is complete (FR-P5).
    const dyn::call* print_call = nullptr;
    internal::frame_info print_frame{};
    std::vector<internal::record_info> print_recs;
    // 2.1.16 (A6): over a windowed source a record's bytes are valid only during the callback, and a print frame
    // is rendered when the next frame starts — so the records of a print frame are copied (print arguments are
    // scalars, strings and small values; the copies live until the frame is rendered).
    std::vector<std::vector<std::byte>> print_bytes;
    bool skip_frame = false; // 2.2.1: a log frame the level filter excluded
    const auto finish_print = [&](const dyn::dictionary& d) {
        if (!print_call)
            return;
        std::optional<internal::log_view> lv;
        if (print_call->is_log())
        { // 2.2.1 (1.7): the time filter needs the frame's records; a frame outside the range leaves no trace
            if (const auto v = internal::decode_log(d, *print_call, print_recs))
            {
                lv = *v;
                if (v->time < opt.since || v->time > opt.until)
                {
                    print_call = nullptr;
                    print_recs.clear();
                    print_bytes.clear();
                    return;
                }
            }
        }
        const auto msg = internal::render_message(d, *print_call, print_recs, !(json && lv));
        if (lv && !json)
        { // the log frame passed the filters: its header (unless --level: bare lines), then the line
            records += pending_header;
            pending_header.clear();
        }
        if (msg)
        {
            if (lv && !json && opt.logs_only)
                records += *msg; // bare: one rendered line per frame
            else if (json && lv)
            {
                std::string_view m = *msg;
                if (m.ends_with('\n'))
                    m.remove_suffix(1);
                records += "{\"frame\":" + dec(print_frame.index) + ",\"log\":{\"level\":\"" +
                           internal::level_name(lv->level) + "\",\"time\":" + dec(lv->time) +
                           ",\"file\":";
                append_quoted(records, std::string(lv->file));
                records += ",\"line\":" + dec(lv->line);
                if (lv->has_thread)
                    records += ",\"thread\":" + dec(lv->thread);
                if (lv->has_sequence)
                    records += ",\"sequence\":" + dec(lv->sequence);
                records += ",\"message\":";
                append_quoted(records, std::string(m));
                records += "}}\n";
            }
            else if (json)
            {
                records += "{\"frame\":" + dec(print_frame.index) + ",\"message\":";
                append_quoted(records, *msg);
                records += "}\n";
            }
            else
            {
                std::string_view m = *msg;
                if (m.ends_with('\n'))
                    m.remove_suffix(1);
                records += lv ? "  log: " : "  print: ";
                records += m;
                records += "\n";
            }
        }
        else
        {
            if (!first_decode)
                first_decode = msg.error();
            decode_problems.push_back(render_problem(print_frame, msg.error()));
        }
        print_call = nullptr;
        print_recs.clear();
        print_bytes.clear();
    };
    const auto flush = [&] {
        if (records.empty())
            return true;
        const bool ok = out(records, ctx);
        records.clear();
        if (!ok)
            io_failed = true;
        return ok;
    };
    const auto in_range = [&](const internal::frame_info& f) {
        return f.index >= opt.first_frame && f.index <= opt.last_frame;
    };

    const auto on_frame = [&](const dyn::dictionary& d, const internal::frame_info& f) {
        finish_print(d);
        pending_header.clear();
        if (!flush())
            return false;
        if (f.index > opt.last_frame)
            return false; // cc F18: nothing after the range is walked
        if (!in_range(f))
            return true;
        if (f.is_dict || f.is_checkpoint || f.is_info)
        { // meta frames count in the numbering and are not shown (2.2.1: the file reader never showed them;
            // --info lists the dictionary blocks and checkpoints) — except 2.2.6's --verbose marker for a
            // dictionary frame: what it added, from the pass-1 blocks
            if (opt.verbose && f.is_dict && !f.is_opening && !opt.logs_only)
                for (const auto& b : blocks)
                    if (b.index == f.index && !b.opening)
                    {
                        records += block_line(b, json);
                        break;
                    }
            return flush();
        }
        skip_frame = false;
        const dyn::call* const k = d.find_call(f.call_hash);
        if (opt.logs_only)
        { // 2.2.1 (1.7): only log frames, at or above the level (the level is a constant of the call entry)
            if (!k || !k->is_log() || k->args.empty() || k->args[0].value < opt.min_level)
            {
                skip_frame = true;
                return true;
            }
        }
        if (k && k->is_print() && !filtered)
        {
            print_call = k;
            print_frame = f;
        }
        if (!json && !opt.logs_only)
        {
            pending_header = "frame " + dec(f.index) + "  " +
                             (print_call ? (print_call->is_log()    ? "log "
                                            : print_call->newline() ? "println "
                                                                    : "print ")
                                         : "call ") +
                             hex64(f.call_hash) + "  (" + dec(f.n_records) + " records" +
                             (f.is_full ? ", full" : "") + ")\n";
            // a log frame's header waits for the time filter (finish_print); every other frame's goes out now
            if (!filtered && !(print_call && print_call->is_log()))
            {
                records += pending_header;
                pending_header.clear();
            }
        }
        return true;
    };

    const auto on_record = [&](const dyn::dictionary& d, const internal::frame_info& f,
                               const internal::record_info& r) {
        if (!in_range(f) || skip_frame)
            return true;
        if (filtered)
        {
            if (r.is_literal || !r.type)
                return true;
            if (opt.type_filter && r.type->schema_hash != opt.type_filter)
                return true;
            if (!opt.type_name_filter.empty() && r.type->name != opt.type_name_filter)
                return true;
        }
        records += pending_header;
        pending_header.clear();
        if (print_call)
        {
            print_recs.push_back(r);
            print_bytes.emplace_back(r.payload.begin(), r.payload.end());
            print_recs.back().payload = print_bytes.back();
            if ((!json && !opt.raw) || print_call->is_log())
                return true; // text: the rendered message replaces the records; a log frame: always rendered
        }
        const std::string prefix = "  [" + dec(r.index) + "] ";
        if (json)
        {
            records += "{\"frame\":" + dec(f.index) + ",\"record\":" + dec(r.index) + ",\"type\":";
            json_visitor v;
            if (r.ak == fmt::arg_scalar)
            { // 1.1
                records += "\"" + std::string(to_string(r.k)) +
                           "\",\"value\":" + scalar_number(r.k, r.payload, true);
            }
            else if (r.ak == fmt::arg_string)
            { // 1.1
                records += "\"string\",\"value\":";
                append_quoted(records, std::string(r.text()));
            }
            else if (r.is_literal)
            {
                const dyn::literal* const l = d.find_literal(r.literal_id);
                v.literal({}, r.literal_id, l ? std::string_view(l->text) : std::string_view{});
                records += "\"literal\",\"value\":" + v.root;
            }
            else if (!r.type)
            {
                records += "null,\"schema\":\"" + hex64(r.schema_hash) + "\"";
            }
            else
            {
                append_quoted(records, r.type->name);
                if (const auto res = visit_record(r.payload, *r.type, d, v, {}, opt.flatten); !res)
                {
                    records += ",\"error\":\"" + std::string(to_string(res.error())) + "\"}\n";
                    if (!first_decode)
                        first_decode = res.error();
                    decode_problems.push_back(internal::decode_problem(f, r, res.error()));
                    return true;
                }
                records += ",\"value\":" + v.root;
            }
            if (opt.hex || (r.ak == fmt::arg_type && !r.type))
                records += ",\"hex\":\"" + raw_hex(r.payload, "") + "\"";
            records += "}\n";
            return true;
        }
        records += prefix;
        text_visitor v(records, "        ", "      ", opt.array_limit, opt.full_arrays);
        if (r.ak == fmt::arg_scalar) // 1.1
            records += std::string(to_string(r.k)) + " = " + scalar_text(r.k, r.payload) + "\n";
        else if (r.ak == fmt::arg_string) // 1.1
            records += "string = " + quote_chars(r.text()) + "\n";
        else if (r.is_literal)
        {
            const dyn::literal* const l = d.find_literal(r.literal_id);
            v.literal({}, r.literal_id, l ? std::string_view(l->text) : std::string_view{});
        }
        else if (!r.type)
        {
            records += "<unknown type " + hex64(r.schema_hash) + "> (" + dec(r.payload.size()) +
                       " bytes)\n";
        }
        else if (const auto res = visit_record(r.payload, *r.type, d, v, {}, opt.flatten); !res)
        {
            records += "\n      <decode error: " + std::string(to_string(res.error())) + ">\n";
            if (!first_decode)
                first_decode = res.error();
            decode_problems.push_back(internal::decode_problem(f, r, res.error()));
            return true;
        }
        if (opt.hex || (r.ak == fmt::arg_type && !r.type))
            hex_lines(records, r.payload, "        ");
        return true;
    };

    const auto w = src(true, on_frame, on_record);
    if (!w)
        return std::unexpected(w.error());
    finish_print(w->dict);
    if (io_failed || !flush())
        return std::unexpected(error::io_error);
    std::vector<std::string> problems = w->problems;
    std::size_t omitted = w->problems_omitted;
    for (auto& p : decode_problems)
        internal::add_problem(problems, omitted, problem_limit, std::move(p));
    std::string tail;
    for (const auto& p : problems)
    {
        if (json)
        {
            tail += "{\"problem\":";
            append_quoted(tail, p);
            tail += "}\n";
        }
        else
            tail += "problem: " + p + "\n";
    }
    if (omitted) // 2.1.16: beyond decode_limits::problems, counted only
        tail += json ? "{\"problems_omitted\":" + dec(omitted) + "}\n"
                     : "problem: ... and " + dec(omitted) + " more\n";
    if (!tail.empty() && !out(tail, ctx))
        return std::unexpected(error::io_error);
    if (w->first_error)
        return std::unexpected(*w->first_error);
    if (!decode_problems.empty())
        return std::unexpected(first_decode.value_or(error::length_mismatch));
    return {};
}

} // namespace

result<void> dump(const std::span<const std::byte> capture, const dyn::dictionary* const extra,
                  const dump_options& opt, const write_fn out, void* const ctx)
{
    return dump_source(
        [&](const bool stop, const internal::frame_fn& on_frame,
            const internal::record_fn& on_record) {
            return internal::for_each_record(capture, extra, stop, on_frame, on_record);
        },
        opt, out, ctx, get_decode_limits().problems);
}

result<void> dump(const std::span<const std::byte> capture, const dyn::dictionary* const extra,
                  const dump_options& opt, std::string& out)
{
    const auto append = [](const std::string_view chunk, void* const ctx) {
        static_cast<std::string*>(ctx)->append(chunk);
        return true;
    };
    return dump(capture, extra, opt, +append, &out);
}

// 2.1.16 (A6): over a file or "-", window by window; two passes over the file.
result<void> dump(const std::string_view path, const dyn::dictionary* const extra,
                  const dump_options& opt, const write_fn out, void* const ctx,
                  const block_options& o)
{
    const decode_limits limits = o.limits ? *o.limits : get_decode_limits();
    block_options bo = o;
    bo.meta_frames =
        bo.meta_frames || opt.verbose; // 2.2.6: the blocks and markers need the meta frames
    return dump_source(
        [&](const bool stop, const internal::frame_fn& on_frame,
            const internal::record_fn& on_record) {
            return internal::for_each_record_in_file(path, extra, stop, on_frame, on_record, bo);
        },
        opt, out, ctx, limits.problems);
}

result<void> dump(const std::string_view path, const dyn::dictionary* const extra,
                  const dump_options& opt, std::string& out, const block_options& o)
{
    const auto append = [](const std::string_view chunk, void* const ctx) {
        static_cast<std::string*>(ctx)->append(chunk);
        return true;
    };
    return dump(path, extra, opt, +append, &out, o);
}

// 2.1.10: the public face of the text formatter's scalar rendering (srdump's text output).
std::string value_text(const kind k, const std::span<const std::byte> bytes)
{
    return scalar_text(k, bytes);
}

// ---- 2.2.1: one value from its record payload (to_string / to_json) ----------------------------------------
result<std::string> render_value(const dyn::dictionary& d, const fmt::arg_kind ak, const kind k,
                                 const std::uint64_t schema, std::span<const std::byte> payload,
                                 const format f)
{
    const bool json = f != format::text;
    internal::record_info r;
    r.ak = ak;
    r.k = k;
    r.schema_hash = schema;
    r.payload = payload;
    switch (ak)
    {
    case fmt::arg_scalar:
        if (kind_size(k) == 0 || payload.size() != kind_size(k))
            return std::unexpected(error::length_mismatch);
        return json ? scalar_number(k, payload, true) : scalar_text(k, payload);
    case fmt::arg_string: {
        if (payload.size() < fmt::string_len_prefix)
            return std::unexpected(error::truncated);
        std::uint32_t len{};
        std::memcpy(&len, payload.data(), 4);
        if (len != payload.size() - fmt::string_len_prefix)
            return std::unexpected(error::length_mismatch);
        r.payload = payload.subspan(fmt::string_len_prefix);
        if (!json)
            return quote_chars(r.text());
        std::string out;
        append_quoted(out, std::string(r.text()));
        return out;
    }
    case fmt::arg_type:
        break;
    default:
        return std::unexpected(error::invalid_argument);
    }
    r.type = d.find_type(schema);
    if (!r.type)
        return std::unexpected(error::unknown_type);
    if (r.type->is_flat)
    {
        if (payload.size() != r.type->size)
            return std::unexpected(error::length_mismatch);
    }
    else
    { // a dynamic type's record: u32 len, u32 0, image
        if (payload.size() < fmt::image_prefix)
            return std::unexpected(error::truncated);
        std::uint32_t inner{};
        std::memcpy(&inner, payload.data(), 4);
        if (inner != payload.size() - fmt::image_prefix)
            return std::unexpected(error::length_mismatch);
        r.payload = payload.subspan(fmt::image_prefix);
    }
    if (!json)
        return arg_as_text(d, r);
    json_visitor v;
    if (const auto res = visit_record(r.payload, *r.type, d, v); !res)
        return std::unexpected(res.error());
    return v.root;
}

namespace
{
struct process_dictionary_cache
{
    std::shared_mutex mu;
    std::uint64_t version = 0; // the registry version the dictionary was parsed from (0: never)
    std::shared_ptr<const dyn::dictionary> dict;
};
process_dictionary_cache& the_process_dictionary()
{
    static process_dictionary_cache
        c; // function-local: no static-initialisation order to care about
    return c;
}
} // namespace

std::shared_ptr<const dyn::dictionary> process_dictionary()
{
    process_dictionary_cache& c = the_process_dictionary();
    const std::uint64_t now = registry::instance().version();
    {
        std::shared_lock lock(c.mu);
        if (c.version == now && c.dict)
            return c.dict;
    }
    std::unique_lock lock(c.mu);
    if (c.version != now || !c.dict)
    {
        auto fresh = std::make_shared<dyn::dictionary>();
        static_cast<void>(fresh->add_rri(registry::instance().export_image()));
        c.dict = std::move(fresh);
        c.version = now;
    }
    return c.dict;
}

} // namespace es::serreg::tools
