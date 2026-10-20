# `include/es/serreg/hash.hpp`

The fingerprint of the whole library and the small constexpr string helpers used to build canonical
descriptions. Every 64-bit identity in `es::serreg` — schema hash, layout hash, literal id, call hash, the
`"string"` and scalar-name argument hashes, the program hash, the blob hashes frozen in the golden tests — is
**XXH3-64** (seed 0, default secret) over bytes produced by the functions in this header. It belongs to the
**wire-format layer** (it defines hash algorithm id 2 of the FILE/RRI headers, spec FR-H5, since format 1.3), has no
reflection dependency, includes nothing from the library, and every function in it is usable in constant
evaluation.

For format 1.7 it also holds **CRC32C**, the integrity check of a frame's optional trailer — not an
identity: it is never part of a name, a schema or a dictionary.

Formats 1.0–1.2 used FNV-1a-64 (`hash_algo` 1). XXH3-64 replaced it everywhere and FNV was removed from the code:
no reader computes FNV, so files of format ≤ 1.2 are refused (`bad_version`), see chapter 7 §7.7.

## Depends on / used by

Includes (standard library only): `<array>`, `<bit>`, `<cstddef>`, `<cstdint>`, `<span>`, `<string>`,
`<string_view>`, `<vector>`.

Included by:

- `include/es/serreg/reflect.hpp` — `fingerprint64` over the canonical strings (`schema_hash_of`,
  `layout_hash_of`), `append_hex`/`append_uint`/`append_int` inside `append_kindspec`, `append_dims`,
  `render_value_arg`.
- `include/es/serreg/literal.hpp` — `fixed_string` (the NTTP of `lit<S>`), `fingerprint64` for the literal id.
- `include/es/serreg/arg.hpp` — `string_arg_hash = fingerprint64("string")`, `fingerprint64(arg_name<T>())`.
- `include/es/serreg/blob.hpp` — `call_hash_v` is `fingerprint64(call_string<...>())`.
- `src/dictionary.cpp` — `fingerprint64_u64s` for `computed_program_hash()`, `fingerprint64` for the ARG checks.
- `src/reader.cpp` — `fingerprint64("string")` when inferring a full record's category.
- `src/registry.cpp` — `fingerprint64_u64s` in `registry::program_hash()` and `export_image()`.
- `src/wireshark.cpp` — `fingerprint64` of every scalar kind name (the `SCALARS` table of full records) and
  of `"string"` (`SERREG.string_hash`) in the generated dissector.
- `include/es/serreg/serialize.hpp`, `include/es/serreg/deserialize.hpp`, `src/reader.cpp` — `crc32c` for
  the frame trailer (written by `put_crc_trailer`, checked by `read_frame_header` and by the frame walkers'
  `check_crc`). The generated Wireshark dissector (`src/wireshark.cpp`) shows the stored trailer but does not
  recompute it.
- `tests/golden/hashes.cpp`, `tests/golden/blobs.cpp`, `tests/unit/test_hash.cpp`, `tests/unit/test_format17.cpp`.

## What it defines

All in `namespace es::serreg` unless noted.

| name | kind | meaning |
|---|---|---|
| `fingerprint64(std::string_view)` | `constexpr` function | XXH3-64 of the chars — every canonical string, literal and kind name |
| `fingerprint64(std::span<const std::byte>)` | `constexpr` function | the same over raw bytes (blob hashes in the golden tests) |
| `fingerprint64_u64s(std::span<const std::uint64_t> sorted)` | `constexpr` function | XXH3-64 of the values' little-endian bytes in order; callers pass a *sorted* range → order-independent program hash (FR-H3) |
| `detail::xxh3::secret` | `u8[192]` | the XXH3 default secret (xxHash `XXH3_kSecret`) |
| `detail::xxh3::hash(const C*, len)` | `constexpr` template | the algorithm; `C` is `char`, `unsigned char` or `std::byte` |
| `detail::xxh3::fold`, `avalanche`, `avalanche64`, `rrmxmx`, `mix16`, `rd64`, `rd32` | `constexpr` | the XXH3 building blocks, named as in the reference |
| `detail::append_hex(std::string&, std::uint64_t)` | `constexpr` function | appends `0x` + exactly 16 lowercase hex digits |
| `detail::append_uint(std::string&, std::uint64_t)` | `constexpr` function | appends the decimal digits |
| `detail::append_int(std::string&, std::int64_t)` | `constexpr` function | signed decimal; handles `INT64_MIN` without overflow |
| `crc32c(std::span<const std::byte>, seed = 0)`, `crc32c(std::string_view, seed = 0)` | `constexpr` functions | CRC32C (Castagnoli) — the frame trailer of format 1.7; the software table in constant evaluation, the SSE4.2 `crc32` instruction at run time when the CPU has it |
| `detail::crc32c_impl::table`, `soft`, `hw`, `have_hw` | `constexpr` table and functions | the 256-entry table of the reflected polynomial `0x82F63B78`, the byte-wise loop, the SSE4.2 loop (8 bytes per step), the once-per-process CPU check |
| `fixed_string<N>` | class template (structural NTTP) | `char data[N]` copied from a string literal; `view()`, `size()` (both exclude the terminator) |

The algorithm id itself, `fmt::hash_algo_xxh3_64 = 2`, lives in `format.hpp` with the other header constants.

## How it works

### XXH3-64

An independent scalar implementation of XXH3-64 with seed 0 and the default secret; the algorithm and the secret
are from xxHash by Yann Collet (BSD 2-Clause, credited in the header). Nothing is vendored: the library still has no
dependencies. Inputs are split the way the reference splits them — 0, 1–3, 4–8, 9–16, 17–128, 129–240 and
> 240 bytes (the last as 64-byte stripes accumulated into eight lanes, scrambled every 1024 bytes). Canonical
schema strings are typically 140–600 bytes, call strings about 50.

One function serves constant evaluation and run time. The only operation that differs is the 8- and 4-byte load:

```cpp
template<class C> constexpr u64 rd64(const C* const p) noexcept
{
    if consteval
    {
        u64 v = 0;
        for (int i = 7; i >= 0; --i)
            v = v << 8 | static_cast<u8>(p[i]);
        return v;
    }
    else
    {
        u64 v;
        __builtin_memcpy(&v, p, 8);
        return v;
    }
}
```
(`hash.hpp:54`). `memcpy` is not a constant expression, so the compile-time branch assembles the word from bytes;
the run-time branch is one unaligned load. Without the split the run-time path would be slower than the FNV it
replaced on short strings (measured: 80 ns vs 37 ns at 50 bytes); with it, 39 ns at 50 bytes and about 1.4–1.9×
faster than FNV from 300 bytes up (sandbox numbers). The header asserts a
little-endian host, which the whole format already requires.

The 64×64→128-bit multiply (`fold`, `hash.hpp:89`) uses `unsigned __int128` where the compiler has it — declared
through `__extension__ typedef` so `-Wpedantic` accepts it — and a portable four-multiply fallback otherwise. Both
paths are tested (the fallback by building with `__SIZEOF_INT128__` undefined).

Compile-time cost: in GCC 16's constant evaluator one 300-byte schema string costs about 1.1 ms (FNV cost 1.6 ms).
Hashing is well under 1 % of a build: a 150-type translation unit took the same time with XXH3, FNV, or a no-op
"hash".

### Program hash

`fingerprint64_u64s` (`hash.hpp:221`) hashes the values' eight little-endian bytes each, in the given order. The
result is what `registry::program_hash()` and `dyn::dictionary::computed_program_hash()` compute over the sorted
schema hashes of all type entries, and what the RRI/FILE headers carry. Order independence comes from the caller
sorting, not from the function — `tests/unit/test_registry.cpp` checks that a reversed range hashes differently,
i.e. that the sort is load-bearing. In constant evaluation it copies the bytes into a transient `std::vector`; at run
time it hashes the array in place.

### Canonical string helpers

The three `detail::append_*` functions exist so that canonical strings never depend on `std::to_string`,
`std::format` or locale: `append_hex` always writes 18 characters (`0x` + 16 digits, zero-padded, lowercase),
which is why a schema hash inside another type's schema string (`struct<0x367f8cd8c6441dd2>`) has a fixed width.
`append_int` writes `-(v + 1)` then adds one back to avoid negating `INT64_MIN`:

```cpp
constexpr void append_int(std::string& out, const std::int64_t v)
{
    if (v < 0)
    {
        out += '-';
        append_uint(out, static_cast<std::uint64_t>(-(v + 1)) + 1);
    }
    else
        append_uint(out, static_cast<std::uint64_t>(v));
}
```
(`hash.hpp:355`). It is used for enumerator values (`enum_string`) and for signed template-argument values in
canonical names (`t::W<i32,-5>`, locked in `tests/golden/hashes.cpp`).

### `fixed_string<N>`

```cpp
template<std::size_t N> struct fixed_string
{
    char data[N]{};
    constexpr fixed_string(const char (&s)[N]) { for (std::size_t i = 0; i < N; ++i) data[i] = s[i]; }
    constexpr std::string_view view() const noexcept { return {data, N - 1}; }
    static constexpr std::size_t size() noexcept { return N - 1; }
};
```
(`hash.hpp:368`). It is a *structural* type (public array member, no user-provided special members), so it can be a
non-type template parameter: `lit<"order rejected">`, `serialize<"evt">(out, ...)`, `print<"x = {}">(...)`
and the UDLs `operator""_lit` / `operator""_` all deduce `fixed_string<N>` from a string literal. Two literals with
the same text produce the same specialisation (template argument equivalence is by value for structural types),
which is the property that makes the four literal spellings one literal (`literal.hpp`). `N` includes the NUL, so
`size()` and `view()` subtract one; the terminator is therefore never hashed and never enters an ENTRY blob.

### Which hash is which

Every 64-bit identity in the system is one of these; all come from this header.

| identity | input to `fingerprint64` | produced in | consumed by |
|---|---|---|---|
| schema hash | canonical schema string (`struct<Name>[…]{name:kind@off[…];…}`) | `reflect.hpp::schema_hash_of` | ENTRY, FIELD `type_hash`, call-string item `0x…`, full-record header, `registry::find_type` |
| layout hash | canonical layout string (names stripped) | `reflect.hpp::layout_hash_of` | ENTRY; FR-R4 conflict detection |
| literal id | the literal's bytes | `literal.hpp::lit<S>::id` | literal ENTRY, ARG hash, full-record payload, `find_literal` |
| call hash | the call string `call(n){…}` / `print(n){…}` / `println(n){…}` | `blob.hpp::call_hash_v` | frame header, call ENTRY, `find_call` |
| scalar argument hash | the kind name (`"i32"`) | `arg.hpp::arg_hash` | ARG hash, full-record header |
| string argument hash | `"string"` = `0xd8ad194648e774f1` | `arg.hpp::string_arg_hash` | ARG hash, full-record header, `frame_view::decode` category inference |
| program hash | the sorted schema hashes, via `fingerprint64_u64s` | `registry::program_hash`, `dictionary::computed_program_hash` | RRI/FILE headers, `srdump --program-hash` |
| blob hash (tests only) | the ENTRY bytes | `tests/golden/blobs.cpp` | format lock |

A hash never crosses the wire in any other form than these eight little-endian `u64`s (the 1.7 CRC32C trailer is
a 32-bit checksum of a frame's bytes, not an identity). They are fingerprints, not
authentication: a 64-bit non-cryptographic hash can be collided on purpose.

### CRC32C

```cpp
constexpr std::uint32_t crc32c(const std::span<const std::byte> s, const std::uint32_t seed = 0) noexcept
{
    if consteval
    {
        std::vector<unsigned char> b(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            b[i] = static_cast<unsigned char>(s[i]);
        return detail::crc32c_impl::soft(seed, b.data(), b.size());
    }
    else
    {
        const auto* const p = reinterpret_cast<const unsigned char*>(s.data());
        return detail::crc32c_impl::have_hw() ? detail::crc32c_impl::hw(seed, p, s.size())
                                         : detail::crc32c_impl::soft(seed, p, s.size());
    }
}
```
(`hash.hpp:302-317`.) The standard CRC32C (iSCSI, RFC 3720): initial value and final XOR `0xFFFFFFFF`, reflected
polynomial `0x82F63B78`, check value `crc32c("123456789") == 0xE3069283` (`STATIC_REQUIRE`d in
`tests/unit/test_format17.cpp`). The table is built by `make_table()` at compile time (`hash.hpp:246-258`). At run
time `have_hw()` asks `__builtin_cpu_supports("sse4.2")` once (a function-local static) and `hw()` — compiled with
`[[gnu::target("sse4.2")]]`, so the library needs no `-msse4.2` — consumes 8 bytes per `crc32` instruction
(`hash.hpp:267-282`); other targets use the table. Both paths give the same value, so a file written on one
machine checks on any other. The pure-Python reader (`python/serreg/format.py`) carries its own table-driven
implementation; the generated Wireshark dissector has none.

The trailer it fills is `u32 crc32c(frame[0, frame_len - 8))`, `u32 0` (format.hpp); `seed` lets a caller continue
a checksum across pieces, which the library itself does not need (a frame is checked in one piece).

### Worked example

`"evt"_lit` has `id = fingerprint64("evt") = 0x198e4c6da0847235` (the 1–3-byte path: the three bytes and the
length are combined into one 32-bit word, XORed with secret bytes 0–7 and avalanched). The golden tests assert
`decltype("evt"_lit)::id == fingerprint64("evt")` and freeze the *blob* containing it (`0x52155f6f3763912f`,
32 bytes). `tests/unit/test_hash.cpp` pins known answers of the reference xxHash 0.8.3 for every length class, in
`STATIC_REQUIRE` and at run time, and checks that the two code paths agree for every length 0–299. The program
hash of two types `A` and `B` is `fingerprint64_u64s({min(hA,hB), max(hA,hB)})` — the same whichever TU registered
first (`link_program_hash_order_independent`).

## Maintaining it

- **Never change the algorithm, the secret or the seed.** `hash_algo_xxh3_64 = 2` in `format.hpp` names this exact
  function; every value in `tests/golden/hashes.cpp` and `tests/golden/blobs.cpp`, every checked-in fixture under
  `tests/fixtures/golden/` and the canary (`tests/canary/rri_canary.cpp`, which decodes a fixture with no library
  header at all) depend on it. A different hash is a new `hash_algo` value and a `format_minor` bump.
- **The known-answer vectors are the contract.** If `test_hash.cpp` fails, the implementation is wrong, not the
  vectors: they come from the reference implementation.
- **`append_hex` width is part of the canonical strings.** `struct<0x…>` / `dyn<0x…>` / `lit:0x…` items are hashed
  with the zero padding; shortening the output changes every nested schema hash and every call hash.
- **`append_int` semantics are part of enum schema strings** (`enum<corpus::Side,u8>[1,1]{buy=1;sell=2;}`); the
  golden hashes lock them.
- `fingerprint64_u64s` byte order (LSB first) is what `srdump --program-hash` recomputes from a dictionary and
  compares against the header; `golden_srdump_program_hash_agrees` / `golden_srdump_program_hash_elf_agrees`
  exercise both sides.
- `fixed_string` must stay structural (no private members, no constructors that would make it non-literal) or every
  `lit<S>` and `serialize<"...">` spelling stops compiling.
- **CRC32C is format too** (1.7): the polynomial, the initial value and the final XOR are fixed by the trailers in
  `tests/fixtures/golden/features.ser`; the hardware and software paths must agree byte for byte.
- Tests: `tests/unit/test_hash.cpp` ("FR-H5 xxh3-64 known answers", "FR-H5 xxh3-64 consteval and run-time paths
  agree", "FR-H3 program hash is xxh3-64 of the sorted u64s"), `tests/golden/hashes.cpp` ("FR-H1 schema hash is
  XXH3-64 of the canonical schema string"), `tests/unit/test_registry.cpp` ("FR-H3 program_hash is the
  order-independent XXH3-64 of the sorted type hashes"), `tests/unit/test_roundtrip.cpp` ("FR-L1 the four literal
  spellings are one literal").
