# 4. Reading back — the five styles

All readers take a `std::span<const std::byte>` positioned at a **frame** (16-byte header first). A capture
file's frames start at `align8(dict_offset + dict_len)`; a stream's frames follow one another; every frame is
skippable by its `frame_len` even when nothing else about it is understood.

## 4.1 Typed: `deserialize`

```cpp
ex::Order o; std::string s; int n;
decode_result<std::size_t> r = deserialize(frame, "new order"_, o);  // same categories, same order as the writer
auto r2 = deserialize(frame, s, n, "evt"_);                          // string → std::string or std::string_view
auto r3 = deserialize_println(frame, "px={:.2f}"_, px);
auto r4 = deserialize(frame, "fill"_, 42_, o, k<Side::sell>);        // constants are named, like literals
std::vector<std::int32_t> ids; std::unordered_map<std::string, double> px;
auto r5 = deserialize(frame, "ids"_, ids);                           // a container argument, into any container of the shape
auto r6 = deserialize(frame, px);                                    //        (the writer may have used a std::map)
```

The result is the number of bytes consumed. The three typed readers return
`decode_result<std::size_t>` = `std::expected<std::size_t, decode_error>` (§4.1a).

Validates first, writes outputs second: header, then (compact) the call hash against `call_hash_v<Ts...>` and
the minimum frame size, or (full) every record's hash and length; then records are read with bounds checks
(strings and images check their prefixes against the remaining bytes). Outputs are untouched when the frame or
a record does not fit; §4.1a says what a failure reports and names the one case that is found later.
Errors: `hash_mismatch` (different signature, different literal, print vs plain), `length_mismatch`,
`truncated`. A dynamic type is decoded member by member (`std::vector` resized, `std::optional`
engaged/reset, `std::variant` emplaced; a valueless variant on the wire becomes `std::monostate` when the variant
has one, otherwise `error::invalid_value` — the other outputs are still read; a map or set is `clear`ed and
refilled with `emplace_hint(end(), …)`, a duplicate key on the wire being `error::invalid_value` with the first
value kept, §4.1c). Both decoders apply
the same slot rules and a byte budget (`fmt::image_budget`, chapter 8), and a `bool` byte other than 0/1 reads
as `true`; install `set_warning_handler` to hear about it.
`deserialize_raw(bytes, T&)` is the payload-only counterpart of `serialize_raw`.

**The range check for enums without a fixed underlying type (decision D; FR-D5) — a compile-time policy, off by
default.** An `enum Color { red, green }` — no `enum class`, no `: T` — holds only the values of the minimal-width
integer that represents its enumerators ([dcl.enum]): `red`/`green` fit two bits, so 0..3 are values of `Color`
and 7 is not, and reading an object that holds 7 is undefined behaviour. Checking a leaf against that range is
value validation (`docs/values-validation.md`), so it is a policy of the translation unit, set like the logger's
level: `-DES_SERREG_VALIDATE_READ=1` turns it on, the default `0` leaves it off (`detail::validate_read_enums`,
`include/es/serreg/reflect.hpp`). When it is on, a typed reader checks such leaves (members, bases, array
elements, top-level arguments) when it copies a flat value; a leaf outside its range **reads as 0** and the read
is reported as `invalid_value`. `deserialize` treats it like a valueless variant — the other arguments are still
read and `decode_error::failed` names the argument; `dispatch` does not deliver the value; `view` refuses it;
`deserialize_raw` and `walk_values` report it. Values inside the range that no enumerator names (flag-style
enums, `3` above) stay valid. Enums with a fixed underlying type (`enum class`, `enum E : std::uint8_t`) are never
checked: every value of `T` is a value of the enum. When the policy is off, the range tables (`enum_ranges_v<T>`)
are not instantiated and the bytes are copied as they are. Dictionary-driven readers (`srdump`, `verify`,
`visit_record`) print the number: since format 1.6 the enum ENTRY carries a `fixed` flag (`dyn::type::enum_fixed`,
shown as `unfixed` / `"fixed": false` in the dumps) that a tool could use, but none does. Tests: "2.1.16 decision
D: an out-of-range value of an enum without a fixed underlying type reads as 0 and is reported as invalid_value"
(`tests/unit/test_limits.cpp`, built with the policy on) and "FR-D5 ES_SERREG_VALIDATE_READ=0: out-of-range enum
leaves read through unchanged" (`tests/unit/test_validate_off.cpp`, the default).

## 4.1a What a failure reports: `decode_error`

```cpp
struct decode_error { error code; std::uint16_t expected, actual, failed; /* converts to error */ };

if (const auto r = deserialize(frame, "evt"_, order, text, n); !r)
    std::println("{}: decoded {} of {} items, item {} failed",
                 to_string(r.error()), r.error().actual, r.error().expected, r.error().failed);
```

A typed reader decodes in one pass and says how far it got. Items are counted from the frame itself: item 0 is
the frame (header, call hash, number of records), item *i* is argument *i*, counted from 1. Literals and constants
are arguments; for `deserialize_print` and `deserialize_println` the format literal is argument 1.

- `expected` — 1 + the number of arguments of the call;
- `actual` — 0 when the frame itself was refused, otherwise 1 + the number of arguments decoded into their outputs;
- `failed` — the first item that failed (0 = the frame).

The four cases:

| what happened | the error | the outputs |
|---|---|---|
| The frame is refused: too short, a bad header, another call hash, another number of records, a dictionary frame | `{code, expected, 0, 0}` | nothing written |
| The frame is accepted and record *i* does not fit: its length or hash is wrong, or it runs past the frame | `{code, expected, 1, i}` | nothing written — every record is checked before the first output is touched |
| Record *i* fails while it is decoded: a dynamic image that is well-formed on the outside and corrupt inside | `{code, expected, i, i}` | arguments before *i* are complete, argument *i* is valid but unspecified, later ones are untouched |
| `invalid_value` (a valueless variant the output cannot hold; an out-of-range leaf of an enum without a fixed underlying type, which reads as 0, when the range check is on; a duplicate key in a map or set) | `{invalid_value, expected, expected − the number of such arguments, the first of them}` | decoding does not stop: a variant output keeps its old value, an enum leaf is 0, a duplicate entry is dropped, all the others are read |

For `deserialize(frame, "evt"_, order, text, n)` on a compact frame whose string claims more bytes than the frame
has: `{truncated, 5, 1, 3}` — five items, the frame was accepted, argument 3 (the string) did not fit, and
`order`, `text` and `n` are untouched.

`decode_error` converts implicitly to `error` and compares with one, so code written against a plain `error`
keeps working:
`r.error() == error::truncated`, `to_string(r.error())`, and `return std::unexpected(r.error());` from a function
that returns `result<T>`. Only code that spelled the return type as `result<std::size_t>` or
`std::expected<std::size_t, error>` must change it to `decode_result<std::size_t>` or `auto`. `deserialize_raw`,
`dispatch`, `inspect` and `frame_router` are unchanged and report a plain `error`. Details:
`files/include-es-serreg-error.hpp.md`, `files/include-es-serreg-deserialize.hpp.md`.

## 4.1b Constants on the reading side (format 1.5)

A frame written with compile-time constants (chapter 3 §3.3b) is read in two ways.

**A typed reader names the same constant.** `deserialize(frame, "fill"_, 42_, order, k<Side::sell>)` reads the
frame `serialize(out, "fill"_, 42_, order, k<Side::sell>)` wrote. The constants are passed by value, like literals,
and nothing is stored for them. Another value is another call: `43_` in place of `42_` gives `hash_mismatch`. On
a compact frame that is the frame's call hash, `{hash_mismatch, expected, 0, 0}`. On a full-records frame each
record is checked on its own, so the error names the argument, `{hash_mismatch, expected, 1, i}`. A run-time `int&`
in place of `42_` is also another call on a compact frame; a full-records frame carries the constant as an ordinary
record, so there it also reads into run-time outputs (`deserialize(full, "fill"_, n, order, side)`).

**Every other reader sees the record the constant stands for.** `inspect`, `dispatch`, `tools::walk_values`,
`tools::verify`, `srdump`, print rendering and the Wireshark dissector present a constant as a scalar or enum
record with the value from the call's dictionary entry:

```cpp
for (const record_view r : inspect(frame)->records())
    if (r.is_const) …    // r.ak is arg_scalar (with r.k) or arg_type (an enum); r.payload holds the value
```

`record_view::is_const` is the only difference from a run-time record. In a compact frame `r.payload` points at the
value stored in the registry's or the side dictionary's call entry, not into the frame, so it stays valid as long
as that registry or dictionary does. `dispatch<int, Side>(frame, f)` delivers a constant `42_` to the `int`
overload and `k<Side::sell>` to the `Side` overload. A `frame_router` handler does not receive constants, as it
does not receive literals; its signature is given with `on<Sig...>` (`files/include-es-serreg-router.hpp.md`).

## 4.1c Containers on the reading side (format 1.6)

**As members.** A `map` or `set` member decodes into *any* container of its shape: the writer's `std::map` reads
into an `std::unordered_map` or `std::flat_map` of the same key and value types and vice versa, because the
member's hashes name the shape, not the container (chapter 8 §8.1b). The decoder `clear`s the output, `reserve`s
`n` when the container has `reserve` (one rehash instead of log n), decodes every entry into temporaries — the
key at the entry's start, the value at its canonical offset — and inserts with `emplace_hint(end(), …)`, which is
O(1) per entry for data in canonical order and merely a wrong hint otherwise. An insertion that does not grow the
container is a **duplicate key** on the wire: the first value is kept, the rest of the image is still decoded, and
the read ends in `error::invalid_value` exactly as for a valueless variant (`deserialize` names the argument in
`decode_error::failed`; `dispatch` does not deliver the value; `deserialize_raw` and `walk_values` report it). The
dictionary-driven walkers (`verify`, `srdump`) accept such an image — its bytes are well-formed — and show both
entries. The slot rules and the byte budget of vectors apply (`take_n` with the entry stride).

**As arguments.** A frame written with a container argument (chapter 3 §3.3c) is read by every style:
`deserialize(frame, "ids"_, ids)` with any container of the shape (a `std::vector<int>` output for a
`std::vector<i32>` record; the type must match the writer's *transparent type* exactly, so a `std::map` output for
an `std::unordered_map` argument is `hash_mismatch` — the call string carries the container's name);
`dispatch<std::vector<int>>(frame, f)` delivers a decoded temporary; `frame_router::on("ids"_, [](const
std::vector<int>&) {…})`; `walk_values<std::vector<int>>(path, on_ids, rest)` claims the record through
`take_struct`; `inspect` shows it as a type record whose `hash` is the transparent type's schema hash and whose
`payload` is the image. `register_types<std::vector<int>>()` names it without a call site. Every dictionary-driven
renderer prints the value alone (`[1] [3, 1, 2]`, `ids=[3, 1, 2]`): a `transparent` type is walked as its one
field (§4.4).

**The other standard containers.** A `std::deque`, `std::list`, `std::forward_list` or `std::stack` output is the
`vector` shape: the sequence is `resize`d to the slot's count and filled in wire order (a stack bottom first, so
`top()` is the writer's top), and any of them reads a block a `std::vector` member wrote — as a *member*, where the
kindspec is the shape; as an *argument* the transparent type's name is the container's, so a `std::deque<int>`
output for a `std::vector<i32>` record is `hash_mismatch` like the map/`unordered_map` case above. A `std::pair`
or `std::tuple` output is a dynamic class whose parts (`first`/`second`, `_0`, `_1`, …) are decoded at their
canonical offsets through `std::get`, whatever the standard library's object layout. A `std::multimap` or
`std::unordered_multimap` is `clear`ed and every `std::pair<K,V>` entry of the block is `insert`ed at the end:
duplicate keys are data here, not `invalid_value`, and the writer's order among equal keys (a `std::multimap`'s
insertion order; stably sorted by key from an unordered multimap) is kept. The dictionary-driven readers see a
`vector` of the pair type (`srdump` prints `st: [10, 20]`, `first: -1`, `second: "b1"`). Tests:
`tests/unit/test_containers.cpp` "FR-A6 std::deque, std::list, std::forward_list and std::stack are the vector
shape", "FR-A6 std::pair and std::tuple are positional parts at canonical offsets, encoded through std::get",
"FR-A6 std::multimap and std::unordered_multimap are a vector of pairs sorted by key".

## 4.1d Reading older or newer frames: projection by member name (level 1)

Every reader above matches a type by its **schema hash**: a frame written by another build of the program, whose
`Order` gained a member or widened one, is `hash_mismatch` to `deserialize` (level 0 of
`docs/schema-evolution-specs.md`). Level 1 (DECISIONS R-9; `projection.hpp`, included by `serreg.hpp`) reads such a
frame when the writer's type **matches the reader's by canonical name** — the same `evo::Order`, or an old name
given by `[[=es::serreg::renamed_from("md::Quote")]]` on the reader's class — and both are **flat** (a type with a
string, vector, optional, variant, map or set stays at level 0). It needs the writer's dictionary (the capture
file's, `tools::load_dictionary_rri`, `extract_embedded_dictionary` of the old binary): the writer's type
description is what the reader projects from, member by member, by name:

| writer member → reader member | result |
|---|---|
| same name, same kind (R1); nested flat class with the same layout (R9) | copied |
| integer into a wider integer of the same signedness, unsigned into a strictly wider signed, `f32` into `f64` (R2–R4); enums by underlying kind (R8) | widened |
| writer-only member (R5) | skipped |
| reader-only member (R6) | the value `T{}` gives it — default member initialisers apply |
| narrowing, another kind family, `bool` ↔ integer, an array that shrank, a writer type not flat or not in the dictionary (R7) | **not projected**: the default is kept, the member is counted in `conflicts` and named in a note; the read succeeds |
| nested flat class with another layout (R9); array `x[N]` into `x[M ≥ N]` (R12) | recursed by member name; the first `N` elements copied, the rest defaulted |
| member `old` into `x` with `[[=renamed_from("old")]]` (R13); type `ns::Old` into `ns::New` with `[[=renamed_from("ns::Old")]]` (R14) | treated as the same name |

Two APIs. `deserialize(dict, bytes, outs...)` — or `deserialize(projection_context{&dict, &report}, bytes,
outs...)` to be told what happened — is style 1 with a dictionary: the exact path first (a frame of the reader's own
call costs nothing more than before), then the projection when each argument either matches exactly or is a flat
type that projects by name; literals and constants must match, and so must the argument count. Compact frames need
the writer's CALL entry in the dictionary, full-records frames carry the hashes themselves. `tools::project<T>(writer,
dict, bytes, &report)` fills one `T` from the raw bytes of one writer value (its `dyn::type` from the dictionary),
for code that has the record already — a `frame_router` handler, a `visit_record` callback. From
`tests/unit/test_evolution.cpp`, the reader's `evo::Order` against a writer whose `Order` had `u32 qty`, `Px price`,
`u16 lots[2]`, `i32 legacy` and `float weight`:

```cpp
struct Order {                                         // the reader's version
    std::uint64_t id;
    [[=es::serreg::renamed_from("price")]] Px px;      // R13: the writer called it `price`
    std::uint64_t qty;                                 // R2: was u32
    Side side;
    std::uint16_t lots[4];                             // R12: was [2]
    std::uint8_t flags;                                // R6: new, 0
    std::uint32_t venue = 7;                           // R6: new, its initialiser
    double weight;                                     // R4: was f32
};
const dyn::dictionary d = *tools::load_dictionary_rri(old_rri);   // the old build's; the test exports its own registry, both versions live in it
tools::projection_report rep;
evo::Order out{};
const auto r = deserialize(projection_context{&d, &rep}, frame, out);   // level 0 said hash_mismatch
// r: the frame's length; out.qty == 1000, out.px filled, out.lots == {3, 4, 0, 0}, out.venue == 7
// rep: copied 4 (id, px, side, lots), widened 2 (qty, weight), defaulted 2, skipped 1 (legacy), conflicts 0
```

**The report.** `projection_report` counts `copied`, `widened`, `defaulted`, `skipped` and `conflicts` per reader
member and keeps one note per member that is not a plain copy — `"evo::Order.qty: u32 -> u64 widened"`,
`"evo::Order.id: i64 -> u64 not projected (R7)"`, `"evo::Order.legacy: writer-only member skipped (R5)"`. The
counters are added on every read; the notes arrive once, with the frame that built the plan — log them, they are
the compatibility statement of that writer version. A writer's type that is not `T` by name is `hash_mismatch`;
too few bytes `truncated`; a writer type the dictionary lacks `unknown_type`.

**Costs.** The plan for a (writer type, reader type) pair — a table of (source offset, destination offset, kinds,
count) — is built once, on the heap, when the pair is first met, and kept for the process (a lock-free list per
`T`). Per frame nothing allocates or locks: a list walk, then one `memcpy` or one integer/float conversion per
step. The annotations are reader-side metadata: `renamed_from` changes neither the schema string nor the
dictionary, so an old binary is never affected by what a new one declares.

**What level 1 does not do** (`docs/schema-evolution-specs.md` §2.6): dynamic members (level 2, specified, not
scheduled); `dispatch`, `frame_router` and `walk_values<Ts...>` still match by hash; `srdump --diff` has no
`--compat` verdict yet; and a dictionary-only reader (`srdump`, Python) cannot see `renamed_from`. Tests:
`tests/unit/test_evolution.cpp` ("R-9 level 1: …", three cases); the mechanism: `files/include-es-serreg-projection.hpp.md`.

## 4.2 Inspect: `inspect(bytes[, &dictionary])` → `frame_view`

"What is in this buffer?" without knowing the signature:

```cpp
const auto fv = inspect(frame);                    // validated once; iteration cannot fail afterwards
for (const record_view r : fv->records())
    if (r.is_literal)      … r.hash …              // id; text via registry::instance().find_literal(r.hash)
    else if (r.ak == fmt::arg_string) … r.text() …
    else if (r.ak == fmt::arg_scalar) … r.k, r.payload …
    else                   … r.desc / r.dyn_desc, r.payload …   // type_desc from this process, or from the side dictionary
```

`record_view{hash, is_literal, desc, dyn_desc, payload, ak, k, is_const}`; for a dynamic type `payload` is the image
(prefix stripped); `is_const` marks a compile-time constant of the call, shown as the scalar or enum record
it stands for (§4.1b). Compact frames need the call signature — from the registry or the optional side
`dyn::dictionary` (a foreign capture's) — full frames do not. The range is lazy (no record cap, no copies).

## 4.3 Dispatch: `dispatch<Ts...>(bytes, f[, &dictionary])`

```cpp
dispatch<ex::Order, ex::Cancel, int>(frame, overloaded{
    [](const ex::Order& o) { … },       // zero-copy when the payload is aligned, else a stack copy — valid during the call
    [](const ex::Cancel& c) { … },
    [](int i) { … },                     // scalar records typed by kind
    [](std::string_view s) { … },        // string records, no Ts needed
    [](literal_view l) { … },            // {id, text}
    [](const record_view& r) { … }});    // anything not matched
```

For each record the first `T` whose hash matches is delivered. A record whose length disagrees with `sizeof(T)`
is not delivered and the call returns `length_mismatch` after the walk. A dynamic `T` is delivered as a decoded
temporary (never zero-copy).

Matching a record to its `T` costs a few inlined compares for up to four types (`detail::dispatch_linear_max`)
and, above that, one lookup in a perfect-hash table built at compile time from the `Ts` hashes
(`detail::hash_index`: a shift, a mask, one load, one compare) plus one indirect call — constant per record, however
long `Ts` is. Neither path walks the registry (`files/include-es-serreg-deserialize.hpp.md`).

## 4.4 Dynamic visitor: `tools::visit_record` (what `srdump` uses)

```cpp
struct my_visitor : tools::decoded_value_visitor { /* begin_struct, scalar, enumerator, literal, … */ };
tools::visit_record(payload, *dyn_type, dictionary, visitor);
```

Descriptor-driven, allocation-free, bounds-checked at every step; works on any capture given only its
dictionary (`tools::load_dictionary_rri`, `extract_registry_from_elf`, or the one embedded in the file). The
dynamic-member events (`string`, `begin_vector`, `null_value`, `begin_variant`, …) have defaults expressed through
the flat events, so a visitor that implements only those keeps working; so do the container events —
`begin_map(field, n)`, `begin_entry(i)`, the key
as `"key"` and the value as `"value"`, `end_entry()`, `end_map()`; `begin_set(field, n)`, `end_set()`;
`begin_transparent(type_name, field)`, `end_transparent()` — whose defaults show a map as a vector of `entry`
structs, a set as a vector and a transparent record as a struct named after the container. `walk_values`
forwards all of them to the `rest` visitor. The text and JSON formatters are the reference implementations. A
dictionary-driven reader needs no registration of the types it reads; a program that wants its *own* registry
(and so its embedded dictionary, `--elf`, `find_type`) to know a type it never serialises names it with
`register_types<T>()` (chapter 5 §5.1a).

## 4.5 Zero-copy view: `view<T>(record_view)`

`result<const T*> p = view<ex::Order>(r)` — a pointer into the buffer when the hash matches, the length is
`sizeof(T)` and the address is aligned (`misaligned` otherwise; compact records are 8-aligned, so any type
with `alignof ≤ 8` qualifies). Flat types only. A view cannot repair bytes it does not own, so a record with a
`bool` byte other than 0/1 or an out-of-range leaf of an enum without a fixed underlying type (when the range
check is on) is `invalid_value`; `deserialize` reads the same record into a copy.

## 4.6 Walking a capture: `tools::verify`, `tools::dump`, `tools::render_frame`

```cpp
auto rep = tools::verify("capture.ser"[, &extra_dict]);   // read in windows — a capture of any size
auto rep2 = tools::verify(bytes[, &extra_dict]);          // or over a span; frames, records, bytes, problems[]; ok()
tools::dump("capture.ser", &extra, dump_options{.fmt = tools::format::json}, out_string);   // windows
tools::dump(bytes, &extra, dump_options{.fmt = tools::format::json}, out_string);           // or the streaming overload
auto text = tools::render_frame(frame, dictionary);   // a print frame's message ("px=2.50 qty=7\n")
```

`verify` decodes every record (it runs the visitor), checks literal ids in full records, string and image
lengths, that a constant in a full-records frame equals the value in its call's entry, and reports
dictionary conflicts; problems are strings like
`frame 3 @1384: record 1: literal id mismatch: …`. `dump` streams one frame at a time through a callback and
renders print frames as messages (`dump_options::raw` to see the records too). Both take a path as well as a
span (§4.6a): over a path the file is read in windows and the state between frames is the
dictionary and the counters, so a capture is bounded by the file system, not by the tools. A report keeps at
most `decode_limits::problems` lines (1000); `verify_report::problems_omitted` counts the rest and `ok()` is
false while any were omitted. `verify_report::program_hash` is the capture header's, 0 for a bare
stream.

**Meta frames and the 1.7 integrity checks.** Every reader accepts formats 1.6 and 1.7. Dictionary frames
(full images, and since 1.7 the deltas a capture file writes after a `dlopen`), checkpoint frames and the
writer-info frame are *meta frames*: the walkers consume them — a dictionary frame is merged into the dictionary
before the next frame is delivered — and never hand them to a callback or list them in a dump. They do count in
the frame numbering (`frame_ref::index`, `srdump --frames`), so frame numbers match the file, but
`verify_report::frames`/`bytes` and `frame_walk::frames` count data frames only. A frame with a CRC32C trailer
(chapter 7 §7.9) is checked before anything in it is believed: `deserialize`, `inspect`, `view` and `dispatch`
return `error::crc_mismatch`; `verify`, `dump`, `for_each_frame` and `walk_values` report `frame N @off: crc
mismatch (frame skipped)` and go on with the next frame, so a damaged frame is never decoded and never delivered.

## 4.6a Captures of any size, and the reader limits (FR-D5, FR-D11)

Every reader over a capture on disk or a pipe — `for_each_frame(path)`, `walk_values(path)`, `verify(path)`,
`dump(path)`, and every `srdump` path over a capture — works on a **window**: the file is read
`decode_limits::window_bytes` (10 MiB) at a time with `pread` (`read` on a pipe); a frame that straddles a window
boundary is completed from the next window; a frame larger than the window grows the buffer to that frame,
within `decode_limits::frame_bytes` (64 MiB), and the buffer is released when the walk ends. A FILE's header and
dictionary are read first (with `pread` at `dict_offset` when a 1.6 file's dictionary was back-patched to the
end), then the frames from the first frame offset (after the 16-byte CHAIN in a 1.7 file). Nothing in `verify` or `dump` grows with the number of frames. The
windows are read, not `mmap`-ed (`docs/deviations.md`: the difference is one copy per window; the whole-file
mapping stays available as `{.blocks = false}`). A 64 MiB capture with a 4 MiB frame in the middle walks and
verifies with a 1 MiB window inside a bounded RSS, and the golden captures give the same text, JSON and TOML
byte for byte with a 4 KiB window, the default window and the whole file in memory (`tests/unit/test_limits.cpp`).

The window is one of seven **reader limits**, `es::serreg::decode_limits` (`limits.hpp`,
`files/include-es-serreg-limits.hpp.md`), the code form of the never-crash contract (FR-D5, `docs/SECURITY.md`): for
any sequence of input bytes every public reading API returns a value or an error, and never crashes, executes
undefined behaviour, or allocates or loops beyond these limits. The defaults are chosen so that no honest data
reaches them:

| limit | default | what a reader does at it |
|---|---|---|
| `frame_bytes` | 64 MiB | a larger `frame_len` is a `bad frame_len` problem and the walk stops — or, with `--resync`, continues at the next checkpoint frame (§4.6e) |
| `dictionary_bytes` | 256 MiB | a FILE's dictionary above it is `unsupported`; a dictionary frame above it is skipped with a problem |
| `dictionary_entries` | 1 000 000 | more declared entries is `corrupt_dictionary` |
| `image_depth` | 64 | the dictionary-driven decoder follows no deeper nesting (`fmt::max_nesting` is the ceiling) |
| `input_bytes` | 1 GiB | the largest input `read_file` reads whole: an RRI, an ELF, a config file, a span reader's input |
| `window_bytes` | 10 MiB | the window above (4 KiB floor) |
| `problems` | 1000 | problem lines a walk keeps; the rest are counted (`… and N more`) |

They are process-wide — `get_decode_limits()` / `set_decode_limits()` — and per call wherever a reader takes
`block_options` or `file_options` (`.limits = &mine`); a walk reads them once, not per frame. `srdump --window
<size>` chooses the window for a run and `--frame-limit <size>` sets `frame_bytes` process-wide (chapter 10).
Outside the contract: system out-of-memory within the limits, policies the user asked for
(`ES_SERREG_CONFLICT=abort`), the writer side, and `es::ipc`, whose rings validate their headers by their own
rules. The fuzz driver checks the contract by gating every input on time and memory as well as on crashes
(`files/tests.md`).

## 4.6b Walking a capture yourself

```cpp
auto walk = tools::for_each_frame("capture.ser", [&](const dyn::dictionary& dict, const tools::frame_ref& f) {
    if (deserialize(f.bytes, book)) { … }                                 // typed readers take f.bytes as is
    else if (f.is_print()) std::print("{}", *tools::render_frame(f.bytes, dict));
    return true;                                                          // false stops
});                                                                       // windows of 10 MiB, any size
auto walk2 = tools::for_each_frame(path, same_callback, {.blocks = false}); // map the whole file instead (mmap, no copy)
// the pieces underneath, still public:
auto file  = tools::mapped_file::open(path);
auto walk3 = tools::for_each_frame(file->bytes(), same_callback);         // any span: a mapped file, a buffer
auto walk4 = tools::for_each_frame_in_file(path, same_callback, {.block_bytes = 64 << 20});   // a 64 MiB window
```

The callback sees every data frame in order, together with the dictionary as known at that frame: the embedded
one, plus any dictionary frames (full or delta) seen so far, plus an optional `extra`; meta frames are not
delivered, and a frame whose CRC fails is reported and skipped. `frame_ref` carries the frame's bytes,
offset, call hash, record count and call ENTRY (`is_print()`). The result lists `verify`-style problems; a frame
that cannot be delimited ends the walk.

`for_each_frame(path, f, file_options = {}, extra = nullptr)` chooses the reading strategy:
`tools::file_options{bool blocks = true; std::size_t block_bytes = 0; bool populate = false; const decode_limits*
limits = nullptr}`. The default (A6) is the windowed reader (§4.6a): the file is read
`block_bytes` at a time — `decode_limits::window_bytes`, 10 MiB, when 0 — by `for_each_frame_in_file`, and
`frame_ref::bytes` is valid only during the callback. With `{.blocks = false}` a regular file is mapped instead
(`mapped_file`, `populate` = `MAP_POPULATE`) and walked in memory; `"-"` (stdin) and anything that is not a
regular file (a pipe, a device) are always read in windows. Frame offsets are file offsets either way, and the two strategies deliver the same frames
(test "2.1.10 for_each_frame(path) and walk_values(path): mapped and in blocks agree"). A missing file is
`error::not_found`.

## 4.6c Typed where compiled, generic elsewhere: `tools::walk_values<Ts...>`

```cpp
tools::walk_values<demo::Order, demo::Fill>(file->bytes(),
    overloaded{[](std::string_view field, const demo::Order& o) { … },   // decoded into the C++ type
               [](std::string_view field, const demo::Fill& f) { … }},
    rest_visitor);                                                     // everything else, field by field
```

The check is made for each record and then for each nested struct value (member, array or vector element,
variant alternative, base), outermost first. A value whose schema hash matches one of `Ts` is decoded into that
type and delivered, and its interior is skipped. Any other value is walked through the file's dictionary to
`rest`, and the check repeats one level down. `walk_values_in_file` does the same over blocks. It works because the
visitor's `take_struct` hook can claim a struct value before it is walked (`files/include-es-serreg-walk.hpp.md`).

The walk also takes a path, with the same `file_options` as `for_each_frame(path)`, and a generic walk
needs no dummy typed handler:

```cpp
tools::walk_values<demo::Order>("capture.ser", on_order, rest_visitor, on_frame);   // windows (the default)
tools::walk_values<demo::Order>("capture.ser", on_order, rest_visitor, on_frame, {.blocks = false}); // mapped
tools::walk_values("capture.ser", rest_visitor);          // no compiled types: every value goes to rest_visitor
tools::walk_values(capture_bytes, rest_visitor, on_frame); // the same over a span
```

`on_frame` (optional) runs before each frame's records; returning `false` skips that frame.

## 4.6d One handler per call signature: `frame_router`

```cpp
frame_router router;
router.on([&](const demo::Book& b) { … })                                   // serialize(out, book)
      .on("order"_, [&](const demo::Order& o) { … })                        // serialize(out, "order"_, order)
      .on_println("  fill #{} of order {}: {} @ {:.2f}"_,                    // println(out, fmt, …)
                  [&](std::uint64_t exec, std::uint64_t order, std::int32_t qty, double px) { … })
      .on<demo::Cancel, decltype("why"_lit)>([&](const demo::Cancel& c) { … }) // any signature, literals anywhere
      .otherwise([&](const dyn::dictionary& d, const tools::frame_ref& f) { … }) // no handler for this call hash
      .on_error([&](const tools::frame_ref& f, error e) { … });              // a handler, but the frame did not decode
auto walk = tools::for_each_frame("capture.ser", router.callback());        // a whole capture
result<bool> r = router.route(frame);                                       // or one raw frame (a ring consumer)
```

The style for reading a whole capture of your own types. Each handler's parameter types name the signature it
receives; the router keeps a hash table from `call_hash_v<Sig...>` to a decoding closure, so each frame costs one
lookup, then the same typed reader as `deserialize` (validation first, then the outputs), then the handler call.
The decoded values live in storage the router keeps between frames, so strings and vectors keep their capacity.
`handled()`, `unhandled()` and `failed()` count the frames seen through the callback and through `route()` (T-10); `route()` also passes a frame that fails to decode to `on_error`. `otherwise`
is called only from the callback: it needs the capture's dictionary, which a raw frame does not carry, so `route()`
only counts a frame without a handler and returns `false`. Registering a signature again replaces its handler, and
a handler's return value is ignored. The parameters must be exactly the written types: a handler taking
`std::int64_t` for an `std::int32_t` argument names another call hash, and those frames go to `otherwise`. The
router is neither copyable nor movable (`callback()` holds a pointer to it), so build it where it
will stay and keep it alive for the whole walk. One router per reading thread. Details in `files/include-es-serreg-router.hpp.md`;
`examples/user_example1/reader1.cpp` is a complete reader built on it.

**Log frames.** `router.on_log([&](std::uint8_t level, std::string_view line) { … })` receives every log
frame (chapter 3 §3.9) that has no handler of its own — any level, any call site, any format — already rendered
as `srdump --level` prints it (`2026-10-07 14:03:11.123456789 WARN  main.cpp:42 book AAPL depth 7`, no trailing
newline), with the level as a number (0 trace … 5 fatal). It needs the capture's dictionary, so like `otherwise` it
works through the walk callback, not through `route()`.

To split the log by level, register floors instead of (or beside) the line handler:

```cpp
router.on_log(level::warn, [&](const tools::log_record& r) { alert(r.level, r.file, r.line, r.message); })
      .on_log(level::trace, [&](const tools::log_record& r) { keep(r.time_ns, r.message); });
```

`on_log(min_level, h)` (the level as `es::serreg::level` or a plain `std::uint8_t`) takes every log frame whose
level is at or above `min_level` and below the next registered floor: a frame goes to exactly one handler, the one
with the highest floor at or below its level. The handler receives a `tools::log_record` — the envelope taken
apart (`level`, `file`, `line`, `mask`, `time_ns`, `thread`/`sequence` with `has_thread`/`has_sequence`), the
format literal's text (`format`) and the message rendered *without* the envelope prefix (`message`). The level is
a constant of the call's dictionary entry, so a frame below the lowest floor is dropped without being rendered
or decoded (counted in `handled()`, not `unhandled()`); a plain `on_log(h)` acts as the floor `trace` for lines
and takes what no floor claims. `tools::render_log(frame, dict)` gives the same `log_record` for one frame
outside a router.

## 4.6e What a file says about itself, and resynchronisation (format 1.7)

```cpp
auto ci = tools::info("capture.ser");      // header, dictionary chain, checkpoints, writer info — no frame walk
// ci->minor, ci->flags, ci->program_hash, ci->abi, ci->file_size
// ci->dict                 the opening dictionary merged with every delta the chain reaches
// ci->dictionary_offsets   the opening block, then each linked dictionary frame
// ci->checkpoints          offset, prev, next, data_frames_before, bytes_before, wall_clock_ns,
//                          dictionary_offset, registry_version, full_dictionary — in order
// ci->writer               the writer-info items (key, text); ci->writer_item(fmt::info_program)
std::print("{}", tools::to_text(*ci));     // or tools::to_json(*ci) — what srdump --info prints
```

`tools::info(path)` reads the header and the opening dictionary, then — for a 1.7 file — follows the CHAIN's two
forward chains with seeks: the dictionary deltas (merged into `dict`) and the checkpoints, and reads the
writer-info frame. The cost is one read per link, whatever the file's size, which makes it the way to get a
multi-terabyte capture's dictionary or to find the checkpoint at a given time or frame count (each checkpoint
names the latest dictionary at that point, and with `capture_options::checkpoint_dictionary` carries the full
dictionary itself). A link must point forward and at a frame of the right kind; a file cut while it was written
simply ends its chains early. A 1.6 file yields its dictionary and no chains; a pipe is `unsupported`. `srdump
--info` and `python3 -m serreg --info` print the same.

**Resynchronisation.** Without it a corrupt `frame_len` ends the walk: the frames after it cannot be delimited.
With `tools::block_options::resync = true` (`srdump --resync`) the windowed reader instead scans forward 8 bytes
at a time for the next checkpoint frame — a valid length, the checkpoint flag and the `"ESSRCKPT"` sentinel
together — reports `bad frame_len N: skipped K bytes to the next checkpoint` as one problem, and continues there;
a file without checkpoints has nothing to resynchronise to. Only the windowed reader (the path overloads,
`srdump` on a file) resyncs; a walk over a span stops at the bad frame. ("P 1.7 --resync: after a corrupt
frame_len the windowed reader continues at the next checkpoint", `tests/unit/test_format17.cpp`.)

## 4.7 Which reader when

| need | use |
|---|---|
| my own types, known signature, fastest | `deserialize` (typed) or `dispatch` (per record, zero-copy); a container argument reads into any container of its shape |
| my own flat types as an older or newer build wrote them | `deserialize(dict, frame, …)` / `tools::project<T>` (§4.1d), with that build's dictionary |
| a whole capture (or ring) of my own types, several signatures | `frame_router`: `router.callback()` for a walk, `route(frame)` per frame |
| ring/IPC consumer | `dispatch` with an overload set, or `frame_router::route` |
| unknown frames from my own process | `inspect` |
| a capture from another program | `inspect(bytes, &dict)` / `tools::visit_record` / `srdump` |
| walk a capture file frame by frame, whatever its size | `tools::for_each_frame(path)` (windows of 10 MiB, `{.blocks = false}` to map), or over a span (`tools::mapped_file`, a buffer) |
| my types where I have them, generic walk for the rest | `tools::walk_values<Ts...>(path \| span, …)`; `tools::walk_values(path \| span, visitor)` for no types |
| verify a file before trusting it | `tools::verify(path)` (windows) / `srdump --verify` |
| logging | `tools::render_frame` / `srdump` text mode; log frames: `srdump --level`, `--since`/`--until`, `frame_router::on_log` |
| a huge or damaged file: overview, dictionary, checkpoints | `tools::info(path)` / `srdump --info` (by seeks); `--resync` to read past a corrupt frame |
| no C++ at all | `python3 -m serreg` (`book/python-serreg.md`) |
