// examples/user_example1/tree_printer.hpp — a tools::decoded_value_visitor that prints an indented tree, one line
// per leaf value, from nothing but the dictionary's description of each type. Used by reader2 and reader3.
#pragma once

#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

namespace ue1
{
using namespace es::serreg;

// A decoded_value_visitor that prints an indented tree: one line per leaf value.
struct tree_printer final : tools::decoded_value_visitor
{
    int depth = 1;
    std::uint64_t values = 0;

    void line(const std::string_view name, const std::string& value)
    {
        ++values;
        std::println("{:{}}{}{}{}", "", 2 * depth, name, name.empty() ? "" : " = ", value);
    }
    void open(const std::string_view name, const std::string& what)
    {
        std::println("{:{}}{}{}{}", "", 2 * depth, name, name.empty() ? "" : ": ", what);
        ++depth;
    }
    void begin_struct(const std::string_view type_name, const std::string_view field) override
    {
        open(field, std::string(type_name) + " {");
    }
    void end_struct() override
    {
        --depth;
        std::println("{:{}}}}", "", 2 * depth);
    }
    void begin_array(const std::string_view field, const std::uint32_t n) override
    {
        open(field, "array[" + std::to_string(n) + "]");
    }
    void end_array() override
    {
        --depth;
    }
    void begin_vector(const std::string_view field, const std::uint32_t n) override
    {
        open(field, "vector[" + std::to_string(n) + "]");
    }
    void end_vector() override
    {
        --depth;
    }
    void begin_variant(const std::string_view field, const std::uint32_t index,
                       const std::string_view alt) override
    {
        open(field, std::string(alt) + " (alternative " + std::to_string(index) + ")");
    }
    void end_variant() override
    {
        --depth;
    }
    void scalar(const std::string_view field, const kind k,
                const std::span<const std::byte> b) override
    {
        line(field,
             tools::value_text(k, b) + "  (" + std::string(to_string(k)) + ")"); // srdump's text
    }
    void enumerator(const std::string_view field, const std::string_view type,
                    const std::string_view name, const std::int64_t value) override
    {
        line(field, std::string(type) + "::" + (name.empty() ? "?" : std::string(name)) + " (" +
                        std::to_string(value) + ")");
    }
    void literal(const std::string_view field, const std::uint64_t,
                 const std::string_view text) override
    {
        line(field, "\"" + std::string(text) + "\" (literal)");
    }
    void string(const std::string_view field, const std::string_view s) override
    {
        line(field, "\"" + std::string(s) + "\"");
    }
    void null_value(const std::string_view field) override
    {
        line(field, "null");
    }
};
} // namespace ue1
