// src/cxxgen.cpp — tools::cxx_header: C++26 type definitions generated from a dictionary (2.1.16, plan A5;
// `srdump --cxx`).
//
// The output is one header that declares every enum and struct of a dyn::dictionary so that, compiled with this
// library, each regenerated type has exactly the schema hash and layout hash the dictionary records — and checks
// that itself with static_asserts at the end of the file. The dictionary model is the whole input: kinds become
// fixed-width scalars, the `elem` descriptors become std::array / std::vector / std::optional / std::variant,
// `<base:…>` pseudo-fields become base classes, since/deprecated annotations are carried, the std vocabulary
// (std::string, std::pair<…>, std::monostate, …) is spelled back and never defined, and a flat type's recorded
// offsets are reproduced with [[=es::serreg::skip]] padding members (absent from the canonical strings, scrubbed
// on the wire — exactly what a gap is). A name a plain declaration cannot spell (a template specialisation, a
// class nested in a class, a remapped namespace) is declared under a sanitized identifier with
// [[=es::serreg::name("<canonical>")]], which makes the library use the recorded name. With `router`, a
// frame_router skeleton with one handler per call signature follows (literals, constants from the ARG values and
// the print forms spelled as router.hpp accepts them). Same dictionary, same text: every container walked is
// ordered. Tools layer: no reflection dependency, nothing here is on a hot path.
#include "internal.hpp"

#include <es/serreg/tools.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace es::serreg::tools
{
namespace
{
using internal::dec;
using internal::hex64;

// ---- canonical names (reflect.hpp, "canonical strings") ------------------------------------------------------
// A canonical name is identifiers joined by "::", with template arguments inside <…> separated by commas:
// "ns::Order", "ns::Outer<i32>::Inner", "std::pair<i32,f64>", "std::array<char,8>", "t::W<i32,-5>".

// The components of a name outside <…>: "a::b::C<x::y,z>::D" → {"a", "b", "C<x::y,z>", "D"}.
std::vector<std::string> split_scopes(const std::string_view name)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (c == '<')
            ++depth;
        else if (c == '>' && depth > 0)
            --depth;
        if (depth == 0 && c == ':' && i + 1 < name.size() && name[i + 1] == ':')
        {
            out.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// The template arguments between the outer angle brackets: "A,B<C,D>,E" → {"A", "B<C,D>", "E"}.
std::vector<std::string> split_args(const std::string_view inner)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (const char c : inner)
    {
        if (c == '<')
            ++depth;
        else if (c == '>' && depth > 0)
            --depth;
        if (c == ',' && depth == 0)
        {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty() || !out.empty())
        out.push_back(cur);
    return out;
}

bool ident_char(const char c)
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '_';
}
bool is_number(const std::string_view s)
{
    const std::string_view digits = s.starts_with("-") ? s.substr(1) : s;
    if (digits.empty())
        return false;
    for (const char c : digits)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// What [[=es::serreg::name("…")]] accepts (reflect.hpp valid_explicit_name): letters, digits, '_', ':', '<', '>',
// ',' and '-'. A name outside that set (an enum template argument renders as "enum(7)") cannot be given back.
bool annotatable(const std::string_view n)
{
    if (n.empty())
        return false;
    for (const char c : n)
        if (!ident_char(c) && c != ':' && c != '<' && c != '>' && c != ',' && c != '-')
            return false;
    return true;
}

// An identifier for any name component: letters, digits and '_' kept, every other run of characters one '_',
// a leading digit prefixed ("Box<i32,3>" → "Box_i32_3", "Keyed<ns::Order>::Key" → "Keyed_ns_Order_Key").
std::string sanitize(const std::string_view s)
{
    std::string out;
    bool under = false;
    for (const char c : s)
    {
        if (ident_char(c))
        {
            out += c;
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
    if (out.empty() || (out.front() >= '0' && out.front() <= '9'))
        out.insert(0, "_");
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string_view sep,
                 const std::size_t from = 0, std::size_t to = std::string::npos)
{
    std::string s;
    to = std::min(to, parts.size());
    for (std::size_t i = from; i < to; ++i)
    {
        if (i != from)
            s += sep;
        s += parts[i];
    }
    return s;
}

// ---- spellings ----------------------------------------------------------------------------------------------
std::string_view scalar_spelling(const kind k)
{
    switch (k)
    {
    case kind::i8:
        return "std::int8_t";
    case kind::i16:
        return "std::int16_t";
    case kind::i32:
        return "std::int32_t";
    case kind::i64:
        return "std::int64_t";
    case kind::u8:
        return "std::uint8_t";
    case kind::u16:
        return "std::uint16_t";
    case kind::u32:
        return "std::uint32_t";
    case kind::u64:
        return "std::uint64_t";
    case kind::f32:
        return "float";
    case kind::f64:
        return "double";
    case kind::boolean:
        return "bool";
    case kind::char_:
        return "char";
    case kind::char8:
        return "char8_t";
    case kind::char16:
        return "char16_t";
    case kind::char32:
        return "char32_t";
    case kind::wchar:
        return "wchar_t";
    case kind::byte_:
        return "std::byte";
    default:
        return {};
    }
}
bool kind_from_name(const std::string_view n, kind& k)
{
    for (unsigned i = 1; i <= static_cast<unsigned>(kind::byte_); ++i)
        if (to_string(static_cast<kind>(i)) == n)
        {
            k = static_cast<kind>(i);
            return true;
        }
    return false;
}
bool is_signed_kind(const kind k)
{
    return k == kind::i8 || k == kind::i16 || k == kind::i32 || k == kind::i64;
}

// A C++ string literal of arbitrary bytes: printable ASCII as is (" and \ escaped), every other byte as \xNN
// followed by "" so that a hex digit after it does not extend the escape (the pieces concatenate to one literal).
std::string cxx_str(const std::string_view s)
{
    std::string out = "\"";
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (c >= 0x20 && c < 0x7f)
            out += ch;
        else
            out += "\\x" + internal::hex8(c) + "\"\"";
    }
    return out + "\"";
}

// A decimal integer literal that is a constant expression of its own value (INT64_MIN has no literal).
std::string int_literal(const std::int64_t v)
{
    if (v == INT64_MIN)
        return "(-9223372036854775807 - 1)";
    return dec(v);
}

// The low `bytes` bytes of a constant's value, sign-extended.
std::int64_t sign_extend(const std::uint64_t v, const std::uint32_t bytes)
{
    if (bytes >= 8)
        return static_cast<std::int64_t>(v);
    const unsigned shift = 64u - 8u * bytes;
    return static_cast<std::int64_t>(v << shift) >> shift;
}

// Shortest decimal that reads back as the same value ("1.5", "2e+20"), with a '.' or an exponent so it is a
// floating literal; empty for a non-finite value (spelled through std::bit_cast by the caller).
template<class F> std::string float_literal(const F v)
{
    if (v != v || v == std::numeric_limits<F>::infinity() ||
        v == -std::numeric_limits<F>::infinity())
        return {};
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, r.ptr);
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    return s;
}

constexpr std::uint64_t align_up(const std::uint64_t n, const std::uint64_t a) noexcept
{
    return a ? (n + a - 1) / a * a : n;
}

// ---- the generator ------------------------------------------------------------------------------------------
// Where and how one dictionary type is declared.
struct plan
{
    std::vector<std::string> ns; // the namespace it is declared in (after --namespace)
    std::string ident;           // the declared identifier
    std::string qualified;       // how the rest of the header names it ("corpus::Order")
    bool annotate = false;       // [[=es::serreg::name("<canonical>")]] is needed
    // the declaration reproduces the canonical name (through the annotation or the plain spelling)
    bool named = true;
    bool emitted = true; // false: std vocabulary, spelled back and never defined
};

struct generator
{
    const dyn::dictionary& d;
    const cxx_options& o;
    std::map<std::uint64_t, plan> plans;
    std::map<std::string, std::uint64_t> by_name; // canonical name → schema hash
    std::set<std::string> includes;               // standard headers the output needs
    std::vector<std::string> notes;               // what could not be reproduced (banner comments)
    std::vector<std::uint64_t> order;             // emitted types in dependency order

    void note(std::string s)
    {
        if (std::find(notes.begin(), notes.end(), s) == notes.end())
            notes.push_back(std::move(s));
    }

    // --namespace old=new: the first rule whose `old` components prefix `ns` replaces them.
    std::vector<std::string> remap(std::vector<std::string> ns, bool& changed) const
    {
        const auto comps = [](const std::string_view s) {
            std::vector<std::string> v = split_scopes(s);
            std::erase_if(v, [](const std::string& c) { return c.empty(); });
            return v;
        };
        for (const auto& [from, to] : o.namespaces)
        {
            const std::vector<std::string> f = comps(from);
            if (f.size() > ns.size() || !std::equal(f.begin(), f.end(), ns.begin()))
                continue;
            std::vector<std::string> out = comps(to);
            out.insert(out.end(), ns.begin() + static_cast<std::ptrdiff_t>(f.size()), ns.end());
            changed = true;
            return out;
        }
        return ns;
    }

    void plan_types()
    {
        for (const auto& [h, t] : d.types)
            by_name[t.name] = h;
        std::set<std::string> used;
        for (const auto& [h, t] : d.types)
        {
            plan p;
            if (t.name.starts_with("std::"))
            { // spelled on demand (spell_name) once the user types are planned, never defined
                p.emitted = false;
                plans[h] = p;
                continue;
            }
            const std::vector<std::string> comps = split_scopes(t.name);
            // the namespace part ends at the first component that is a class: a template specialisation, or a
            // name the dictionary knows as a type (a nested class is declared beside its parent, not inside it)
            std::size_t k = comps.size() - 1;
            for (std::size_t i = 0; i + 1 < comps.size(); ++i)
            {
                if (comps[i].find('<') != std::string::npos ||
                    by_name.contains(join(comps, "::", 0, i + 1)))
                {
                    k = i;
                    break;
                }
            }
            p.ns.assign(comps.begin(), comps.begin() + static_cast<std::ptrdiff_t>(k));
            const std::string rest = join(comps, "::", k);
            p.ident = sanitize(rest);
            bool changed = false;
            p.ns = remap(p.ns, changed);
            p.annotate = rest != p.ident || changed;
            std::string key = join(p.ns, "::") + "::" + p.ident;
            if (!used.insert(key).second)
            { // two names that sanitize alike (Box<a> and Box<a_>): the hash tells them apart
                p.ident += "_" + hex64(h).substr(2, 8);
                p.annotate = true;
                used.insert(join(p.ns, "::") + "::" + p.ident);
            }
            p.qualified = p.ns.empty() ? p.ident : join(p.ns, "::") + "::" + p.ident;
            if (p.annotate && !annotatable(t.name))
            { // declared under the sanitized identifier only: its schema hash will not be the dictionary's
                p.annotate = false;
                p.named = false;
                note("type " + t.name +
                     " has a canonical name the name annotation cannot carry; it is "
                     "declared as " +
                     p.qualified + " and its name and schema hash are not checked");
            }
            plans[h] = p;
        }
        for (auto& [h, p] : plans)
            if (!p.emitted)
                p.qualified = spell_name(d.types.at(h).name);
    }

    // The C++ spelling of a canonical name that is not declared here: a scalar kind, a number or bool (template
    // arguments), a dictionary type (its planned name), a std type with its arguments spelled recursively.
    std::string spell_name(const std::string_view name)
    {
        kind k{};
        if (kind_from_name(name, k))
            return std::string(scalar_spelling(k));
        if (is_number(name) || name == "true" || name == "false")
            return std::string(name);
        if (const auto it = by_name.find(std::string(name)); it != by_name.end())
        {
            plan& p = plans.at(it->second);
            if (p.emitted)
                return p.qualified;
            if (p.qualified.empty()) // a std type referenced by another std type's arguments
                p.qualified = spell_std(name);
            return p.qualified;
        }
        if (name.starts_with("std::") || name.find('<') != std::string::npos)
            return spell_std(name);
        note("type " + std::string(name) +
             " is not in the dictionary; its declaration is missing here");
        return std::string(name);
    }
    std::string spell_std(const std::string_view name)
    {
        std::vector<std::string> comps = split_scopes(name);
        for (std::string& c : comps)
        {
            const std::size_t lt = c.find('<');
            if (lt == std::string::npos || c.back() != '>')
                continue;
            std::string s = c.substr(0, lt) + "<";
            const std::vector<std::string> args =
                split_args(std::string_view(c).substr(lt + 1, c.size() - lt - 2));
            for (std::size_t i = 0; i < args.size(); ++i)
                s += (i ? ", " : "") + spell_name(args[i]);
            c = s + ">";
        }
        if (comps.size() >= 2 && comps[0] == "std")
        {
            const std::string_view head = std::string_view(comps[1]).substr(0, comps[1].find('<'));
            static constexpr std::pair<std::string_view, std::string_view> headers[]{
                {"string", "<string>"},
                {"basic_string", "<string>"},
                {"vector", "<vector>"},
                {"array", "<array>"},
                {"optional", "<optional>"},
                {"variant", "<variant>"},
                {"monostate", "<variant>"},
                {"pair", "<utility>"},
                {"tuple", "<tuple>"},
                {"chrono", "<chrono>"},
                {"ratio", "<ratio>"},
                {"map", "<map>"},
                {"unordered_map", "<unordered_map>"},
                {"flat_map", "<flat_map>"},
                {"set", "<set>"},
                {"unordered_set", "<unordered_set>"},
                {"flat_set", "<flat_set>"},
                {"multimap", "<map>"},
                {"unordered_multimap", "<unordered_map>"},
                {"deque", "<deque>"},
                {"list", "<list>"},
                {"forward_list", "<forward_list>"},
                {"stack", "<stack>"}};
            for (const auto& [n, inc] : headers)
                if (n == head)
                    includes.insert(std::string(inc));
        }
        return join(comps, "::");
    }

    // How a member, element or argument of a dictionary type is spelled.
    std::string type_ref(const std::uint64_t h)
    {
        if (const auto it = plans.find(h); it != plans.end())
        {
            plan& p = it->second;
            if (!p.emitted &&
                p.qualified.empty()) // 1.6: a std type referenced directly (a container argument)
                if (const dyn::type* const t = d.find_type(h))
                    p.qualified = spell_std(t->name);
            return p.qualified;
        }
        note("type " + hex64(h) +
             " is referenced but not in the dictionary (add its source with --dict/--elf)");
        return "es_serreg_missing_" + hex64(h).substr(2);
    }
    std::string member_type(const dyn::field& f, const int depth = 0)
    {
        if (depth > 64)
            return "std::byte"; // a (corrupt) self-nesting descriptor
        if (is_scalar_kind(f.k))
            return std::string(scalar_spelling(f.k));
        const auto elem = [&](const std::size_t i) {
            return i < f.elem.size() ? member_type(f.elem[i], depth + 1) : std::string("std::byte");
        };
        switch (f.k)
        {
        case kind::enum_:
        case kind::struct_:
            return type_ref(f.type_hash);
        case kind::array_:
            includes.insert("<array>");
            return "std::array<" + elem(0) + ", " + dec(f.extent) + ">";
        case kind::string_:
            includes.insert("<string>");
            return "std::string";
        case kind::vector_:
            includes.insert("<vector>");
            return "std::vector<" + elem(0) + ">";
        case kind::optional_:
            includes.insert("<optional>");
            return "std::optional<" + elem(0) + ">";
        case kind::variant_: {
            includes.insert("<variant>");
            std::string s = "std::variant<";
            for (std::size_t i = 0; i < f.elem.size(); ++i)
                s += (i ? ", " : "") + elem(i);
            return s + ">";
        }
        case kind::
            map_: // 1.6: the dictionary records the shape; std::map is the container that reproduces it
            includes.insert("<map>");
            return "std::map<" + elem(0) + ", " + elem(1) + ">";
        case kind::set_:
            includes.insert("<set>");
            return "std::set<" + elem(0) + ">";
        default:
            note("member kind " + dec(static_cast<unsigned>(f.k)) + " is not supported");
            return "std::byte";
        }
    }

    // The emitted types a type's definition needs complete before it: every type its fields reference, through
    // element descriptors and through std vocabulary types (spelled, not defined, so their members count too).
    void collect_deps(const dyn::type& t, std::set<std::uint64_t>& out,
                      std::set<std::uint64_t>& seen)
    {
        const auto walk = [&](const auto& self, const dyn::field& f) -> void {
            if (f.type_hash != 0 && (f.k == kind::enum_ || f.k == kind::struct_))
            {
                const auto it = plans.find(f.type_hash);
                if (it == plans.end() || f.type_hash == t.schema_hash)
                    return;
                if (it->second.emitted)
                    out.insert(f.type_hash);
                else if (seen.insert(f.type_hash).second)
                    collect_deps(d.types.at(f.type_hash), out, seen);
            }
            for (const dyn::field& e : f.elem)
                self(self, e);
        };
        for (const dyn::field& f : t.fields)
            walk(walk, f);
    }
    void order_types()
    {
        std::vector<std::uint64_t> remaining;
        std::map<std::uint64_t, std::set<std::uint64_t>> deps;
        for (const auto& [h, p] : plans)
            if (p.emitted)
            {
                remaining.push_back(h);
                std::set<std::uint64_t> seen{h};
                collect_deps(d.types.at(h), deps[h], seen);
            }
        const auto name_less = [&](const std::uint64_t a, const std::uint64_t b) {
            return d.types.at(a).name < d.types.at(b).name;
        };
        std::sort(remaining.begin(), remaining.end(), name_less);
        std::set<std::uint64_t> done;
        while (!remaining.empty())
        { // the first (by canonical name) type whose dependencies are all defined; a cycle (impossible for
            // honest input) is broken at the first name
            auto it = std::find_if(remaining.begin(), remaining.end(), [&](const std::uint64_t h) {
                return std::all_of(deps[h].begin(), deps[h].end(), [&](const std::uint64_t x) {
                    return done.contains(x) || x == h;
                });
            });
            if (it == remaining.end())
            {
                note("type " + d.types.at(remaining.front()).name +
                     " is part of a dependency cycle");
                it = remaining.begin();
            }
            order.push_back(*it);
            done.insert(*it);
            remaining.erase(it);
        }
    }

    // ---- declarations ---------------------------------------------------------------------------------------
    static bool is_base(const dyn::field& f)
    {
        return f.name.starts_with("<base:") && f.name.ends_with(">");
    }
    bool empty_struct(const std::uint64_t h) const
    {
        const dyn::type* const t = d.find_type(h);
        return t && !t->is_enum && t->fields.empty();
    }
    std::string type_attrs(const dyn::type& t, const plan& p) const
    {
        std::string s;
        if (p.annotate)
            s += "[[=es::serreg::name(" + cxx_str(t.name) + ")]] ";
        if (t.since)
            s += "[[=es::serreg::since(" + dec(t.since) + ")]] ";
        if (t.deprecated)
            s += "[[=es::serreg::deprecated(" + dec(t.deprecated) + ")]] ";
        return s;
    }
    static std::string field_attrs(const dyn::field& f)
    {
        std::string s;
        if (f.since)
            s += "[[=es::serreg::since(" + dec(f.since) + ")]] ";
        if (f.deprecated)
            s += "[[=es::serreg::deprecated(" + dec(f.deprecated) + ")]] ";
        return s;
    }

    std::string declare_enum(const dyn::type& t, const plan& p) const
    {
        std::string s = "enum class " + type_attrs(t, p) + p.ident + " : " +
                        std::string(scalar_spelling(t.underlying)) + "\n{\n";
        for (const auto& [name, v] : t.enumerators)
        {
            s += "    " + name + " = ";
            s += t.underlying == kind::boolean ? std::string(v ? "true" : "false") : int_literal(v);
            s += ",\n";
        }
        return s + "};\n";
    }

    // One member line of a struct, with the padding before it when the recorded layout needs one.
    struct member_line
    {
        std::string text;
        std::uint64_t offset{}, size{};
        // occupies [offset, offset + size) in the layout (false: an empty member the next one overlaps)
        bool counted = true;
        bool pad = false; // a padding member or a layout note, not a field of the type
    };

    std::string declare_struct(const dyn::type& t, const plan& p)
    {
        std::vector<std::string> bases;
        std::vector<member_line> lines;
        std::set<std::string> member_names;
        for (const dyn::field& f : t.fields)
            member_names.insert(f.name);
        std::uint64_t max_align = 1;
        unsigned pads = 0;
        bool packed = false;
        const auto end_of = [&] {
            std::uint64_t e = 0;
            for (const member_line& m : lines)
                if (m.counted)
                    e = std::max(e, m.offset + m.size);
            return e;
        };
        const auto pad_name = [&] {
            std::string n;
            do
                n = "_skip" + dec(pads++);
            while (member_names.contains(n));
            return n;
        };
        for (std::size_t i = 0; i < t.fields.size(); ++i)
        {
            const dyn::field& f = t.fields[i];
            const bool base = is_base(f);
            if (base)
                bases.push_back(type_ref(f.type_hash));
            const std::string type = base ? std::string{} : member_type(f);
            max_align = std::max<std::uint64_t>(max_align, f.align);
            if (t.is_flat)
            { // native offsets: pad up to the recorded offset; an empty member overlapped by the next one was
                // [[no_unique_address]]; anything else below the natural offset is a packed layout
                const std::uint64_t natural = align_up(end_of(), f.align ? f.align : 1);
                if (f.offset > natural)
                {
                    lines.push_back({"    [[=es::serreg::skip]] std::byte " + pad_name() + "[" +
                                         dec(f.offset - natural) + "];\n",
                                     natural, f.offset - natural, true, true});
                }
                else if (f.offset < natural)
                {
                    bool fixed = false;
                    if (!lines.empty() && !lines.back().pad && lines.back().counted &&
                        lines.back().size == 1 && !is_base(t.fields[i - 1]) &&
                        t.fields[i - 1].k == kind::struct_ &&
                        empty_struct(t.fields[i - 1].type_hash))
                    {
                        lines.back().counted = false;
                        lines.back().text.insert(4, "[[no_unique_address]] ");
                        fixed = align_up(end_of(), f.align ? f.align : 1) == f.offset;
                    }
                    if (!fixed)
                    {
                        packed = true;
                        lines.push_back({"    // NOTE: packed layout, offset " + dec(f.offset) +
                                             " of " + f.name + " is not reproducible\n",
                                         f.offset, 0, false, true});
                    }
                }
            }
            if (base)
            { // an empty base takes no bytes (empty-base optimisation): it does not advance the layout
                lines.push_back({{}, f.offset, f.size, !empty_struct(f.type_hash), false});
                continue;
            }
            lines.push_back({"    " + field_attrs(f) + type + " " + f.name + ";\n", f.offset,
                             f.size, true, false});
        }
        std::string body;
        for (const member_line& m : lines)
            body += m.text;
        if (t.is_flat && !(t.fields.empty() && t.size == 1))
        {
            const std::uint64_t end = end_of();
            if (t.size > align_up(end, t.align ? t.align : 1))
                body += "    [[=es::serreg::skip]] std::byte " + pad_name() + "[" +
                        dec(t.size - end) + "];\n";
            else if (t.size < align_up(end, t.align ? t.align : 1))
                packed = true;
        }
        if (packed)
            note("type " + t.name + " has a packed layout; its static_assert will fail");
        std::string head = "struct " + type_attrs(t, p);
        if (t.is_flat && t.align > max_align)
            head += "alignas(" + dec(t.align) + ") ";
        head += p.ident;
        if (!bases.empty())
            head += " : " + join(bases, ", ");
        return head + "\n{\n" + body + "};\n";
    }

    // The static_asserts of one type: size and alignment (flat: native; dynamic: the image's fixed part),
    // offsets (flat types without bases: offsetof is conditionally supported otherwise), the canonical name and
    // both hashes, exactly as the dictionary records them.
    std::string checks(const dyn::type& t, const plan& p) const
    {
        const std::string q = p.qualified;
        std::string s;
        if (t.is_flat)
            s += "static_assert(sizeof(" + q + ") == " + dec(t.size) + " && alignof(" + q +
                 ") == " + dec(t.align) + ");\n";
        else
            s += "static_assert(es::serreg::descriptor_v<" + q + ">.size == " + dec(t.size) +
                 " && es::serreg::descriptor_v<" + q + ">.align == " + dec(t.align) + ");\n";
        if (t.is_flat && !t.is_enum && p.emitted &&
            std::none_of(t.fields.begin(), t.fields.end(), is_base))
            for (const dyn::field& f : t.fields)
                s += "static_assert(offsetof(" + q + ", " + f.name + ") == " + dec(f.offset) +
                     ");\n";
        if (p.named)
        {
            s += "static_assert(es::serreg::canonical_name_v<" + q + "> == " + cxx_str(t.name) +
                 ");\n";
            s += "static_assert(es::serreg::schema_hash_v<" + q + "> == " + hex64(t.schema_hash) +
                 "ull);\n";
        }
        else
            s += "// " + q + " stands for " + t.name + " (schema hash " + hex64(t.schema_hash) +
                 "): the name is not reproducible, so only the layout hash is checked\n";
        s += "static_assert(es::serreg::layout_hash_v<" + q + "> == " + hex64(t.layout_hash) +
             "ull);\n";
        return s;
    }

    // ---- the router skeleton --------------------------------------------------------------------------------
    // A constant argument as the V of es::serreg::constant<V>: the value's own type, so the call string
    // ("const:i32=42", "const:<enum hash>=2") is the writer's. Empty when it cannot be spelled.
    std::string constant_value(const dyn::call_arg& a)
    {
        const std::uint32_t n = kind_size(a.k);
        if (n == 0)
        { // an enum constant: the enumerator of that value, or a cast
            const dyn::type* const t = d.find_type(a.hash);
            if (!t || !t->is_enum)
                return {};
            const std::int64_t v = is_signed_kind(t->underlying)
                                       ? sign_extend(a.value, kind_size(t->underlying))
                                       : static_cast<std::int64_t>(a.value);
            const std::string q = type_ref(a.hash);
            for (const auto& [name, ev] : t->enumerators)
                if (ev == v)
                    return q + "::" + name;
            return "static_cast<" + q + ">(" + int_literal(v) + ")";
        }
        const std::string spelled(scalar_spelling(a.k));
        switch (a.k)
        {
        case kind::boolean:
            return a.value ? "true" : "false";
        case kind::i32:
            return a.value == 0x80000000ull ? "(-2147483647 - 1)" : dec(sign_extend(a.value, 4));
        case kind::i8:
        case kind::i16:
        case kind::i64:
            return spelled + "{" + int_literal(sign_extend(a.value, n)) + "}";
        case kind::u8:
        case kind::u16:
        case kind::u32:
        case kind::u64:
            return spelled + "{" + dec(a.value) + "u}";
        case kind::f64:
            if (const std::string f = float_literal(std::bit_cast<double>(a.value)); !f.empty())
                return f;
            includes.insert("<bit>");
            return "std::bit_cast<double>(" + hex64(a.value) + "ull)";
        case kind::f32:
            if (const std::string f =
                    float_literal(std::bit_cast<float>(static_cast<std::uint32_t>(a.value)));
                !f.empty())
                return f + "f";
            includes.insert("<bit>");
            return "std::bit_cast<float>(0x" + hex64(a.value).substr(10) + "u)";
        case kind::char_:
            if (a.value >= 0x20 && a.value < 0x7f && a.value != '\'' && a.value != '\\')
                return "'" + std::string(1, static_cast<char>(a.value)) + "'";
            return "static_cast<char>(" + dec(a.value) + ")";
        case kind::byte_:
            return "std::byte{" + dec(a.value) + "}";
        default: // char8, char16, char32, wchar
            return "static_cast<" + spelled + ">(" + dec(a.value) + ")";
        }
    }

    // One r.on… line for a call signature, or a comment saying why there is none.
    std::string handler(const std::uint64_t hash, const dyn::call& c)
    {
        const std::string tag = " // call " + hex64(hash) + "\n";
        if (c.args.empty())
            return "    // call " + hex64(hash) + ": no arguments — skipped\n";
        std::vector<std::string> sig, params; // the explicit signature; the handler's parameters
        bool general = false;  // a literal or constant anywhere but first: on<Sig...>(h)
        std::string first_lit; // the first argument's text when it is a literal ("…"_)
        for (std::size_t i = 0; i < c.args.size(); ++i)
        {
            const dyn::call_arg& a = c.args[i];
            const std::string an = "a" + dec(i);
            if (a.ak == fmt::arg_literal)
            {
                const dyn::literal* const l = d.find_literal(a.hash);
                if (!l)
                    return "    // call " + hex64(hash) + ": literal " + hex64(a.hash) +
                           " unknown — skipped\n";
                sig.push_back("es::serreg::lit<" + cxx_str(l->text) + ">");
                if (i == 0)
                    first_lit = cxx_str(l->text);
                else
                    general = true;
            }
            else if (a.ak == fmt::arg_const)
            {
                if (const dyn::type* const t = kind_size(a.k) == 0 ? d.find_type(a.hash) : nullptr;
                    t && !plans.at(a.hash).named)
                    return "    // call " + hex64(hash) + ": constant of type " + t->name +
                           ", which is not reproducible — skipped\n";
                const std::string v = constant_value(a);
                if (v.empty())
                    return "    // call " + hex64(hash) + ": constant of unknown type " +
                           hex64(a.hash) + " — skipped\n";
                sig.push_back("es::serreg::constant<" + v + ">");
                general = true;
            }
            else if (a.ak == fmt::arg_scalar)
            {
                const std::string t(scalar_spelling(a.k));
                if (t.empty())
                    return "    // call " + hex64(hash) + ": scalar kind " +
                           dec(static_cast<unsigned>(a.k)) + " unknown — skipped\n";
                sig.push_back(t);
                params.push_back("[[maybe_unused]] " + t + " " + an);
            }
            else if (a.ak == fmt::arg_string)
            {
                includes.insert("<string_view>");
                sig.push_back("std::string_view");
                params.push_back("[[maybe_unused]] std::string_view " + an);
            }
            else if (a.ak == fmt::arg_type)
            {
                const dyn::type* const t = d.find_type(a.hash);
                if (!t)
                    return "    // call " + hex64(hash) + ": type " + hex64(a.hash) +
                           " unknown — skipped\n";
                if (!plans.at(a.hash)
                         .named) // its schema hash, so this call hash, is not reproducible
                    return "    // call " + hex64(hash) + ": type " + t->name +
                           " is not reproducible — skipped\n";
                const std::string q = type_ref(a.hash);
                sig.push_back(q);
                params.push_back("[[maybe_unused]] " + (t->is_enum ? q : "const " + q + "&") + " " +
                                 an);
            }
            else
                return "    // call " + hex64(hash) + ": argument category " +
                       dec(static_cast<unsigned>(a.ak)) + " unknown — skipped\n";
        }
        const std::string lambda = "[](" + join(params, ", ") + ") { /* TODO */ }";
        // "    r.on…(" + the lambda + ");" and the call hash; the lambda goes to its own line when that is long
        const auto line = [&](const std::string& head) {
            const std::string one = head + lambda + ");";
            if (one.size() <= 100)
                return one + tag;
            return head.substr(0, head.find_last_not_of(' ') + 1) + "\n        " + lambda + ");" +
                   tag;
        };
        if (c.is_print())
        { // on_print / on_println(fmt, h): the format literal, then h's parameters; constants need on<Sig...>
            if (first_lit.empty())
                return "    // call " + hex64(hash) +
                       ": a print call without a format literal — skipped\n";
            if (!general)
                return line(std::string("    r.") + (c.newline() ? "on_println(" : "on_print(") +
                            first_lit + "_, ");
            sig.insert(sig.begin(),
                       c.newline() ? "es::serreg::println_tag" : "es::serreg::print_tag");
            return line("    r.on<" + join(sig, ", ") + ">(");
        }
        if (!general && !first_lit.empty()) // on(lit, h): a literal, then h's parameters
            return line("    r.on(" + first_lit + "_, ");
        if (!general) // on(h): h's parameters are the signature
            return line("    r.on(");
        return line("    r.on<" + join(sig, ", ") + ">("); // literals and constants anywhere
    }

    std::string run()
    {
        plan_types();
        order_types();
        includes.insert("<cstddef>");
        includes.insert("<cstdint>");
        // declarations first: they decide the includes and the notes of the banner
        std::string decls;
        std::vector<std::string> open;
        for (const std::uint64_t h : order)
        {
            const dyn::type& t = d.types.at(h);
            const plan& p = plans.at(h);
            if (p.ns != open)
            {
                if (!open.empty())
                    decls += "\n} // namespace " + join(open, "::") + "\n";
                open = p.ns;
                if (!open.empty())
                    decls += "\nnamespace " + join(open, "::") + "\n{\n";
            }
            decls += "\n" + (t.is_enum ? declare_enum(t, p) : declare_struct(t, p));
        }
        if (!open.empty())
            decls += "\n} // namespace " + join(open, "::") + "\n";

        std::string router;
        if (o.router)
        {
            router = "\n// One handler per call signature of the dictionary. Fill in the bodies.\n"
                     "inline void install_handlers(es::serreg::frame_router& r)\n{\n"
                     "    using namespace es::serreg::literals;\n";
            for (const auto& [h, c] : d.calls)
                router += handler(h, c);
            if (d.calls.empty())
                router += "    static_cast<void>(r); // the dictionary has no call signatures\n";
            router += "}\n";
        }

        std::string out =
            "// generated by srdump --cxx: " + dec(d.types.size()) + " types, " +
            dec(d.literals.size()) + " literals, " + dec(d.calls.size()) + " calls; program hash " +
            hex64(d.program_hash ? d.program_hash : d.computed_program_hash()) + "; format " +
            dec(fmt::format_major) + "." + dec(fmt::format_minor) + ", hash_algo " +
            dec(fmt::hash_algo_xxh3_64) + "; abi " + internal::abi_text(d.abi) +
            "\n// Every type below reproduces the schema hash and layout hash the dictionary "
            "records; the static_asserts at the end\n// check that when this file is compiled. "
            "Do not edit: regenerate when the program's types change.\n";
        if (!d.same_abi())
            out += "// NOTE: the dictionary was written on another ABI (this host: " +
                   internal::abi_text(fmt::host_abi_tag) +
                   "); sizes and offsets may not be reproducible.\n";
        for (const std::string& n : notes)
            out += "// NOTE: " + n + "\n";
        out += "#pragma once\n#include " + std::string(o.include) + "\n\n";
        for (const std::string& inc : includes)
            out += "#include " + inc + "\n";
        out += decls;
        if (!d.literals.empty())
        {
            out += "\n// literals\n";
            for (const auto& [id, l] : d.literals)
                out += "//   " + hex64(id) + ": " + cxx_str(l.text) + "\n";
        }
        out += "\n// self-check: sizes, offsets, names and hashes as the dictionary records them\n";
        for (const std::uint64_t h : order)
            out += checks(d.types.at(h), plans.at(h));
        for (const auto& [name, h] : by_name)
            if (!plans.at(h).emitted)
                out += checks(d.types.at(h), plans.at(h));
        return out + router;
    }
};

} // namespace

std::string cxx_header(const dyn::dictionary& d, const cxx_options& o)
{
    generator g{d, o, {}, {}, {}, {}, {}};
    return g.run();
}

} // namespace es::serreg::tools
