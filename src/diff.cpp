// src/diff.cpp — tools::diff: compare two dictionaries by canonical type name (spec FR-D3; 2.1.16: the direction
// decides whether removals or additions are breaking — decision E):
// identical | names-only | metadata-only (since/deprecated, compatible enumerator additions) | breaking (layout
// differs, or an enumerator value changed/disappeared — gk 1.11) | added | removed | duplicate-name (a second
// type with the same canonical name, §6.2b). Calls and literals are diffed by id: "call added/removed/changed",
// "literal added/removed/changed".
#include "internal.hpp"

#include <es/serreg/tools.hpp>

#include <algorithm>

namespace es::serreg::tools
{
namespace
{

using internal::dec;
using internal::hex64;

bool same_metadata(const std::vector<dyn::field>& a, const std::vector<dyn::field>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].since != b[i].since || a[i].deprecated != b[i].deprecated ||
            !same_metadata(a[i].elem, b[i].elem))
            return false;
    return true;
}

// gk 1.11: enumerators are compared as (name, value) pairs. A value that disappears or a name whose value
// changes is breaking; a renamed value is names-only; extra values in b are a compatible addition.
std::string enum_verdict(const dyn::type& a, const dyn::type& b)
{
    bool renamed = false;
    for (const auto& [name, value] : a.enumerators)
    {
        const auto by_name =
            std::ranges::find_if(b.enumerators, [&](const auto& e) { return e.first == name; });
        if (by_name != b.enumerators.end() && by_name->second != value)
            return "breaking";
        const auto by_value =
            std::ranges::find_if(b.enumerators, [&](const auto& e) { return e.second == value; });
        if (by_value == b.enumerators.end())
            return "breaking";
        if (by_name == b.enumerators.end())
            renamed = true;
    }
    if (renamed)
        return "names-only";
    return a.enumerators.size() == b.enumerators.size() && a.since == b.since &&
                   a.deprecated == b.deprecated
               ? "identical"
               : "metadata-only";
}

// All types per canonical name, ordered by schema hash so the "primary" definition is deterministic.
std::map<std::string_view, std::vector<const dyn::type*>> by_name(const dyn::dictionary& d)
{
    std::map<std::string_view, std::vector<const dyn::type*>> m;
    for (const auto& [h, t] : d.types)
        m[t.name].push_back(&t);
    return m;
}

std::string literal_label(const dyn::literal& l)
{
    std::string s = "literal \"";
    for (const char c : l.text)
    {
        if (c == '"' || c == '\\')
            s += '\\';
        s += (static_cast<unsigned char>(c) < 0x20) ? '?' : c;
    }
    return s + '"';
}

} // namespace

diff_report diff(const dyn::dictionary& a, const dyn::dictionary& b, const diff_direction direction)
{
    diff_report rep;
    rep.direction = direction;
    const auto ma = by_name(a), mb = by_name(b);
    const auto extras = [&](const std::string_view name, const std::vector<const dyn::type*>& v,
                            const bool side_a) {
        for (std::size_t i = 1; i < v.size(); ++i) // gk 1.11: never drop a same-named type silently
            rep.entries.push_back({std::string(name), side_a ? v[i]->schema_hash : 0,
                                   side_a ? 0 : v[i]->schema_hash, "duplicate-name"});
    };
    const auto verdict_of = [&](const dyn::type* const ta, const dyn::type* const tb) {
        std::string verdict;
        if (ta->schema_hash == tb->schema_hash)
            verdict = "identical";
        else if (ta->layout_hash != tb->layout_hash || ta->is_enum != tb->is_enum)
            verdict = "breaking";
        else if (ta->is_enum)
            verdict = enum_verdict(*ta, *tb);
        else if (ta->since == tb->since && ta->deprecated == tb->deprecated &&
                 same_metadata(ta->fields, tb->fields))
            verdict = "names-only";
        else
            verdict = "metadata-only";
        return verdict;
    };
    for (const auto& [name, va] : ma)
    {
        const auto it = mb.find(name);
        if (it == mb.end())
        {
            rep.entries.push_back({std::string(name), va[0]->schema_hash, 0, "removed"});
            extras(name, va, true);
            continue;
        }
        // 2.1.12 (review T-09): several versions of one name (a dictionary merged from several binaries) pair
        // up by schema hash first, so a version present on both sides is "identical"; only the versions left
        // over are compared with each other (first with first), and the rest are listed as duplicate names.
        std::vector<const dyn::type*> ra, rb;
        for (const dyn::type* const ta : va)
        {
            if (std::ranges::any_of(it->second, [&](const dyn::type* const tb) {
                    return tb->schema_hash == ta->schema_hash;
                }))
            {
                rep.entries.push_back(
                    {std::string(name), ta->schema_hash, ta->schema_hash, "identical"});
            }
            else
                ra.push_back(ta);
        }
        for (const dyn::type* const tb : it->second)
            if (!std::ranges::any_of(va, [&](const dyn::type* const ta) {
                    return ta->schema_hash == tb->schema_hash;
                }))
                rb.push_back(tb);
        if (!ra.empty() && !rb.empty())
        {
            std::string verdict = verdict_of(ra[0], rb[0]);
            rep.entries.push_back(
                {std::string(name), ra[0]->schema_hash, rb[0]->schema_hash, std::move(verdict)});
            ra.erase(ra.begin());
            rb.erase(rb.begin());
        }
        for (const dyn::type* const ta : ra)
            rep.entries.push_back({std::string(name), ta->schema_hash, 0, "duplicate-name"});
        for (const dyn::type* const tb : rb)
            rep.entries.push_back({std::string(name), 0, tb->schema_hash, "duplicate-name"});
    }
    for (const auto& [name, vb] : mb)
        if (!ma.contains(name))
        {
            rep.entries.push_back({std::string(name), 0, vb[0]->schema_hash, "added"});
            extras(name, vb, false);
        }

    for (const auto& [h, ka] : a.calls)
    {
        const auto it = b.calls.find(h);
        if (it == b.calls.end())
        {
            rep.entries.push_back({"call " + hex64(h), h, 0, "call removed"});
            continue;
        }
        const auto& args_a = ka.args;
        const auto& args_b = it->second.args;
        const bool same = ka.flags == it->second.flags && args_a.size() == args_b.size() &&
                          std::ranges::equal(args_a, args_b, [](const auto& x, const auto& y) {
                              return x == y; // 1.1: category, scalar kind and hash
                          });
        if (!same)
        {
            rep.entries.push_back({"call " + hex64(h), h, h, "call changed"});
        }
    }
    for (const auto& [h, kb] : b.calls)
        if (!a.calls.contains(h))
            rep.entries.push_back({"call " + hex64(h), 0, h, "call added"});

    for (const auto& [id, la] : a.literals)
    {
        const auto it = b.literals.find(id);
        if (it == b.literals.end())
        {
            rep.entries.push_back({literal_label(la), id, 0, "literal removed"});
            continue;
        }
        if (it->second.text != la.text)
        {
            rep.entries.push_back({literal_label(la) + " -> " + literal_label(it->second), id, id,
                                   "literal changed"});
        }
    }
    for (const auto& [id, lb] : b.literals)
        if (!a.literals.contains(id))
            rep.entries.push_back({literal_label(lb), 0, id, "literal added"});
    // 2.1.16 (decision E): what breaks depends on the direction the caller named
    const bool strict_removals =
        direction == diff_direction::old_data || direction == diff_direction::both;
    const bool strict_additions =
        direction == diff_direction::new_data || direction == diff_direction::both;
    for (diff_entry& e : rep.entries)
    {
        if (e.verdict == "breaking" || e.verdict == "call changed" ||
            e.verdict == "literal changed")
        {
            e.breaking = true;
            e.note =
                e.verdict == "breaking"
                    ? "layout differs: frames of either side cannot be read by the other"
                    : "changed on both sides: frames of either side cannot be read by the other";
            ++rep.breaking_changes;
        }
        else if (strict_removals && (e.verdict == "removed" || e.verdict == "call removed" ||
                                     e.verdict == "literal removed"))
        {
            e.breaking = true;
            e.note =
                std::string(
                    "removed: a reader built from the new side cannot read old frames of this ") +
                (e.verdict == "removed"        ? "type"
                 : e.verdict == "call removed" ? "call"
                                               : "literal") +
                " typed";
            ++rep.breaking_removals;
        }
        else if (strict_additions && (e.verdict == "added" || e.verdict == "call added" ||
                                      e.verdict == "literal added"))
        {
            e.breaking = true;
            e.note = std::string("added: a reader built from the old side meets an unknown ") +
                     (e.verdict == "added"        ? "type"
                      : e.verdict == "call added" ? "call"
                                                  : "literal");
            ++rep.breaking_additions;
        }
        if (e.breaking)
            rep.breaking = true;
    }
    return rep;
}

std::string diff_report::to_text() const
{
    std::string out;
    std::map<std::string, std::size_t> counts;
    std::size_t n_types = 0, n_calls = 0, n_literals = 0;
    for (const auto& e : entries)
    {
        ++counts[e.verdict];
        if (e.verdict.starts_with("call "))
            ++n_calls;
        else if (e.verdict.starts_with("literal "))
            ++n_literals;
        else
            ++n_types;
        out += e.verdict;
        out.append(e.verdict.size() < 16 ? 16 - e.verdict.size() : 1, ' ');
        out += e.name + "  " + (e.schema_a ? hex64(e.schema_a) : std::string("-")) + " -> " +
               (e.schema_b ? hex64(e.schema_b) : std::string("-"));
        if (e.breaking)
            out += "  breaking: " + e.note; // 2.1.16
        out += "\n";
    }
    out += dec(n_types) + " types:";
    for (const char* const v : {"identical", "names-only", "metadata-only", "breaking", "added",
                                "removed", "duplicate-name"})
        out +=
            " " + dec(counts[v]) + " " + v + (v == std::string_view("duplicate-name") ? ";" : ",");
    out += " " + dec(n_calls) + " calls: " + dec(counts["call added"]) + " added, " +
           dec(counts["call removed"]) + " removed, " + dec(counts["call changed"]) + " changed;";
    out += " " + dec(n_literals) + " literals: " + dec(counts["literal added"]) + " added, " +
           dec(counts["literal removed"]) + " removed, " + dec(counts["literal changed"]) +
           " changed";
    if (breaking)
    { // 2.1.16: say which direction, and what broke it
        out += "  BREAKING (direction " + std::string(to_string(direction)) + ": " +
               dec(breaking_changes) + " changed, " + dec(breaking_removals) + " removed, " +
               dec(breaking_additions) + " added)\n";
    }
    else
        out += "  (direction " + std::string(to_string(direction)) + ")\n";
    return out;
}

} // namespace es::serreg::tools
