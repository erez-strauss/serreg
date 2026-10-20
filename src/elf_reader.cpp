// src/elf_reader.cpp — self-contained ELF64 little-endian section reader (no libelf) used to extract the
// embedded registry copy (`es_serreg_dict`, spec §6.8, FR-D6) — the offline half of the ELF image backend
// (review addendum A4; the load-time half is src/image_elf.cpp). File I/O lives in src/file_io.cpp (2.1.8).
// Hostile-input rules (FR-D5, gk 1.15): every offset is computed with overflow checks, the section count is
// capped by what the image can hold, and inputs are capped at internal::input_limit().
#include "internal.hpp"

#include <es/serreg/tools.hpp>

#include <cstring>

namespace es::serreg::tools
{
namespace
{

struct section_header
{
    std::uint32_t name;
    std::uint32_t type;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t link;
};

// Reads section header `i` from the table at `shoff` (entries `shentsize` bytes apart); bounds- and
// overflow-checked (gk 1.15: `shoff + i * shentsize` must not wrap into the image).
bool read_shdr(const std::span<const std::byte> img, const std::uint64_t shoff,
               const std::uint16_t shentsize, const std::uint64_t i, section_header& out)
{
    std::uint64_t rel{}, pos{};
    if (shentsize < 64 || __builtin_mul_overflow(i, std::uint64_t{shentsize}, &rel) ||
        __builtin_add_overflow(shoff, rel, &pos))
        return false;
    if (pos > img.size() || img.size() - pos < 64)
        return false;
    fmt::cursor c(img.subspan(pos, 64));
    std::uint64_t flags{}, addr{};
    return c.read(out.name) && c.read(out.type) && c.read(flags) && c.read(addr) &&
           c.read(out.offset) && c.read(out.size) && c.read(out.link);
}

// Is the NUL-terminated name at `pos` exactly `want`? 2.1.12 (review D-04): compares at most want.size() + 1
// bytes; scanning each name to its NUL made a crafted string table (one long name, many headers pointing into
// it) quadratic.
bool name_is(const std::span<const std::byte> names, const std::uint64_t pos,
             const std::string_view want)
{
    if (pos >= names.size() || names.size() - pos <= want.size())
        return false;
    const char* const p = reinterpret_cast<const char*>(names.data() + pos);
    return std::memcmp(p, want.data(), want.size()) == 0 && p[want.size()] == '\0';
}

result<std::vector<std::byte>> find_section(const std::span<const std::byte> img,
                                            const std::string_view section)
{
    constexpr std::size_t ehdr_size = 64;
    if (img.size() < ehdr_size)
        return std::unexpected(error::truncated);
    if (img[0] != std::byte{0x7f} || img[1] != std::byte{'E'} || img[2] != std::byte{'L'} ||
        img[3] != std::byte{'F'})
        return std::unexpected(error::bad_magic);
    if (img[4] != std::byte{2})
        return std::unexpected(error::unsupported); // ELFCLASS64 only
    if (img[5] != std::byte{1})
        return std::unexpected(error::endianness_mismatch); // ELFDATA2LSB only
    fmt::cursor c(img.subspan(40));                         // e_shoff lives at 40
    std::uint64_t shoff{};
    std::uint32_t eflags{};
    std::uint16_t ehsize{}, phentsize{}, phnum{}, shentsize{}, shnum{}, shstrndx{};
    if (!c.read(shoff) || !c.read(eflags) || !c.read(ehsize) || !c.read(phentsize) ||
        !c.read(phnum) || !c.read(shentsize) || !c.read(shnum) || !c.read(shstrndx))
        return std::unexpected(error::truncated);
    if (shoff == 0)
        return std::unexpected(error::not_found);
    section_header sh0{};
    if (!read_shdr(img, shoff, shentsize, 0, sh0))
        return std::unexpected(error::truncated);
    std::uint64_t n_sections = shnum;
    std::uint64_t str_index = shstrndx;
    if (n_sections == 0)
        n_sections = sh0.size; // extended numbering
    if (str_index == 0xffff)
        str_index = sh0.link; // SHN_XINDEX
    // The table cannot hold more headers than the image has bytes for (gk 1.15: sh0.size is attacker-chosen).
    if (n_sections > img.size() / shentsize)
        return std::unexpected(error::truncated);
    section_header strtab{};
    if (str_index >= n_sections || !read_shdr(img, shoff, shentsize, str_index, strtab) ||
        strtab.offset > img.size() || img.size() - strtab.offset < strtab.size)
        return std::unexpected(error::truncated);
    const std::span<const std::byte> names = img.subspan(strtab.offset, strtab.size);
    for (std::uint64_t i = 1; i < n_sections; ++i)
    {
        section_header sh{};
        if (!read_shdr(img, shoff, shentsize, i, sh))
            return std::unexpected(error::truncated);
        if (!name_is(names, sh.name, section))
            continue;
        if (sh.type == 8 /*SHT_NOBITS*/)
            return std::vector<std::byte>{};
        if (sh.offset > img.size() || img.size() - sh.offset < sh.size)
            return std::unexpected(error::truncated);
        const auto s = img.subspan(sh.offset, sh.size);
        return std::vector<std::byte>(s.begin(), s.end());
    }
    return std::unexpected(error::not_found);
}

} // namespace

result<std::vector<std::byte>> extract_section_from_image(const std::span<const std::byte> image,
                                                          const std::string_view section)
{
    auto r = find_section(image, section);
    // The attribute spells the section without a dot; some toolchains/documents use ".es_serreg_dict".
    if (!r && r.error() == error::not_found && !section.empty() && section[0] != '.')
        r = find_section(image, "." + std::string(section));
    return r;
}

result<std::vector<std::byte>> extract_section_from_elf(const std::string_view path,
                                                        const std::string_view section)
{
    const auto img = read_file(path);
    if (!img)
        return std::unexpected(img.error());
    return extract_section_from_image(*img, section);
}

result<dyn::dictionary> extract_embedded_dictionary(const std::span<const std::byte> image)
{
    // A4 (2.1.8): the executable format is recognised by its magic; ELF is the only format with a reader. Any
    // other image (PE, Mach-O, a raw blob) is error::unsupported — the offline path of the null backend.
    if (image.size() < 4 || image[0] != std::byte{0x7f} || image[1] != std::byte{'E'} ||
        image[2] != std::byte{'L'} || image[3] != std::byte{'F'})
        return std::unexpected(image.size() < 4 ? error::truncated : error::unsupported);
    const auto bytes = extract_section_from_image(image, "es_serreg_dict");
    if (!bytes)
        return std::unexpected(bytes.error());
    dyn::dictionary d;
    const auto r = d.add_entries(*bytes);
    if (!r)
        return std::unexpected(r.error());
    return d;
}

result<dyn::dictionary> extract_embedded_dictionary(const std::string_view path)
{
    const auto img = read_file(path);
    if (!img)
        return std::unexpected(img.error());
    return extract_embedded_dictionary(*img);
}

result<dyn::dictionary> extract_registry_from_elf(const std::string_view path)
{
    return extract_embedded_dictionary(path);
}

} // namespace es::serreg::tools
