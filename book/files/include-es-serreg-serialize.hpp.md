# `include/es/serreg/serialize.hpp`

`serialize()`: one frame per call (spec §6.4, §6.5, FR-S1..S6, FR-W2). It computes the frame size, writes the
16-byte header and one record per argument, and hands the bytes to a sink — in place for a `claiming_sink`,
through a stack buffer or a streaming loop for a plain sink. Registration is a side effect of instantiation
(`registry.hpp`), so the hot path does no allocation, no lock and no registry access. This is the
**serialisation layer**; it is reflection-dependent through `arg.hpp`/`registry.hpp` (`descriptor_v`, the
image encoder in `reflect.hpp`).

## Depends on / used by

Includes: `<cstddef>`, `<cstdint>`, `<cstring>`, `<span>`, `<type_traits>`, `<vector>` (the heap fallback),
`es/serreg/arg.hpp`, `es/serreg/error.hpp`, `es/serreg/format.hpp`, `es/serreg/registry.hpp`, `es/serreg/sink.hpp`.

Included by: `deserialize.hpp` (`frame_size`, `record_payload_size`, `serialize_options`, `sink_noexcept` for
symmetry), `print.hpp` (`serialize_frame`, `print_tag`), `capture.hpp` (`sink_failure`),
`tests/support/test_support.hpp`; everything that writes frames.

## What it defines

Public:

| name | kind | meaning |
|---|---|---|
| `serialize_options` | struct | `scrub_padding = true` (FR-S6: zero gaps, normalise bools), `full_records = false` (FR-W2: per-record hash + length), `crc = false` (format 1.7: an 8-byte CRC32C trailer, frame flag bit 2), `canonical_unordered = true` (unordered containers sorted by key on the wire; `false` writes them in iteration order — same value, possibly different bytes, no sort, every reader accepts both) (`serialize.hpp:41-54`) |
| `stack_frame_limit` | `constexpr size_t` 64 KiB | largest frame assembled on the stack for a plain sink |
| `stream_chunk_size` | `constexpr size_t` 16 KiB | chunk for streaming larger records |
| `frame_size<Ts...>(o = {})` | `constexpr` function template | header + Σ `record_size<T>`; exact without strings/dynamic types, the minimum otherwise |
| `frame_size(o, const Ts&... vs)` | function template | exact always (one measure pass) |
| `has_string_arg_v<Ts...>` | variable template | any runtime string in the pack |
| `has_runtime_size_v<Ts...>` | variable template | any string or dynamic type (size known only at run time) |
| `serialize_with(o, S& out, const Ts&...)` | function template | one frame with options |
| `serialize(S& out, const Ts&...)` | function template | `serialize_with({}, …)` |
| `serialize<L>(S& out, const Ts&...)` / `serialize_with<L>(o, …)` | function templates | `fixed_string L` first: `serialize<"evt">(out, o)` ≡ `serialize(out, lit<"evt">{}, o)` |
| `serialize(S&, const Ts&...) = delete("…")`, `serialize_with(o, S&, const Ts&...) = delete("…")` | deleted overloads, `requires(!(serializable_arg<Ts> && ...))` (B10) | an argument that is no category — a pointer, a reference, a union, a bit-field, a virtual class — gets a message ("an argument is not a serialisable category (a class or enum, a literal, a constant, a scalar, a runtime string, or — since 2.2.0 — a container). Pointers, references, unions, bit-fields and virtual classes are never serialised; map them with as_flat<T>") instead of an overload-resolution failure (`serialize.hpp:504-517`). A container is a type argument (format 1.6) — its transparent type — and `serializable_arg` admits it through `dynamic_type` |
| `serialize_raw(S& out, const T& v, scrub = true)` | function template (`flat_type T`) | payload only — no header, no id (FR-S5) |

`namespace detail`:

| name | meaning |
|---|---|
| `record_payload_size<T>()` | 8 (literal, full mode) / 4 (string minimum) / `sizeof` (scalar) / 8 (dynamic minimum) / `sizeof(mapped_t<T>)`; for a constant (1.5), the payload size of its value type |
| `record_size<T>(full)` | 0 for a compact literal and for a compact constant (1.5); a full-records constant has the size of its value's record; else `(full ? 16 : 0) + align8(payload)` |
| `record_payload_bytes(v)` / `record_bytes(v, full)` | runtime-exact versions (string length, `measure_image`) |
| `sink_with_state<S>`, `sink_failure(s)` | the optional `status state()` protocol |
| `claim_noexcept<S>`, `sink_noexcept<S>` | whether `serialize` into `S` is `noexcept` |
| `scrub_range(dst, desc, off, len)` | zero `padding_gaps` and normalise `bool_offsets` within a byte range |
| `put_object(dst, v, scrub)` | memcpy one flat object (through `as_flat::to` when mapped) + scrub |
| `put_record_header<T>(p, o, payload_len)` | full-records header: `arg_hash<T>()`, `u32 len`, `u16 0, u16 0` |
| `put_value_record(p, v, o)` | one record: header (full) + payload + pad to 8 |
| `put_record(p, v, o)` | one argument — a constant writes nothing (compact) or the record of its value (full); everything else goes to `put_value_record` |
| `put_frame_header(p, len, flags, n, call_hash)` | the 16 bytes |
| `stream_bytes(out, src, total, desc, o, chunk, pad)` | copy through the chunk buffer, scrubbing per chunk |
| `stream_record(out, v, o, pad = true)` | one record through the chunk buffer (plain sinks, large frames) |
| `sink_wants_crc(out)` | `out.frame_crc()` when the sink has it (a `{.crc = true}` capture file), else false |
| `put_crc_trailer(frame, total)` | `crc32c` of the frame's first `total - 8` bytes, then four zero bytes |
| `serialize_frame<Sig...>(o, out, vs...)` | the engine: `Sig` is the signature pack (optional form tag + categories), `vs` the records |

## How it works

### Record layout per category

`put_value_record` (`serialize.hpp:188-225`) is the single writer of record bytes; `put_record`
(`serialize.hpp:227-242`) is the entry point per argument and adds the one case that is not a value:

| category | compact payload | full-records payload | pad |
|---|---|---|---|
| literal | **nothing** — returns before writing (FR-L6) | `u64 id` | to 8 |
| constant (1.5) | **nothing**: the call's entry holds the value | the scalar or enum record of its value | to 8 |
| string | `u32 len, bytes` (`string_bytes(v)`) | same | to 8 |
| scalar | `sizeof(T)` bytes (`memcpy(&v)`) | same | to 8 |
| flat type | `sizeof(U)` bytes via `put_object` (`as_flat::to` if mapped, then `scrub_range`) | same | to 8 |
| dynamic type | `u32 len, u32 0, encode_image(v, …, o.canonical_unordered)` | same | to 8 |

In full-records mode `put_record_header<T>` precedes the payload with `arg_hash<T>()` (schema hash / literal id /
scalar-name hash / `"string"` hash), the payload length and a zero `layout`. The payload of a string or image is
byte-identical in both modes (`payload_len` = 4 + len, resp. 8 + len).

### Constants (format 1.5)

```cpp
template<class T>
inline void put_record(std::byte*& p, const T& v, const serialize_options& o) noexcept
{
    if constexpr (is_constant_v<T>)
    { // nothing in a compact frame; in a full-records frame the record of the value it stands for
        if (o.full_records)
        {
            const arg_value_t<T> value = T::value;
            put_record(p, value, o);
        }
    }
    else
        put_value_record(p, v, o);
}
```
(`serialize.hpp:239`). A `constant<V>` (`42_`, `k<Side::buy>`; `literal.hpp`) is part of the call signature, like a
literal. In a compact frame it writes nothing: `record_size<constant<V>>(false)` is 0, so
`frame_size<lit<"fill">, constant<42>, Order>()` equals `frame_size<Order>()` and a frame of a literal and
constants only is its 16-byte header. `n_records` still counts the constant. In a full-records frame the constant
is written as a run-time value of its type would be — the full-record header with the scalar name's hash or the
enum's schema hash, then the bytes — so the frame stays self-describing. A full frame written with `42_` and one
written with a run-time `42` differ only in the call hash of the frame header. `stream_record` (plain sinks, large
frames) takes the same route for a constant as for a literal (`serialize.hpp:285-290`).

The cost on the hot path is zero: the branch is `if constexpr`, and for a compact frame it compiles to nothing.
`registry.hpp::touch_arg<constant<V>>` registers the enum of an enum constant, so the reader's dictionary can show
its enumerator name. Test: "FR-A5 constants: no bytes in a compact frame, the value's record in a full one (1.5)"
(`tests/unit/test_roundtrip.cpp`), which also checks that a plain sink streams the same bytes.

### Frame size: constexpr when possible, measured otherwise

```cpp
template<class... Ts> constexpr std::size_t frame_size(const serialize_options o = {}) noexcept
{
    return fmt::frame_header_size + (o.crc ? fmt::crc_trailer_size : 0) +
           (std::size_t{0} + ... + detail::record_size<std::remove_cvref_t<Ts>>(o.full_records));
}
```
(`serialize.hpp:360-364`; the runtime form `frame_size(o, vs...)` follows at `serialize.hpp:365-370`). Both
count the 8-byte CRC trailer when `o.crc` is set. `serialize_frame` picks the runtime form only when `has_runtime_size_v<Ts...>`, i.e. a
string (one `strlen`/`size()`) or a dynamic type (one `measure_image` pass, no allocation) is present. Two
`static_assert`s guard the `u16 n_records` and `u32 frame_len` limits at compile time; a > 4 GiB string or image
is caught at run time with `error::invalid_argument`.

### The claiming path

```cpp
auto fill = [&] [[gnu::always_inline]] (std::byte * p) noexcept {
    std::byte* const frame = p;
    detail::put_frame_header(p, static_cast<std::uint32_t>(total), flags,
                             static_cast<std::uint16_t>(sizeof...(Ts)), call_hash_v<Sig...>);
    p += fmt::frame_header_size;
    (detail::put_record(p, vs, o), ...);
    if (o.crc)
        detail::put_crc_trailer(frame, total);
};
// ...

if constexpr (claiming_sink<S>)
{
    std::byte* const p = out.claim(total);
    if (!p)
        return std::unexpected(detail::sink_failure(out));
    fill(p);
    if (!detail::commit_claimed(out, total))
        return std::unexpected(detail::sink_failure(out));
}
```
(`serialize.hpp:426-447`.) Measure → claim → encode in place → (checksum) → commit. For flat arguments this is the hot path the
header comment describes: `memcpy` per object, gap scrub from the compile-time tables, done. For a dynamic type the
image is encoded straight into the claimed slot (`encode_image` needs random access into the image to patch tail
offsets, which a claimed slot provides) — so the encoder never allocates for a claiming sink; whatever the sink's
own `claim` does (a ring: nothing; `capture_file`: grows its buffer once to the largest frame seen) is the only
allocation in the picture.

Two details of this block are review fixes:

- **`fill` is `[[gnu::always_inline]]` (C-08).** Without it, at `-O2` (CMake's RelWithDebInfo) GCC keeps the lambda
  out of line and calls it through the closure, so the hot path makes a call. The attribute sits between the capture list and
  the parameter list, where it appertains to the call operator; written after the parameter list it appertains to
  the lambda's type and GCC ignores it. The ctest `serialize_inlines_at_O2` compiles `tests/bench/hot_inline.cpp`
  (`serialize(span_sink&, "new order"_lit, corpus::Order)`) to assembly at `-O2` and
  `tests/bench/check_inline.cmake` fails when the function `hot()` contains a `call` or a tail `jmp` to a symbol.
- **The commit can fail (C-09).** The frame is committed through `detail::commit_claimed(out, total)` (`sink.hpp`):
  for a sink whose `commit(n)` returns `bool`, `false` means the frame was not written, and `serialize` returns
  `sink_failure(out)` (the sink's `state().error()` or `sink_full`) instead of the byte count. A `void` `commit`
  cannot fail, as before. `serialize_raw`'s claiming path does the same. `capture_file::commit` is the first sink to
  return `bool` (`capture.hpp`).

### The plain-sink paths

### The CRC32C trailer (format 1.7)

```cpp
if (detail::sink_wants_crc(out))
    o.crc = true;
const std::size_t total = dynamic ? frame_size(o, vs...) : frame_size<Ts...>(o);
```
(`serialize.hpp:425-427`.) `o.crc` comes from `serialize_options{.crc = true}` or from the sink: a sink with a
`frame_crc()` member returning true (`capture_file` opened with `capture_options{.crc = true}`) gets a trailer on
every frame whatever the caller passed. The frame then carries `frame_flag_crc`, is 8 bytes longer, and `fill`
ends with `put_crc_trailer` (`serialize.hpp:392-397`): `u32 crc32c(frame[0, total-8))`, `u32 0`. The CRC is
`crc32c()` of `hash.hpp` (SSE4.2 when the CPU has it). With the option off there are zero extra bytes and no extra
work: the branch is on a value known in the caller. The stack buffers are sized for the largest form
(`serialize_options{.full_records = true, .crc = true}`), and the two `static_assert`s check that form too.

Four plain-sink cases, in order (`serialize.hpp:448-481`):

1. **Static size ≤ 64 KiB** (`!dynamic && frame_size<Ts...>(full) <= stack_frame_limit`): an *exactly sized*
   stack buffer `std::byte buf[frame_size<Ts...>(full)]` (cc S2), filled, one `write`.
2. **Runtime size ≤ 4 KiB** (`dynamic && total <= 4096`): a fixed 4 KiB stack buffer for the common short-message
   case (strings, small images).
3. **A CRC frame larger than that**: the trailer covers the whole frame, so it cannot be streamed; the
   frame is assembled in a `std::vector<std::byte>` and written once — a plain sink's second possible allocation.
4. **Otherwise: streaming.** Header written first, then `stream_record` per argument through a 16 KiB
   `alignas(16)` chunk on the stack:
   - literal: nothing (compact) or header + id; a constant (1.5): nothing (compact) or its value's record;
   - scalar: header + bytes, padded;
   - string: header (full) + `u32 len` from the chunk, then the bytes **straight from the caller's storage**,
     then the pad — no copy of the string;
   - flat type: header (full), then `stream_bytes` copies `sizeof(U)` bytes in 16 KiB pieces, applying
     `scrub_range(chunk, desc, off, len)` per piece (gap intervals are clipped to the window) — so an 8 MiB
     struct serialises under a 1 MiB stack (`tests/unit/test_sinks.cpp` gk 1.3) with no heap. An `as_flat` type
     needs one converted copy `const U u` on the stack (its size is the mapped type's);
   - dynamic type: the image must be assembled whole (tail offsets are patched into the fixed part), so it goes
     into the chunk when `record_bytes(v, full) ≤ 16 KiB`, otherwise into a `std::vector<std::byte>` — **the one
     place a plain sink can allocate** (`serialize.hpp:312`; a CRC frame above 4 KiB of runtime size or 64 KiB of static size is the other, `serialize.hpp:465`; claiming sinks never do).

Writing stops at the first `false` from the sink; the error is `sink_failure(out)` — the sink's `state().error()`
when it has one, else `sink_full`.

### Scrubbing (FR-S6)

```cpp
inline void scrub_range(std::byte* const dst, const type_desc& d, const std::size_t off, const std::size_t len) noexcept
{
    for (const gap g : d.padding_gaps) { /* zero the intersection of [g.offset, g.offset+g.len) with [off, off+len) */ }
    for (const std::uint32_t b : d.bool_offsets)
        if (b >= off && b < off + len)
            dst[b - off] = dst[b - off] == std::byte{0} ? std::byte{0} : std::byte{1};
}
```
(`serialize.hpp:154`). With `scrub_padding` (the default, D3) identical values give identical bytes: padding
inside nested structs and arrays (the gap table is flattened at compile time by `reflect.hpp::compute_gaps`),
skipped members (they count as gaps) and any non-0/1 byte in a `bool` (gk 1.7) are normalised. With it off, the
native bytes go out as they are. Dynamic images have no scrub table: `encode_image` zero-fills and
`put_flat_image` scrubs the flat leaves itself.

### Registration happens here

```cpp
(detail::touch_arg<Sig>(), ...); // FR-R1: registration by instantiation
static_cast<void>(&detail::registrar_call<Sig...>::g);
if constexpr (scoped_sink<S>) // 2.2.1: the (scope, signature) node of a scoped sink
    static_cast<void>(&detail::registrar_scoped<typename S::dictionary_scope, Sig...>::g);
```
(`serialize.hpp:412-415`). These lines are why every type reachable from a serialised argument and every call
signature are in the registry before `main()`. They compile to nothing. A sink that declares a
`dictionary_scope` (`scoped_sink`, `sink.hpp`; `scoped_capture_file<Tag>`) also instantiates the scope node of
this (scope, signature) pair, which is how `registry::export_image_scope` knows which calls a scoped file's
dictionary must hold — at compile time, with no per-frame cost.

### `serialize_raw`

Payload only (FR-S5): `touch<T>()` for registration, then `put_object` into a claimed slot / an `alignas(U)` stack
temporary (≤ 64 KiB) / `stream_record(..., pad_to_8 = false)`. The caller owns framing (an SBE envelope, say);
`deserialize_raw` is the inverse. Flat types only.

### Worked example: `serialize(out, "evt"_, order)` in compact mode

`Sig = lit<"evt">, corpus::Order`; `corpus::Order` is 32 bytes with one 3-byte gap at offset 29
(`tests/golden/hashes.cpp`). `frame_size<lit<"evt">, Order>()` = 16 + 0 + 32 = 48 (the golden test says
`16 + 0 + 32`; in 1.0 the literal cost 8 more).

| offset | bytes | from |
|---|---|---|
| 0 | `u32 48` | `put_frame_header`: `frame_len` |
| 4 | `u16 0` | flags (compact) |
| 6 | `u16 2` | `n_records` — the literal counts even though it has no bytes |
| 8 | `u64 call_hash_v<lit<"evt">, Order>` | `fingerprint64("call(2){lit:0x…,0x367f8cd8c6441dd2}")` |
| 16 | — | `put_record(p, lit<"evt">{}, o)` → `put_value_record` returns immediately (FR-L6) |
| 16 | 32 bytes of `Order` | `put_object`: `memcpy`, then `scrub_range` zeroes bytes 45..47 (gap `29+3`) and normalises no bools |

With `full_records = true` the same call gives 16 + (16 + 8) + (16 + 32) = 88 bytes: each record is preceded by
`u64 arg_hash, u32 payload_len, u16 0, u16 0`, and the literal's `u64 id` reappears as its payload. The frozen
size is in "FR-S3 frame sizes".

### Worked example: the dynamic record of plan §2

`serialize(out, corpus::DynMsg{7, "ab", {1, -2, 3}})` — `frame_size(o, v)` = 16 + `align8(8 + measure_image(v))`
= 16 + `align8(8 + 28)` = 56 (the unit test asserts `16 + 8 + 32`). `put_record` writes `u32 28, u32 0`, then
`encode_image` produces the 28-byte image (fixed part 20 bytes: `id@0`, `name` slot `@4 = {20, 2}`, `xs` slot
`@12 = {22, 3}`; tail: `"ab"` at 20, three `i16` at 22), then `memset` pads 4 bytes to 8. Identical values give
identical bytes (`frame_of(DynMsg{7,"ab",{1,-2,3}}) == buf`).

### The decision tree, in one table

| sink kind | size known at compile time? | frame ≤ limit? | path | allocation |
|---|---|---|---|---|
| claiming | either | any | `claim(total)` → fill → `commit_claimed` (fails when a `bool` `commit` refuses) | none (the sink's) |
| plain | yes | ≤ 64 KiB (full-records size) | exact stack buffer, one `write` | none |
| plain | yes | > 64 KiB | header, then `stream_record` per argument (16 KiB chunk) | none |
| plain | no (string / dynamic) | `total ≤ 4096` | 4 KiB stack buffer, one `write` | none |
| plain | no | > 4096 | header, then `stream_record`: strings from caller storage, images in the chunk or, above 16 KiB, in a `std::vector` | only for an image > 16 KiB |

### `sink_noexcept` without naming `claim` on plain sinks

```cpp
template<class S> constexpr bool claim_noexcept = true; // plain sinks: nothing more to check
template<claiming_sink S>
constexpr bool claim_noexcept<S> = noexcept(std::declval<S&>().claim(std::size_t{0})) &&
                                   noexcept(std::declval<S&>().commit(std::size_t{0}));
template<class S>
constexpr bool sink_noexcept =
    noexcept(std::declval<S&>().write(static_cast<const std::byte*>(nullptr), std::size_t{0})) && claim_noexcept<S>;
```
(`serialize.hpp:124`). The constrained partial specialisation is chosen only for claiming sinks; a plain sink never
has `claim` substituted into an expression. `tests/support/test_support.hpp` documents the bug this avoids
(a `||` that does not short-circuit *substitution*) and keeps a static check that the library derives the value the
tests expect. Results: `span_sink` → true, `fd_sink` → true, `vector_sink` → false, `capture_file` → false.

`serialize_noexcept<S, Ts...>` (G2-7) is what the `serialize*` functions and `stream_record` actually use:
`sink_noexcept<S>` and, for a plain (non-claiming) sink, no dynamic argument — a dynamic record above
`stream_chunk_size` is assembled in a heap buffer, so `serialize(fd_sink&, DynMsg)` is not `noexcept`, while
`serialize(span_sink&, DynMsg)` (built in claimed memory) is. Since review B1 it is also false for any argument
with `detail::may_sort_v` (`any_may_sort_v<Ts...>`, a loop rather than a fold because `-Wlogical-op` rejects a
repeated type in the pack), on claiming sinks too: the canonical sort's order table above 64 entries is the one
remaining allocation; `put_record`, `put_value_record` and `serialize_frame`'s `fill` carry the same conditional
`noexcept`, so a `std::bad_alloc` propagates instead of terminating. The file header's "hot-path rules" paragraph
lists what allocates where.

### Hot-path checklist (what the flat claiming path compiles to)

For `serialize(ring, "evt"_, order)` into a ring-buffer claiming sink at `-O2`:

1. `touch_arg<…>()` and the registrar odr-uses — no code.
2. `frame_size<Ts...>()` — a constant.
3. `claim(48)`, null check.
4. Four stores for the header (the call hash is a constant).
5. One 32-byte copy and one 3-byte `memset` for the gap (both inlined from constant tables).
6. `commit(48)`.

No branch depends on the registry, no function from `libserreg` is called, nothing is `new`ed, no mutex is
taken, and no call remains at all (`fill` is `always_inline`; ctest `serialize_inlines_at_O2` checks
it). `tests/bench/bench.cpp` measures this; keep any addition to `put_record`/`put_value_record`/`put_object` behind
`if constexpr` on the category so the flat path stays this shape.

## Maintaining it

- **Byte-level behaviour is golden.** `frame_size` values are frozen in `tests/golden/hashes.cpp` "FR-S3 frame
  sizes" (`16 + 0 + 32` for `("evt"_, Order)`, `16` for a bare literal, `16 + 8` for a dynamic type's minimum);
  the frames in `tests/fixtures/golden/*.ser` are byte-exact; `tests/unit/test_dynamic.cpp` checks the plan §2
  image byte for byte. Any change to `put_record`/`put_value_record`, padding, the prefix or the full-record header is a
  `format_minor` bump with fixtures and CHANGELOG.
- **Keep the flat hot path free of the dynamic code.** Everything dynamic is behind `if constexpr (is_dynamic<T>())`;
  `frame_size<Ts...>()` must stay `constexpr` for flat/scalar/literal packs (a `static_assert` in
  `tests/unit/test_print.cpp` "FR-A3 frame_size: constexpr without strings…" and the golden `STATIC_REQUIRE`s
  would catch a regression).
- **`noexcept` is derived, not declared:** `sink_noexcept<S>` is `noexcept(write) && claim_noexcept<S>`, where
  `claim_noexcept` is a partial specialisation for `claiming_sink` (so `claim`/`commit` are never named on a plain
  sink — the rule described in `tests/support/test_support.hpp`). `tests/unit/test_sinks.cpp` "cc F6" pins
  the table. The dynamic heap fallback is inside a `noexcept(sink_noexcept<S>)` function; a `std::bad_alloc` there
  terminates — acceptable per the header comment, but do not widen that path.
- **Stack budget:** 64 KiB exact buffer (case 1) *or* 4 KiB (case 2) *or* 16 KiB chunk (case 3) — never two of
  them in one activation; keep it that way if you add a case, and remember `serialize` can be called on small
  thread stacks (the gk 1.3 tests run under a 1 MiB stack).
- `stream_bytes` scrubs per window using absolute offsets (`off`); a gap straddling two chunks is handled by
  clipping, do not "optimise" it to a per-object scrub.
- `serialize_options` is passed by value everywhere; adding a field is ABI-neutral for header-only code but
  changes `frame_size(o, …)` overload resolution if you give it a non-`bool` member — keep it an aggregate of
  bools. `canonical_unordered` is the one option that reaches into the image encoder (`put_value_record` passes it
  to `encode_image`, which stores it in the `image_cursor`); it changes bytes, never sizes or hashes, so
  `frame_size` and `measure_image` ignore it and the readers never see it. Test: "FR-A6
  serialize_options::canonical_unordered: sorted by default, iteration order on request, readers take both"
  (`tests/unit/test_containers.cpp`).
- Tests: `tests/unit/test_roundtrip.cpp` (FR-S1 every flat shape, FR-L1 literals, FR-S3 `frame_size` equals bytes
  written, FR-S6 determinism, FR-W2 full records, FR-S2 `span_sink` exhaustion, FR-S5 raw, FR-S8 `as_flat`, FR-S10
  skipped members zero on the wire; "FR-A5 constants: no bytes in a compact frame, the value's record in a
  full one (1.5)"), `tests/unit/test_sinks.cpp` (gk 1.3 streaming under a small stack, gk 1.7 bool
  scrub, cc F6 noexcept, failing sink stops), `tests/unit/test_print.cpp` (FR-A1/A2/A3, FR-L6, strings past the 4 KiB
  buffer), `tests/unit/test_dynamic.cpp` (byte-exact image, every shape, large vectors on plain vs claiming sinks),
  `tests/unit/test_sinks.cpp` "2.1.12 C-09 a claiming sink whose commit() returns false makes serialize() fail",
  `tests/negative/neg_pointer_argument.cpp` (B10: the deleted overload's message, expected text "never
  serialised"),
  `tests/unit/test_containers.cpp` (a `std::vector<int>` argument is a 48-byte frame, an empty one 32,
  full records, a map, an optional, a variant and a set in one call),
  `tests/bench/bench.cpp`, and the ctest `serialize_inlines_at_O2` (`tests/bench/hot_inline.cpp`,
  `tests/bench/check_inline.cmake`).
- **The deleted overloads must stay less constrained than the real ones**: they match only when some
  `Ts` is not a `serializable_arg`, so a valid call never sees them; a new argument category widens
  `serializable_arg`, not the deleted overloads' message.
- **Keep the attribute where it is.** Moving `[[gnu::always_inline]]` after `fill`'s parameter list, or replacing
  the lambda with a function the optimiser may decline to inline, brings the call back; `serialize_inlines_at_O2`
  catches it.
