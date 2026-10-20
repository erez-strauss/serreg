# `src/elf_reader.cpp`

A self-contained ELF64 little-endian section reader (no libelf) and the offline half of the ELF image backend
(spec §6.8, FR-D6, FR-D5; review addendum A4 — the load-time half is `src/image_elf.cpp`). It implements
`tools::extract_section_from_image()`, `extract_section_from_elf()`, `extract_embedded_dictionary()` (path and
in-memory image: the executable format is recognised by its magic, the `es_serreg_dict` section →
`dyn::dictionary`) and `extract_registry_from_elf()`, a wrapper kept for existing callers and `srdump --elf`. It
refuses anything that is not ELFCLASS64 + ELFDATA2LSB, never computes a file offset without an overflow check,
caps the section count by what the image can hold, and never maps or executes anything — it reads bytes and
slices spans. The file helpers (`read_file`, `write_file`, both `write_all`s and `slurp`) live in
`src/file_io.cpp`.

## Depends on / used by

Includes: `internal.hpp`, `es/serreg/tools.hpp`. From `format.hpp`: `fmt::cursor` (the little-endian field reader
is reused for ELF headers). From `dictionary.hpp`: `dyn::dictionary::add_entries`. From `file_io.cpp`:
`tools::read_file` (with its 1 GiB cap) for the path overloads.

Used by: `src/cli.cpp` (`--elf` and `--diff` on a binary, through `extract_registry_from_elf`),
`tests/fuzz/fuzz_targets.cpp` (`fuzz_elf` → `extract_section_from_image` and
`extract_embedded_dictionary(bytes)`), `tests/unit/runtime_selftest.cpp` (in-memory image, then
`/proc/self/exe`), `tests/unit/test_capture.cpp` ("FR-D6 extract_registry_from_elf(/proc/self/exe) matches the
compile-time hashes"), `tests/link/image_none_main.cpp` (no section → `not_found`), the `srdump_elf_*` ctests.

## Structure

- **`section_header`** (`elf_reader.cpp:17-24`): the five fields the reader needs (`name`, `type`, `offset`,
  `size`, `link`).
- **`read_shdr()`** (`elf_reader.cpp:28-41`): section header `i`, overflow-checked.
- **`name_is()`** (`elf_reader.cpp:46-52`): whether the NUL-terminated name at a string-table offset is
  exactly the wanted name; replaces `cstr_at()`.
- **`find_section()`** (`elf_reader.cpp:54-108`): ELF header → section table → string table → linear scan.
- **Public section extractors** (`elf_reader.cpp:112-129`): `extract_section_from_image` (with the dotted-name
  retry), `extract_section_from_elf`.
- **Dictionary extractors** (`elf_reader.cpp:131-159`): `extract_embedded_dictionary(span)` (magic dispatch, then
  section → `add_entries`), `extract_embedded_dictionary(path)`, `extract_registry_from_elf(path)` (wrapper).

## How it works

### Recognising the format

```cpp
if (image.size() < 4 || image[0] != std::byte{0x7f} || image[1] != std::byte{'E'} ||
    image[2] != std::byte{'L'} || image[3] != std::byte{'F'})
    return std::unexpected(image.size() < 4 ? error::truncated : error::unsupported);
const auto bytes = extract_section_from_image(image, "es_serreg_dict");
```

(`elf_reader.cpp:135-138`.) `extract_embedded_dictionary` looks at the first four bytes: `\x7fELF` goes to the
section reader, any other format — PE (`MZ`), Mach-O, a raw blob — is `error::unsupported`, because ELF is the only
format with a reader; fewer than four bytes is `truncated`. The section bytes, a bare ENTRY stream with no RRI
header, go to `dyn::dictionary::add_entries`. The path overload is `read_file` + the span overload;
`extract_registry_from_elf(path)` returns `extract_embedded_dictionary(path)` (`elf_reader.cpp:156-159`), so
`srdump --elf` and existing callers get the same results; a non-ELF input is `unsupported`.

| input to `extract_embedded_dictionary` | result |
|---|---|
| fewer than 4 bytes | `truncated` |
| first 4 bytes not `\x7fELF` | `unsupported` |
| ELF, shorter than the 64-byte header | `truncated` |
| ELFCLASS32 | `unsupported` |
| big-endian | `endianness_mismatch` |
| `e_shoff == 0` (stripped), or no `es_serreg_dict`/`.es_serreg_dict` section (a program built with `ES_SERREG_IMAGE_FORMAT=none`) | `not_found` |
| any header/table/section window outside the image | `truncated` |
| section of type `SHT_NOBITS` | an empty dictionary |
| section present, ENTRY stream malformed | the parser's error (`corrupt_dictionary`, …) |
| path overload: file missing / unreadable / over 1 GiB | `not_found` / `io_error` / `unsupported` (from `read_file`) |

`runtime_selftest` checks the first rows (an in-memory ELF with 3 types and 1 call, an `MZ` blob, 3 bytes);
`link_image_none_backend` checks `not_found` on an executable without the section.

### Finding `es_serreg_dict`

```cpp
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
```

(`elf_reader.cpp:63-75`.) Steps of `find_section` (`elf_reader.cpp:54-108`): (1) 64-byte header present,
`\x7fELF` magic (`bad_magic` when called directly through `extract_section_from_image`), class 64, little-endian;
(2) read `e_shoff … e_shstrndx` from offset 40 with `fmt::cursor`; a stripped binary with `shoff == 0` is
`not_found`, not an error; (3) read section header 0 — it carries the extended counts: `shnum == 0` means the
real count is `sh0.size`, `shstrndx == 0xffff` (SHN_XINDEX) means the string table index is `sh0.link`;
(4) locate the section-name string table and check its window; (5) scan headers 1..n, comparing
`name_is(names, sh.name, section)`; (6) an `SHT_NOBITS` (8) match returns an empty vector, a
real match is bounds-checked and copied out. `extract_section_from_image` retries with a leading dot
(`.es_serreg_dict`) when the plain name is not found, since some toolchains and documents spell it that way.

### Bounds and overflow checks

```cpp
bool read_shdr(const std::span<const std::byte> img, const std::uint64_t shoff,
               const std::uint16_t shentsize, const std::uint64_t i, section_header& out)
{
    std::uint64_t rel{}, pos{};
    if (shentsize < 64 || __builtin_mul_overflow(i, std::uint64_t{shentsize}, &rel) ||
        __builtin_add_overflow(shoff, rel, &pos))
        return false;
    if (pos > img.size() || img.size() - pos < 64)
        return false;
```

(`elf_reader.cpp:28-41`.) `shoff + i × shentsize` is computed with `__builtin_mul_overflow` /
`__builtin_add_overflow` so an attacker-chosen `shoff` near 2^64 cannot wrap back into the image; the entry
must be at least 64 bytes and lie inside the image. The section count is capped:

```cpp
// The table cannot hold more headers than the image has bytes for (gk 1.15: sh0.size is attacker-chosen).
if (n_sections > img.size() / shentsize)
    return std::unexpected(error::truncated);
```

(`elf_reader.cpp:85-87`.) Without this a 64-bit `sh0.size` would make the scan loop run until the first
out-of-range header — bounded, but slow. Every section window (`strtab`, the found section) is checked as
`offset ≤ size && size - offset ≥ len`, never `offset + len ≤ size`.

A section name is compared, not extracted (D-04):

```cpp
bool name_is(const std::span<const std::byte> names, const std::uint64_t pos, const std::string_view want)
{
    if (pos >= names.size() || names.size() - pos <= want.size())
        return false;
    const char* const p = reinterpret_cast<const char*>(names.data() + pos);
    return std::memcmp(p, want.data(), want.size()) == 0 && p[want.size()] == '\0';
}
```
(`elf_reader.cpp:46-52`.) It reads at most `want.size() + 1` bytes: the name's bytes and its terminating NUL,
which must lie inside the string table. Scanning each name to its NUL (or the table's end) before comparing would
let a crafted string table — one long name, many section headers pointing into it — make the lookup quadratic:
20 000 headers over a 1 MiB name would cost about 2 × 20 G compares (`runtime_selftest.cpp`
`review_2_1_10_decoders()` checks that the lookup of such an image finishes in seconds).

### What `extract_section_from_image` refuses

| input | result |
|---|---|
| shorter than 64 bytes | `truncated` |
| not `\x7fELF` | `bad_magic` |
| ELFCLASS32 | `unsupported` |
| big-endian | `endianness_mismatch` |
| `e_shoff == 0` (stripped) or no section of that name | `not_found` |
| any header/table/section window outside the image | `truncated` |

The path variant `extract_section_from_elf` adds `read_file`'s errors (input over 1 GiB → `unsupported`). The
reader does not parse program headers, symbols, relocations or notes, and does not understand compressed sections
(`SHF_COMPRESSED`) — the `es_serreg_dict` section is plain read-only data emitted by `detail/embed_elf.hpp`.

### Why the section has no RRI header

The section is a concatenation of TU-local ENTRY copies, not an export, so such a dictionary has
`program_hash == 0` and `srdump --program-hash --elf` falls back to `computed_program_hash()`
(`golden_srdump_program_hash_elf_agrees`).

## Maintaining it

- **Tests:** `runtime_selftest` (section reader on a hand-built image, `e_shnum`/`e_shoff` attacks, truncation,
  `extract_embedded_dictionary` on ELF / `MZ` / 3 bytes), `test_capture.cpp` (FR-D6 over `/proc/self/exe`),
  `srdump_elf_premain`, `srdump_elf_serreg_tests`, `srdump_elf_no_skipped_type`,
  `srdump_elf_conflicting_plugins_*`, `golden_srdump_program_hash_elf_agrees`, `link_image_none_backend`, and
  `fuzz_elf` with `tests/fixtures/corpus/elf/minimal.o` (run under ASan/UBSan after any change — `CLAUDE.md`'s
  rule for the ELF reader).
- **Keep the checks in this order:** compute with overflow builtins, compare against `img.size()`, then
  `subspan`. Never form `offset + size` in plain arithmetic on wire-supplied values.
- **Do not add libelf or `mmap`** — no new dependencies, and the reader must work on an in-memory image for
  the fuzz target and embedders.
- **A new executable format** is a new branch on the magic in `extract_embedded_dictionary` plus its own reader;
  until then it stays `unsupported`. Keep `extract_registry_from_elf` a one-line wrapper.
- **The section name is `es_serreg_dict` without a dot** (`detail/embed_elf.hpp`); the dotted retry is a courtesy
  for other producers, not a second canonical name.
- **No file I/O here** beyond calling `read_file`; new helpers go to `src/file_io.cpp`.
- **Wire format:** none here — the section's *content* is the ENTRY stream owned by `format.hpp`/
  `dictionary.cpp`; this file only finds and copies it.
- **Allocation:** tools layer; the section is copied into a vector, the path overloads read the whole file (up to
  1 GiB).
- **Threading:** all functions are reentrant; no shared state.
