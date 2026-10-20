# `include/es/serreg/deserialize.hpp`

The in-process readers (spec §6.4, FR-S4/S5) — four of the five reader styles live here; the fifth
(`tools::visit_record`, the descriptor-driven dynamic visitor) is in `tools.hpp` because it needs no C++ types.
Every reader starts from `read_frame_header`, validates before it touches an output, and bounds-checks every
length it takes from the wire (FR-D5). This is the **readers layer**; it is reflection-dependent through
`registry.hpp` (`touch_arg`, `descriptor_v`, `decode_image` in `reflect.hpp`).

Two properties shape the readers. The typed readers (`deserialize`, `deserialize_print`, `deserialize_println`) return
`decode_result<std::size_t>`: on failure a `decode_error` with the code, the number of items expected, the number
decoded and the first item that failed (`error.hpp`, decision C). And every reader knows compile-time constants
(format 1.5): a typed reader names the same constant, the others see it as the scalar or enum record it stands for,
marked `record_view::is_const`.

```cpp
//   1. deserialize(bytes, T&...)             typed, whole objects; validates before touching outputs
//   2. inspect(bytes[, dictionary])          frame_view: header + a lazy record range, no copies, no record-count cap
//   3. dispatch<Ts...>(bytes, f[, dict])     compile-time match of each record against Ts; zero-copy when aligned
//   4. dynamic visitor                       tools::visit_record (tools.hpp) over a dyn::dictionary — used by srdump
//   5. view<T>(record_view)                  zero-copy typed pointer when alignment allows, error::misaligned otherwise
```
(`deserialize.hpp:1`).

## Depends on / used by

Includes: `<algorithm>` (`std::ranges::stable_sort`, `lower_bound`), `<array>`, `<bit>` (`std::bit_ceil`),
`<cstddef>`, `<cstdint>`, `<cstring>`, `<iterator>`, `<memory>`, `<new>` (`std::launder`,
`std::start_lifetime_as`), `<ranges>` (`std::ranges::subrange`), `<span>`, `<string>`, `<string_view>`, `<tuple>`,
`<type_traits>`, `<vector>` (the compile-time search of `hash_index`), `es/serreg/dictionary.hpp` (side dictionaries), `es/serreg/error.hpp`, `es/serreg/format.hpp`,
`es/serreg/registry.hpp`, `es/serreg/serialize.hpp` (`frame_size`, `record_payload_size`, `arg_hash`).

Included by: `serreg.hpp`, `walk.hpp`, `router.hpp` (`frame_router` decodes with
`detail::deserialize_frame`), `src/formatters.cpp` (only `read_frame_header`), `tests/support/test_support.hpp`
(`all_records(frame_view)`). The compiled decoder (`src/reader.cpp`, `src/decoder.cpp`) does *not* use these
templates; it walks frames from dictionaries.

## What it defines

| name | kind | meaning |
|---|---|---|
| `frame_header` | struct | `frame_len, flags, n_records, call_hash`; `full()`, `dictionary()` (flag *or* sentinel); (1.7) `meta()` (a dictionary, checkpoint or writer-info frame — no records), `crc()` (the frame ends in a CRC32C trailer), `payload_end()` (`frame_len` minus the trailer) (`deserialize.hpp:39-67`) |
| `read_frame_header(bytes)` | function | validates size ≥ 16, `frame_len` ≥ 16 and % 8, `frame_len ≤ bytes`, known flags only (else `bad_version`) |
| `literal_view` | struct | `id, text` (empty when unknown) — what `dispatch` delivers for a literal |
| `record_view` | struct | `hash, is_literal, desc (type_desc*), dyn_desc (dyn::type*), payload, ak, k, is_const` (1.5); `text()` |
| `detail::resolver` | struct | call/type lookup through the registry first, then a side `dyn::dictionary`; `n_args`, `arg_kind(i)`, `arg_hash(i)`, `arg_value(i)` and `const_size(i)` (1.5), `type_size`, `is_dynamic`, `resolve_call` |
| `frame_view` | class | style 2: `header`, `size()`, `begin()/end()`, `records()` (input iterator over `record_view`) |
| `inspect(bytes, dict = nullptr)` | function | builds a validated `frame_view` |
| `view<T>(const record_view&)` | function template (`flat_type T`) | style 5: `result<const mapped_t<T>*>` |
| `detail::get_object`, `is_owning_string_v`, `string_out`, `validate_frame`, `get_record`, `deserialize_frame` | helpers | the typed-reader engine; `string_out` accepts `std::string_view` and any `std::basic_string<char, char_traits<char>, A>` |
| `deserialize(bytes, Ts&&... outs)` | function template | style 1; literals as `lit<S>` values, constants as `constant<V>` values (`42_`), everything else non-const lvalues; returns `decode_result<std::size_t>` |
| `deserialize_print(bytes, lit<F>, outs...)` / `deserialize_println` | function templates | typed readers for print frames (1.1); the same return type |
| `deserialize_raw(bytes, T& out)` | function template (`flat_type T`) | payload only (FR-S5) |
| `detail::hash_index<Keys...>` | class template | compile-time perfect hash from a record hash to its position in `Keys...`; `lay` (`m`, `shift`, `perfect`), `slots`, `sorted` (fallback), `find(h) -> int` (position or -1) |
| `detail::call_for_index<Ts...>(i, l)` | function template | calls `l(std::type_identity<T_i>{})` through a `static constexpr` table of function pointers |
| `detail::dispatch_linear_max` | `inline constexpr std::size_t` = 4 | up to this many types `dispatch` compares linearly; above it, one `hash_index` lookup |
| `dispatch<Ts...>(bytes, F&& f, dict = nullptr)` | function template | style 3 |

## How it works

### Header validation, once

```cpp
if (h.frame_len < fmt::frame_header_size || h.frame_len % 8)
    return std::unexpected(error::length_mismatch);
if (h.frame_len > bytes.size())
    return std::unexpected(error::truncated);
if (h.flags & ~(fmt::frame_flag_full | fmt::frame_flag_dict | fmt::frame_flag_crc |
                fmt::frame_flag_checkpoint | fmt::frame_flag_info))
    return std::unexpected(error::bad_version); // 1.1: unknown frame flags are not guessed at
if (h.crc())
{ // 1.7: the trailer is checked before anything in the frame is believed
    if (h.frame_len < fmt::frame_header_size + fmt::crc_trailer_size)
        return std::unexpected(error::length_mismatch);
    std::uint32_t stored{};
    std::memcpy(&stored, bytes.data() + h.frame_len - fmt::crc_trailer_size, 4);
    if (crc32c(bytes.first(h.frame_len - fmt::crc_trailer_size)) != stored)
        return std::unexpected(error::crc_mismatch);
}
```
(`deserialize.hpp:79-94`). The flags line is the 1.1 rule: an unknown flag means a newer writer, refuse rather than
guess; the five known bits are full records, dictionary, CRC32C trailer, checkpoint and writer info (1.7). A
frame with the CRC flag (bit 2) is checked before anything in it is believed: the last 8 bytes are
`u32 crc32c(frame[0, len-8))` and `u32 0`, and a mismatch returns `error::crc_mismatch` — no output is written. `frame_header::dictionary()` accepts either the flag or `call_hash == dict_frame_sentinel`; `meta()` adds the
checkpoint and writer-info frames of 1.7. `inspect` returns a `frame_view` with no records for a meta frame
(`deserialize.hpp:385-386`); `deserialize` on one is `hash_mismatch` (`validate_frame`, `deserialize.hpp:539-545`).
Every record loop stops at `payload_end()`, so the trailer is never read as a record.

### Style 1: `deserialize` validates, then reads

`deserialize_frame<Sig...>(bytes, outs...)`:

1. `static_assert` that every output that is neither a literal nor a constant is a non-const lvalue; `touch_arg<Sig>()...` and the call
   registrar (a reader registers the same entities as a writer — the dictionary of a pure consumer is complete too).
2. `read_frame_header`. A failure here is `decode_error{code, expected, 0, 0}`.
3. `validate_frame<Sig...>(h, bytes, outs, failed)`: a dictionary frame → `hash_mismatch`; `n_records` ≠ arity → `length_mismatch`.
   **Compact:** `call_hash` must equal `call_hash_v<Sig...>` (one 64-bit compare validates the whole signature),
   then one cursor walks every record in order — a string or dynamic record by its own `u32 len`, everything else
   by its constant size — and each step must fit before `frame_len` (`truncated`). **Full:** the same cursor walks
   the record headers: 16 header bytes must be left, each `hash` must equal `arg_hash<T>()`, each `len` must equal
   `record_payload_size<T>()` (flat, scalar, literal) or, for a string/image, be at least the prefix *and* agree
   with the record's own inner `u32 len` (`length_mismatch`), and the padded payload must fit (`truncated`).
   A constant (1.5) costs no bytes in a compact frame; in a full frame its record must have the hash and length of
   the value it stands for, and the payload must equal the value the reader names (`hash_mismatch` otherwise).
   The function sets `failed`: 0 while it checks the frame itself, then the 1-based number of the
   record it is checking, so the caller knows which record did not fit.
   (G2-1: comparing `frame_len` only with the constexpr *minimum* would let a string that claimed more bytes make
   the next record's `memcpy` read past `frame_len`.)
4. `get_record<T>` per output, in order, with `p` and `end`:

```cpp
else if constexpr (is_string_arg_v<U>)
{
    static_assert(string_out<U>, "es::serreg::deserialize: a string record is read into std::string (any allocator) or std::string_view");
    if (static_cast<std::size_t>(end - p) < fmt::string_len_prefix) return std::unexpected(error::truncated);
    std::uint32_t len; std::memcpy(&len, p, 4);
    if (static_cast<std::size_t>(end - p) < fmt::align8(fmt::string_len_prefix + len)) return std::unexpected(error::truncated);
    const char* const text = reinterpret_cast<const char*>(p + fmt::string_len_prefix);
    if constexpr (std::is_same_v<U, std::string_view>)
        out = U{text, len};
    else
        out.assign(text, len); // keeps the output's allocator (std::pmr::string)
    p += fmt::align8(fmt::string_len_prefix + len);
}
```
(`deserialize.hpp:692`). `get_record` keeps its own bounds checks against `end` as defence in depth; after
`validate_frame` they cannot fail. A compact literal consumes nothing; a
full-mode literal's `u64 id` is compared to `U::id`. A constant (1.5) consumes nothing in a compact frame and is
skipped in a full one (`deserialize.hpp:661-667`): `validate_frame` already compared its value. A scalar or flat object is copied by `get_object` (through
`as_flat::from` when mapped), which repairs any `bool` byte other than 0/1 to 1 and reports
`warning::noncanonical_bool` (review G2-12; `copy_flat` in `reflect.hpp`). The range check for enums without a fixed
underlying type (decision D, FR-D5) is a **compile-time policy of the translation unit, off by default**:
`ES_SERREG_VALIDATE_READ=1` sets `detail::validate_read_enums` (`reflect.hpp`), and nothing in this header tests it
directly — `copy_flat` and `bools_canonical`, which `get_object`, `view`, `dispatch` and `deserialize_raw` all go
through, do. With it on, `get_object` returns `false` when a leaf whose type is such an enum held a value
outside the range of its enumerators' minimal-width integer (`detail::enum_ranges_v<U>`, `copy_flat`): that leaf
reads as 0 and `get_record` returns `invalid_value` after advancing past the record (`deserialize.hpp:730-737`).
With the default the tables are not instantiated and the bytes are copied as they are
(`tests/unit/test_validate_off.cpp`, built with the default; `serreg_tests` is built with the policy on).
A dynamic type is
`decode_image(out, span(p + 8, len))` after the `u32 len` was checked against `end`: a slot that breaks the rules
of `fmt::image_budget` is `length_mismatch`; a valueless variant that the output cannot hold (no
`std::monostate` alternative) is `invalid_value`, and so is such an enum leaf inside the image. `get_record`
advances past the record before returning
`invalid_value`, and `deserialize_frame` keeps it as a *soft* error: the remaining outputs are read and the call
returns `invalid_value` at the end, with `decode_error::failed` naming the first such argument.

Outputs are written only after `validate_frame` passed, so a frame whose records do not fit writes no output at
all. The one mid-way failure left is a dynamic image whose *contents* are corrupt (a slot pointing outside the
image, or over the byte budget): `decode_image` finds that while decoding, after earlier outputs were assigned.

**What a failure reports (decision C).** `deserialize_frame` returns `decode_result<std::size_t>`
(`deserialize.hpp:714`). `expected` is `1 + sizeof...(outs)`: the frame, then every argument, literals and constants
included (for `deserialize_print` the format literal is argument 1).

```cpp
std::uint16_t failed = 0;
if (const auto v = detail::validate_frame<Sig...>(
        *h, bytes, std::type_identity<std::tuple<std::remove_cvref_t<Ts>...>>{}, failed);
    !v) // a record that does not fit: the frame was accepted (1), nothing has been written
    return std::unexpected(decode_error{
        v.error(), expected, static_cast<std::uint16_t>(failed == 0 ? 0 : 1), failed});
```
(`deserialize.hpp:759-764`).

| stage that fails | `actual` | `failed` | outputs |
|---|---|---|---|
| `read_frame_header`, or `validate_frame` on the frame itself (dictionary frame, `n_records`, compact call hash) | 0 | 0 | none written |
| `validate_frame` on record *i* (length, hash, a full-records constant's value, a record past `frame_len`) | 1 | *i* | none written |
| `get_record` on argument *i*, a hard error (a corrupt dynamic image) | *i* | *i* | arguments before *i* complete, argument *i* valid but unspecified, later ones untouched |
| `get_record`, `invalid_value` on one or more arguments | `expected` − their number | the first of them | those outputs keep their old values, the rest are read |

There is one pass and no temporary copy of the outputs: the counters are two `std::uint16_t` in the decode loop
(`item`, `done`; `deserialize.hpp:767-794`). A wrong signature on a compact frame is the first row: the call hash
covers every literal and constant, so `deserialize(frame, "fill"_, 43_, o)` on a frame written with `42_` is
`{hash_mismatch, 4, 0, 0}`. On a full-records frame the same mistake is the second row, `{hash_mismatch, 4, 1, 2}`.
Tests: "FR-S4 a failure reports expected items, decoded items and the first that failed (2.1.15)"
(`tests/unit/test_roundtrip.cpp`), "FR-S4 a record that fails while it is decoded: expected, actual and the failed
item" (`tests/unit/test_dynamic.cpp`).

**`noexcept`** (G2-7): `deserialize`, `deserialize_print`, `deserialize_println` and `dispatch` are
`noexcept(detail::decode_noexcept<Outs...>)` — false as soon as an output is a dynamic type or a `std::string`
(decoding allocates, and `bad_alloc` must not become `std::terminate`), true for flat, scalar, `string_view` and
literal outputs.

### Style 2: `inspect` and `frame_view`

`inspect` validates every record once; iteration cannot fail afterwards:

- **Compact:** `resolve_call(call_hash)` through the registry then the side dictionary (`unknown_call` if
  neither); `n_args` must equal `n_records`; for each argument `compact_size(i, p)` — 0 for a literal and for a
  constant (1.5; a constant whose enum is unknown is `unknown_type`), `kind_size` for a scalar, `4 + u32 len` for a string, the type's size for a flat
  type, `8 + u32 len` for a dynamic type (after checking the 8-byte prefix is present); an unknown type has size 0
  → `unknown_type` (the frame cannot be walked past it); every record must fit before `end`.
- **Full:** each `u64 hash, u32 len` header must fit, the aligned payload must fit; for a string or a dynamic type
  the inner `u32 len` must agree with `payload_len − prefix` (`length_mismatch` otherwise).
- A dictionary frame yields a `frame_view` with zero records.

`frame_view::iterator` is an input iterator that re-derives each `record_view` on dereference (`decode`) and
advances by `advance` — no vector of records, no record-count cap (300-record frames are tested). `decode` fills
`hash`, `ak`, `k`, `is_literal`, `desc`/`dyn_desc` (via `resolver::type_size`) and `payload`: for a string the
bytes *after* the length prefix; for a dynamic type the image *after* the 8-byte prefix; for a full-records frame
without a known call the category is inferred (`string_arg_hash` → string, a registered literal id → literal, else
type). `record_view::text()` views a string payload as `std::string_view`.

**Constants (1.5).** `decode` never shows `arg_const` to the caller. It sets `is_const` and presents the record as
what it stands for: `ak = arg_scalar` with `k` when the ARG has a scalar kind, `ak = arg_type` (an enum) otherwise,
`hash` the ARG hash.

```cpp
if (r.ak == fmt::arg_const)
{ // no bytes in the frame: the value is in the call's entry
    r.is_const = true;
    r.ak = kind_size(res_.arg_scalar_kind(i)) != 0 ? fmt::arg_scalar : fmt::arg_type;
    p = res_.arg_value(i);
    len = res_.const_size(i);
}
```
(`deserialize.hpp:337-343`). In a compact frame the payload is not in the frame: `resolver::arg_value(i)`
(`deserialize.hpp:154-158`) points at the `u64 value` of the call's entry — `call_arg_desc::value` in the registry or
`dyn::call_arg::value` in the side dictionary — and `const_size(i)` (`deserialize.hpp:159-170`) is the size of the
value: `kind_size` for a scalar, the enum's size otherwise, 0 when the enum is unknown or the entry claims a type
larger than 8 bytes. The payload therefore lives as long as the registry or the dictionary, not the frame buffer.
In a full-records frame the constant was written as an ordinary record, so the payload is in the frame and only
`is_const` and `ak` are adjusted (`deserialize.hpp:301-305`). Either way `dispatch`, `view<T>`, `walk_values` and
the print renderer handle it like a run-time value of that type.

### Style 5: `view<T>`

```cpp
template<flat_type T> result<const detail::mapped_t<T>*> view(const record_view& r) noexcept
{
    if (r.is_literal || r.hash != schema_hash_v<T>) return std::unexpected(error::hash_mismatch);
    if (r.payload.size() != sizeof(U))              return std::unexpected(error::length_mismatch);
    if (reinterpret_cast<std::uintptr_t>(r.payload.data()) % alignof(U)) return std::unexpected(error::misaligned);
    // 2.1.3 (G2-12): a view cannot repair a bool byte other than 0/1 in place; copy with deserialize instead
    if (!detail::bools_canonical<U>(r.payload.data())) return std::unexpected(error::invalid_value);
#if defined(__cpp_lib_start_lifetime_as)
    return std::start_lifetime_as<U>(r.payload.data());
#else
    return std::launder(reinterpret_cast<const U*>(r.payload.data()));
#endif
}
```
(`deserialize.hpp:466`). Zero-copy is possible because `serialize` writes 8-aligned records of trivially copyable
bytes and `capture_file`/`span_sink` keep frames 8-aligned; a buffer at an odd address gives `misaligned`, never a
misaligned load. `view` is `flat_type` only; a dynamic record cannot be viewed (its C++ object does not exist in
the image). It also refuses a record whose `bool` bytes are not all 0/1 (`invalid_value`): handing out
such an object would be undefined behaviour on the first read of the member, and a view cannot repair bytes it
does not own. With the range check on (decision D; `ES_SERREG_VALIDATE_READ=1`) `bools_canonical` also refuses a record in which a leaf of an enum
without a fixed underlying type is outside its range (`enum_ranges_v<U>`): the same `invalid_value`, for the
same reason. The check reads one byte per `bool` leaf (`bools_v<U>`) and one integer per bounded enum leaf;
types without either pay nothing.

### Style 3: `dispatch<Ts...>`

For each record of `inspect(bytes, dict)`:

- literal → `f(literal_view{id, registry.find_literal(id)})` if `f` accepts it;
- string (1.1) → `f(std::string_view)` if invocable, else `f(const record_view&)`;
- otherwise the first `T` in `Ts...` with `r.hash == arg_hash_v<T>`: a scalar is copied out and delivered by
  value; a **dynamic** `T` is decoded into a temporary and delivered as `const T&` (never zero-copy; a decode
  error — `length_mismatch`, or `invalid_value` for a valueless variant or an out-of-range enum leaf — means the
  record is not delivered and
  the call returns that error after the walk); a flat `T` is delivered through `**view<T>(r)` when the payload is
  aligned, its `bool` bytes and bounded enum leaves are canonical and `T` has no `as_flat`, otherwise through a
  stack copy (`get_object`,
  which repairs `bool`s) — the reference is valid only during the call. A payload whose size ≠ `sizeof(U)` is
  not delivered and makes the call return `length_mismatch` *after* the walk (gk 1.1). With the range check
  (decision D) a scalar or flat record whose `get_object` reports an out-of-range enum leaf is not delivered either, and the
  call returns `invalid_value` after the walk (`deserialize.hpp:992-998`, `1032-1038`) — the value is never
  handed to `f`, as a valueless variant is not (G2-6);
- no match → `f(const record_view&)` if invocable.

`f` is an overload set (typically an overloaded lambda). `Ts` may contain `flat_type`, `dynamic_type` and scalar
types; `touch_arg<Ts>()...` registers them.

**Matching cost.** The per-type work is the `try_one` lambda; which `T` it runs for is chosen by:

```cpp
if constexpr (sizeof...(Ts) <= detail::dispatch_linear_max)
    (try_one(std::type_identity<Ts>{}), ...);
else if (const int i = detail::hash_index<arg_hash_v<Ts>...>::find(r.hash); i >= 0)
    detail::call_for_index<Ts...>(static_cast<std::size_t>(i), try_one);
```

(`deserialize.hpp:1047-1050`.) For up to four types the fold expression compares `r.hash` with each
`arg_hash_v<T>` (constants inlined by the compiler; the first match wins, later ones return at once). Above four,
one lookup in `detail::hash_index<arg_hash_v<Ts>...>` gives the position of the matching type or -1, and
`detail::call_for_index` calls `try_one` for that one type through a table of function pointers: a shift, a mask,
a table load, one key compare and one indirect call per record, however many types `Ts` has. Neither path walks
the registry.

```cpp
static consteval layout find_layout()
{
    const std::size_t m0 = std::bit_ceil(std::max<std::size_t>(2 * n, 2));
    for (std::size_t m = m0; m <= 8 * m0; m *= 2)
        for (unsigned s = 0; s < 64; ++s)
        {
            std::vector<bool> used(m, false);
```

(`deserialize.hpp:869-875`.) `hash_index` searches at compile time for a table size `m = 2^k ≥ 2N` (from the
smallest such power of two up to 8 times it) and a shift `s` in 0..63 for which `(h >> s) & (m - 1)` is distinct
for every distinct key: a perfect hash. The keys are XXH3-64 values, so the first `m` almost always has such a
shift. `slots` maps a slot to position + 1 (0 = empty); `find` loads the slot and compares the stored key with `h`,
so a hash that is not in `Keys...` returns -1. If no `(m, s)` pair exists (a pathological key set), `lay.perfect`
is false and `find` does a binary search (`std::ranges::lower_bound`) over `sorted`, the `(key, position)` pairs
stable-sorted by key. A key listed twice — the same type twice in `Ts` — keeps its first position in both paths,
which is what the linear fold did. The test
"2.1.9 dispatch over more than four types uses the compile-time hash index" (`tests/unit/test_readers.cpp`) checks `lay.perfect`, `find` for present, absent and duplicated keys, and a
twelve-type `dispatch` over a frame with an unmatched record.

The registry's own lookups (`find_type`, `find_call`, the resolver below) stay binary searches over the sorted
published index, and `dyn::dictionary` stays a `std::map` (its output is ordered).

### `deserialize_raw`

`bytes.size() < sizeof(U)` → `truncated`; else `get_object`. No header, no hash — the caller knows the type.
With the range check on (decision D; `ES_SERREG_VALIDATE_READ=1`) an out-of-range leaf of an enum without a fixed
underlying type is `invalid_value` (`deserialize.hpp:838-839`); the output holds the record with that leaf read as 0.

### The resolver: registry first, side dictionary second

```cpp
struct resolver
{
    const dyn::dictionary* dict;
    const call_desc* cd = nullptr;    // compact frames: argument list from the registry
    const dyn::call* dcall = nullptr; //                 or from the dictionary
    std::size_t type_size(const std::uint64_t h, const type_desc*& d, const dyn::type*& dd) const noexcept
    {
        d = registry::instance().find_type(h);
        dd = nullptr;
        if (d) return d->size;
        if (dict) { dd = dict->find_type(h); if (dd) return dd->size; }
        return 0;
    }
    static bool is_dynamic(const type_desc* d, const dyn::type* dd) noexcept { return (d && !d->is_flat) || (dd && !dd->is_flat); }
    bool resolve_call(const std::uint64_t h) noexcept { cd = registry::instance().find_call(h); if (!cd && dict) dcall = dict->find_call(h); return cd || dcall; }
};
```
(`deserialize.hpp:129`, condensed). Every lookup is a lock-free binary search in the published index; the side
dictionary is consulted only for hashes this program does not know. That is how one binary can `inspect` a
foreign capture ("a side dictionary cuts foreign frames") and how `record_view::desc` vs `dyn_desc` tells a
caller *which* description it got. `type_size` returning 0 is the "unknown everywhere" signal.

### `compact_size`: how a compact frame is walked without per-record headers

```cpp
switch (res_.arg_kind(i))
{
case fmt::arg_literal:                                                               // FR-L6
case fmt::arg_const:   return 0;                                                     // 1.5
case fmt::arg_scalar:  return kind_size(res_.arg_scalar_kind(i));
case fmt::arg_string:  { std::uint32_t len; std::memcpy(&len, p, 4); return fmt::string_len_prefix + len; }
default: {
    const type_desc* d; const dyn::type* dd;
    const std::size_t sz = res_.type_size(res_.arg_hash(i), d, dd);
    if (detail::resolver::is_dynamic(d, dd)) { std::uint32_t len; std::memcpy(&len, p, 4); return fmt::image_prefix + len; }
    return sz;
}
}
```
(`deserialize.hpp:280`). A compact frame is self-describing only together with its call ENTRY: the ARG list gives
the category and, for a scalar, the kind; a flat type's size comes from its descriptor; strings and images carry
their own `u32 len`. `inspect` runs this once per record with `end` checks (and, for strings/images, checks that
the prefix itself is present before reading it); afterwards the iterator calls it again *unchecked* on
dereference/advance, which is safe because the frame was validated and is immutable.

### Which error when

| situation | function | error |
|---|---|---|
| fewer than 16 bytes, or `frame_len` > buffer | `read_frame_header` | `truncated` |
| `frame_len` < 16 or not a multiple of 8 | `read_frame_header` | `length_mismatch` |
| unknown frame flag bits | `read_frame_header` | `bad_version` |
| CRC flag set and the trailer does not match (1.7) | `read_frame_header` | `crc_mismatch` |
| compact frame, call hash unknown to registry and dictionary | `inspect` | `unknown_call` |
| compact frame, `n_records` ≠ call arity | `inspect` | `length_mismatch` |
| compact type record whose type is unknown everywhere; 1.5: a constant whose enum is unknown everywhere | `inspect` | `unknown_type` |
| a record (or a string/image prefix) runs past `frame_len` | `inspect`, `get_record` | `truncated` |
| full record: inner `u32 len` disagrees with `payload_len` | `inspect` | `length_mismatch` |
| `deserialize` on a dictionary frame, or wrong call hash / per-record hash; 1.5: a full-records constant with another value | `validate_frame` | `hash_mismatch` |
| `deserialize` with the wrong arity, or a flat record of the wrong size | `validate_frame` | `length_mismatch` |
| corrupt dynamic image (slot outside the image or into the fixed part, `n = 0` with `off ≠ 0`, over the byte budget, depth, bad tag) | `get_record`, `dispatch` | `length_mismatch` |
| valueless variant (`n = 0`) read into a variant without `std::monostate` | `get_record` (soft: later outputs are read), `dispatch` | `invalid_value` |
| a leaf of an enum without a fixed underlying type outside its range (decision D; the leaf reads as 0) | `get_record` (soft), `dispatch` (not delivered), `deserialize_raw` | `invalid_value` |
| `view<T>` on a literal or another type / wrong size / odd address / a `bool` byte not 0 or 1 or an out-of-range enum leaf | `view` | `hash_mismatch` / `length_mismatch` / `misaligned` / `invalid_value` |
| `dispatch`: a matched record whose payload ≠ `sizeof(U)` | `dispatch` (after the walk) | `length_mismatch` |

### Using `dispatch`

```cpp
struct overloaded : Fs... { using Fs::operator()...; };   // user code
auto r = es::serreg::dispatch<corpus::Order, corpus::Cancel, int>(frame, overloaded{
    [&](const corpus::Order& o) { /* zero-copy when aligned */ },
    [&](const corpus::Cancel& c) { },
    [&](int qty) { },
    [&](std::string_view s) { },                  // 1.1 string records
    [&](es::serreg::literal_view l) { },          // l.text empty if the id is unknown here
    [&](const es::serreg::record_view& rv) { }    // anything else: rv.hash, rv.ak, rv.payload
});
```
Each overload is optional (`std::is_invocable_v` checks); a category with no matching overload is silently
skipped. `Ts` order matters only if two types could share a hash, which they cannot unless the canonical strings
collide. The `const T&` for a flat aligned record points into the caller's buffer — do not keep it past the call.

### `frame_view` iteration cost

`begin()` positions `p_` right after the header; `operator*` calls `decode(i_, p_)` (two or three lookups through
the resolver: call args are cached in `res_`, `type_size` does a binary search per type record) and `operator++`
calls `advance(i_, p_)`. The view stores only `data_`, `count_`, `sizes_`, `header` and the resolver — 72 bytes or
so, no heap. This is what lets `inspect` handle a 300-record frame (gk 1.2) without a cap.

## Maintaining it

- **Bounds discipline.** Every `memcpy` here is preceded by a check against `end`/`bytes.size()`, including the
  8-byte dynamic prefix before its `u32 len` is read; keep that invariant when adding a record kind, and run
  `fuzz_capture` (which drives the compiled decoder, not these templates — so add a unit case in
  `tests/unit/test_readers.cpp`/`test_dynamic.cpp` with a crafted frame as well). "2.0 corrupt images are rejected,
  never over-read" is the model.
- **`hash_index` is compile-time only**: the search and the `std::vector<bool>` exist during constant
  evaluation; at run time there is one `static constexpr` table per `Ts...` pack. Keep the duplicate-key rule
  (first position) and the -1 result for an unknown hash; `dispatch_linear_max` is a tuning constant, not a
  semantic one — both paths must deliver the same records.
- **Category dispatch must match `arg.hpp`** and the writer: `compact_size`/`decode`/`get_record`/`dispatch`
  each have one branch per `fmt::arg_kind`; a new category touches all four plus `resolver`. `arg_const`
  is the exception in `dispatch`: `decode` turns it into `arg_scalar` or `arg_type`, so `dispatch` has no branch
  for it.
- **The `decode_error` counters** are set in two places: `validate_frame` (`failed`) and the decode loop
  of `deserialize_frame` (`item`, `done`). A new check in `validate_frame` must come after `failed = ++item` for
  its record, or it reports the wrong item. `expected` counts every element of `outs`, so a new argument category
  that is passed by value (as literals and constants are) still counts as an item.
- **Minor-version handling** lives in `read_frame_header` (unknown flags are `bad_version`). A reader accepts the
  wire minors 6 and 7 (`fmt::format_minor_oldest` … `fmt::format_minor`); files of other minors are refused by the
  FILE/RRI header checks (`tests/unit/test_print.cpp` "FR-H5 captures of an older minor or hash algorithm are
  refused").
- **`view<T>` correctness depends on the writer's scrub** (a `bool` read through `view` must be 0/1 — checked)
  and on 8-aligned frame starts; do not relax the `misaligned` or `bools_canonical` checks —
  `dispatch` falls back to a copy for both cases.
- The `validate_frame` compact path trusts `call_hash`: a 64-bit collision between two different signatures would
  misread a frame. That is the documented risk of the design (FR-H1); full-records mode is the archival
  alternative and validates per record.
- `frame_view` holds a `resolver` with a raw `const dyn::dictionary*`; the dictionary must outlive the view.
  `registry_entry` payload pointers used through `desc` are valid for the life of the process (G2-4: the main
  program's own descriptors, deep copies held by the registry's heap entries for any other image), also across a
  `dlclose`; the unloaded image's entries are retained, so its frames still resolve.
- The static asserts' texts ("non-literal outputs must be non-const lvalues", "a string record is read into
  std::string or std::string_view") are the user-facing diagnostics; keep them.
- Tests: `tests/unit/test_readers.cpp` (FR-S4 `inspect` counts/types/literal flags, `view` aligned vs misaligned,
  `dispatch` routing and gk 1.1 short records, gk 1.2 41- and 300-record frames,
  "2.1.9 dispatch over more than four types uses the compile-time hash index"), `tests/unit/test_roundtrip.cpp`
  (FR-S1, FR-L1, FR-W2 per-record validation, FR-S4 wrong signature fails before touching outputs, FR-D5
  truncated, FR-S5 raw, FR-S8 `as_flat` through `dispatch`; the `decode_error` cases and "FR-A5 constants:
  no bytes in a compact frame, the value's record in a full one (1.5)", which covers `deserialize`, `inspect` and
  `dispatch` over constants; "2.1.16 decision D: an out-of-range value of an enum without a fixed
  underlying type reads as 0 and is reported as invalid_value" in `tests/unit/test_limits.cpp`, which covers
  `deserialize`, `deserialize_raw`, `dispatch` and `view`), `tests/unit/test_print.cpp` (FR-A4 string/scalar
  records in `inspect`/`dispatch`, FR-P1 `deserialize_print`/`println`), `tests/unit/test_dynamic.cpp` ("2.0
  inspect / dispatch / view over dynamic records", corrupt images, typed round trips),
  `tests/unit/test_capture.cpp` (dictionary frames through `inspect`).
