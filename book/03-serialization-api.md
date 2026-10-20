# 3. The writer API

Everything here is in `<es/serreg/serreg.hpp>` (umbrella) — the pieces live in `serialize.hpp`, `sink.hpp`,
`capture.hpp`, `literal.hpp`, `print.hpp`, `arg.hpp` and `log.hpp`; `to_string.hpp` (§3.8) is
included separately because it links `es::serreg_tools`.

## 3.1 `serialize` and friends

```cpp
template<sink S, serializable_arg... Ts>
result<std::size_t> serialize(S& out, const Ts&... vs);                       // one frame, bytes written
result<std::size_t> serialize_with(serialize_options o, S& out, const Ts&... vs);
template<fixed_string L> result<std::size_t> serialize(S& out, const Ts&... vs);   // = serialize(out, lit<L>{}, vs...)
result<std::size_t> serialize_raw(S& out, const T& v, bool scrub_padding = true);  // payload only, no frame (FR-S5)

struct serialize_options { bool scrub_padding = true;        // FR-S6: zero padding, normalise bools
                           bool full_records = false;        // FR-W2: self-describing records (archival)
                           bool crc = false;                 // format 1.7: an 8-byte CRC32C trailer
                           bool canonical_unordered = true; }; // unordered containers sorted by key on the wire
```

`crc` appends `u32 crc32c(frame bytes before it), u32 0` and sets frame flag bit 2; every reader then checks the
frame before decoding it and skips it with a problem (`error::crc_mismatch` from the typed readers) when it does
not match (chapter 7 §7.9). A sink can ask for it on every frame: a capture file opened with
`capture_options{.crc = true}` returns true from `frame_crc()`, and `serialize` honours that whatever the
options say. Off, it costs nothing; on, one CRC pass over the frame (SSE4.2 where available).

`result<T>` is `std::expected<T, error>`; the library never throws. The call is `noexcept` exactly when the sink's
`write`/`claim`/`commit` are (`vector_sink` allocates, so it is not). Errors: `sink_full` when a write is refused
and the sink has no state, otherwise the sink's `state().error()` (`io_error` for `fd_sink`/`capture_file`),
`invalid_argument` for a frame beyond 4 GiB.

**Frame size.** `frame_size<Ts...>(opts)` is `constexpr` and exact when no argument is a runtime string or a
dynamic type; `frame_size(opts, vs...)` is exact always (one measure pass, no allocation);
`has_runtime_size_v<Ts...>` tells you which case you are in. A claiming sink is asked for exactly that many bytes.

**The hot path.** For flat arguments into a claiming sink: claim → 16-byte header → `memcpy` per object →
zero the padding gaps and normalise bools from a compile-time table → commit. No allocation, no lock, no
registry access (registration happened at load). Dynamic arguments add one measure pass and the image
encode; still no allocation on a claiming sink.

**Plain sinks** (only `write`): frames up to 64 KiB are assembled in an exactly-sized stack buffer; larger flat
records are streamed through a 16 KiB chunk (scrub applied per chunk); frames with runtime strings up to 4 KiB
use a 4 KiB stack buffer; a *single dynamic record* larger than 16 KiB is the one case that heap-allocates
(the image needs random access while its tail offsets are patched) — use a claiming sink such as
`capture_file` when that matters. A frame with a CRC trailer above those stack sizes is assembled on the heap too
(the checksum covers the whole frame, so it cannot be streamed).

## 3.2 Sinks

```cpp
template<class S> concept sink          = requires(S& s, const std::byte* p, std::size_t n) { { s.write(p, n) } -> std::same_as<bool>; };
template<class S> concept claiming_sink = sink<S> && requires(S& s, std::size_t n) { { s.claim(n) } -> std::same_as<std::byte*>; { s.commit(n) }; };
// optional: status state() const noexcept — reported instead of sink_full when a write is refused
// commit(n) returns void, or bool: false ⇒ the frame was not written, and serialize() fails
// optional: using dictionary_scope = Tag;  — a scoped sink (§3.4, §3.9)
// optional: bool frame_crc() const       — every frame written into it carries the CRC trailer
template<class S> concept scoped_sink = sink<S> && requires { typename S::dictionary_scope; };
```

| sink | kind | notes |
|---|---|---|
| `span_sink{std::span<std::byte>}` | claiming | bounded buffer; `bytes()` = used part; refuses (no wrap) when full |
| `vector_sink{std::vector<std::byte>&}` | claiming | appends; allocates (not `noexcept`) |
| `fd_sink{int fd}` | plain | blocking descriptor; EINTR retried; sticky `io_error` in `state()`, `reset()` |
| `capture_file` | claiming | FILE container (§3.4); grows one internal buffer to the largest frame; `commit` returns the `fwrite` result, so a frame the file system refuses fails that `serialize` (C-09) |
| `scoped_capture_file<Tag>` | claiming, scoped | the same file, whose dictionary holds only the calls serialised into files of scope `Tag` (§3.4) |
| `es::ipc::sink` | claiming | a record of an `es::ipc` SPMC ring or SPSC queue, of the message type the sink was given; `claim` spins on a full ring (`full_policy::spin`) or returns `nullptr` (`fail`); `commit` refuses without an open claim or beyond the claim — chapter 11, `files/include-es-ipc-sink.hpp.md`, `examples/ipc_ring.cpp` |
| your own | either | a ring/disruptor slot is the intended `claim/commit` use |

## 3.3 Arguments

Five categories (chapter 1.3). What matters in practice:

- **Literals** — `"order rejected"_` (or `_lit`, `lit<"…">{}`, `serialize<"…">(out, …)`, `ES_SERREG_LIT`). The
  identity is in the *type*; the text is interned once per image; the wire carries nothing in compact frames.
  A named constant is fine: `constexpr es::serreg::lit<"order rejected"> k_rejected;`.
- **Runtime strings** — `"abc"` without a suffix, `std::string`, `std::string_view`, `const char*` (NUL-terminated;
  `nullptr` = empty; a `char[N]` is text up to its first NUL). Bytes travel. Read back into `std::string` (copy)
  or `std::string_view` (view into the frame).
- **Scalars** — every arithmetic type, `bool`, char types, `std::byte`. Named by kind (`i32`, `f64`, …) in the
  call string, so `int` on one side and `std::int32_t` on the other are the same call.
- **Flat and dynamic types** — by schema hash. `as_flat<T>` maps a non-flat top-level type onto a flat image
  (`using type = U; static U to(const T&); static T from(const U&);`), top level only. A
  **container** — `std::vector<E>`, `std::optional<E>`, `std::variant<…>`, or any of the six map/set containers —
  is a type argument of its own (§3.3c): `serialize(out, "ids"_, ids)` with a `std::vector<int>` compiles and is
  the record of the container's *transparent type*.
- **Constants** — `42_`, `-7_`, `1.5_`, `k<Side::buy>`: a number or enumerator fixed at compile time.
  The value is part of the call signature and costs nothing in a compact frame (§3.3b).

Order and categories define the **call hash**; two call sites with the same argument categories share one call
ENTRY, and `deserialize` demands the same categories in the same order.

**What is not an argument.** Pointers, references, unions, bit-fields and virtual classes. Such an argument
fails on a deleted overload (B10) with a message — `es::serreg::serialize: an argument is not a
serialisable category (a class or enum, a literal, a constant, a scalar, a runtime string, or — since 2.2.0 — a
container). Pointers, references, unions, bit-fields and virtual classes are never serialised; map them with
as_flat<T>.` — instead of an overload-resolution failure (`serialize.hpp`, the
`requires(!(serializable_arg<Ts> && ...))` overloads of `serialize` and `serialize_with`). A top-level
`std::vector` was once in that list too ("wrap it in a struct"); it is now a container argument, and the negative
test is `neg_pointer_argument` (`tests/negative/`).

## 3.3c Containers as arguments (format 1.6)

```cpp
std::vector<std::int32_t> ids{3, 1, 2};
std::map<std::string, double> px{{"b", 2.5}, {"a", 1.5}};
serialize(out, "ids"_, ids);                      // 48 bytes: header + {u32 20, u32 0} + slot {8,3} + 3 × i32, padded
serialize(out, "ids"_, std::vector<std::int32_t>{}); // 32 bytes: the record is u32 8, u32 0, the slot {0, 0}
println(out, "ids={} px={}"_, ids, px); // renders as  ids=[3, 1, 2] px={"a": 1.5, "b": 2.5}
std::optional<Order> maybe; std::set<int> s;
serialize(out, px, maybe, s);                    // any dynamic member type, in any position
```

A container passed as an argument is a type argument (`arg_type`, chapter 6 §6.2) whose type is its **transparent
type**: a synthetic struct ENTRY whose canonical name is the container's vocabulary name — `std::vector<i32>`,
`std::map<std::string,f64>`, `std::unordered_map<i32,std::string>` (a comparator, hasher or equality other than
the defaults is part of the name, the allocator is not) — with one unnamed field of the member kind at offset 0,
size 8, align 4, and the struct flag `transparent`. It is registered like any type (`registrar`, `find_type`,
`find_type_by_name("std::vector<i32>")`, the `es_serreg_dict` section, `register_types<std::vector<int>>()`),
its schema hash is the call string's item, and its record is the ordinary dynamic-type record `u32 len | u32 0 |
image`, where the image is the container's slot followed by its tail — exactly the bytes the container produces
as the only member of a struct (chapter 7 §7.5b). `dynamic_type<T>` admits every dyn member type but
`std::string`, which stays the `string` category (a top-level `std::string` is a runtime string, as before), and
`std::array<E, N>` of a flat `E` stays a flat class. On the reading side `deserialize(frame, "ids"_, ids)`,
`dispatch<std::vector<int>>`, `frame_router::on`, `walk_values<std::vector<int>>` and `inspect` all take it
(chapter 4 §4.1c); every dictionary-driven renderer prints the value itself (`[1] [3, 1, 2]`, chapter 10). A
transparent type's name is the exact container type, so the same values sent as a `std::map` and as a
`std::unordered_map` top-level argument are two calls with two call hashes — while as *members* the two give the
same hashes (§8.1b). Frozen: `schema_hash_v<std::vector<std::int32_t>> == 0x02c87ef3a015eb45`
(`tests/golden/hashes.cpp`); tests `tests/unit/test_containers.cpp`.

**The rest of the std vocabulary.** As members and as arguments alike: `std::deque<E>`, `std::list<E>`,
`std::forward_list<E>` and `std::stack<E>` (its underlying container, bottom first) are the `vector` shape — a
`std::deque<int>` member writes the bytes a `std::vector<int>` member writes, with the same schema and layout
hashes; as an argument it is its own transparent type `std::deque<i32>` (same layout hash as `std::vector<i32>`,
another schema hash). `std::pair<A,B>` and `std::tuple<Ts…>` are positional dynamic classes — parts `first`/`second`
or `_0`, `_1`, … laid out at canonical offsets like a struct's members and always encoded element-wise through
`std::get`, never `memcpy` (a libstdc++ tuple stores its elements in reverse); a `std::pair<int, double>` argument
is the 16-byte image `dyn<std::pair<i32,f64>>[16,8]{first:i32@0[4,4];second:f64@8[8,8];}`.
`std::multimap<K,V>` and `std::unordered_multimap<K,V>` are a `vector` of `std::pair<K,V>` sorted by key (stably,
so the entries of one key keep the container's order; duplicate keys are data) with the schema kindspec
`multimap<K,V>` and the layout of a `std::vector<std::pair<K,V>>`. **Every other standard-library class is
rejected** by `check_supported` with a message that lists the vocabulary and names the way out (`as_flat<T>`, or
`es::serreg::type_properties<T>` for a type one takes responsibility for); smart pointers and `std::tuple<>` have
messages of their own (`tests/negative/neg_multimap_member.cpp` — a `std::priority_queue` —,
`neg_smart_pointer_member.cpp`, `neg_empty_tuple_member.cpp`). Golden strings: `tests/golden/hashes.cpp` "golden
strings of std::deque/list/forward_list, std::pair/tuple, multimaps and std::stack".

## 3.3b Compile-time constants (format 1.5)

```cpp
using namespace es::serreg::literals;                             // the _ operators
using es::serreg::k;
serialize(out, "fill"_, 42_, order, -7_, 1.5_, k<Side::sell>);   // 16-byte header + the Order: 48 bytes
println(out, "limit={} on={}"_, 5'000'000'000_, k<true>);   // a 16-byte frame
```

`_` builds a constant from a number the way it builds a literal from a string. A constant can stand at any
argument position, next to run-time values.

**Spelling.**

| you write | you get |
|---|---|
| `42_`, `0x10_`, `0b101_`, `017_`, `1'000'000_` | `constant<V>` with `V` an `int`; a value that does not fit is a `long long`, and one that does not fit that an `unsigned long long` |
| `-7_`, `+7_` | unary minus and plus on a constant: `constant<-7>`, `constant<7>` |
| `1.5_`, `2.5e-3_`, `1e3_` | a `double` with the value the compiler gives the same spelling |
| `k<true>`, `k<'x'>`, `k<42u>`, `k<1.5f>`, `k<Side::buy>`, `k<std::byte{7}>` | every other constant: `k<V>` is `constant<V>` for a scalar or an enum `V` |

The string forms `"text"_` and `"text"_lit` are unchanged: `"42"_` is still a literal. Code:
`files/include-es-serreg-literal.hpp.md`.

**What it costs on the wire.** Nothing in a compact frame. The frame's `n_records` counts the constant, but there
are no bytes for it: a frame of a literal and constants only is its 16-byte header, and
`frame_size<lit<"fill">, constant<42>, Order>()` equals `frame_size<Order>()`. The value is written once, in the
call's dictionary entry (chapter 7 §7.4). In a full-records frame (`serialize_options{.full_records = true}`) a
constant is written as the scalar or enum record a run-time value would be, so such a frame stays self-describing.

**How it enters the call hash.** The type and the value are an item of the call string:

```
serialize(out, "evt"_, 42_, "abc")     call(3){lit:0x198e4c6da0847235,const:i32=42,string}
k<true>                                const:bool=true
1.5_                                   const:f64=0x3ff8000000000000      (floating values by bit pattern)
k<Side::sell>                          const:0x074e267cca5188e1=2        (the enum's schema hash, the underlying value)
```

So `42_`, `43_`, `k<42u>` and a run-time `int` are four different calls with four call hashes. A reader names the
same constant (`deserialize(bytes, "fill"_, 42_, order)`, chapter 4 §4.1b); a tool that has only the dictionary
shows the value from the call's entry.

**When to use one.** For a value that is the same in every frame of a call site and that the reader should still
see: a message subtype, a version, a side fixed by the code path, a threshold in a log line. A run-time scalar
costs 8 bytes in every frame; a constant costs 8 more bytes in the call's dictionary entry, once.

**Limits.**

- `V` is a scalar (`bool`, a character type, an integer, `float`, `double`, `std::byte`) or an enum. Class-type
  constants (`constant<Venue{…}>`) are not supported.
- A character literal with `_` (`'c'_`) is not supported: the language passes it to a literal operator as a
  run-time value. Write `k<'c'>`.
- A floating literal with `_` is a decimal literal with at most 15 significant digits whose decimal exponent,
  after the fraction digits are counted, is within ±22 — the range in which the library computes exactly the
  compiler's value. Anything else, and every hexadecimal floating literal, is a compile error that says to write
  `k<value>`, which takes any `double` or `float` constant expression (`k<0x1p3>`).
- An integer literal with `_` does not try unsigned types before `long long`: `0xFFFFFFFF_` is a `long long`.
  Write `k<0xFFFFFFFFu>` for an `unsigned`.
- `print` checks and renders a constant as a run-time value of its type; `k<std::byte{…}>` and the
  wide character types can be serialized and not printed, like their run-time forms (§3.5).
- Constants are why the wire format moved to 1.5; a 1.4 reader refuses such a file (`bad_version`).

## 3.4 `capture_file` — the FILE container

```cpp
capture_file f{"orders.ser"};   // or f.open(path, options); failure is sticky: is_open() false, state().error() why
serialize(f, …);                // frames go straight into the file's buffer (claiming sink)
if (!f.close()) …               // [[nodiscard]]: flush (+ final checkpoint, fdatasync); nothing is rewritten
```

Opening writes the 48-byte header, the current registry as the opening dictionary (an RRI), the 16-byte CHAIN and
(by default) the writer-info frame — format 1.7, chapter 7 §7.2–§7.2b. If a `dlopen` registers more types while
the file is open, the next frame is preceded by a **dictionary delta** frame (only the new entries) linked forward
from the previous dictionary by one 8-byte `pwrite` (FR-W7, chapter 7 §7.9). Nothing is ever rewritten: a file is
readable up to its last complete frame at every instant, also after a crash. (In format 1.6 `close()` appended a
fresh RRI and back-patched the header; such files are still read.) The destructor closes and prints one
`stderr` line on failure (it never aborts a process over an I/O error).

**Options (format 1.7)** — per file, at run time; every default writes what 1.6 wrote:

```cpp
capture_options o;
o.crc = true;                            // a CRC32C trailer on every data frame
o.checkpoint_bytes = 64 << 20;           // a checkpoint frame every 64 MiB of frames …
o.checkpoint_every = std::chrono::seconds(10);   // … and/or every 10 s (the clock is read every 64 frames)
o.checkpoint_dictionary = false;         // true: each checkpoint carries the full dictionary (seek without the chain)
o.writer_info = true;                    // the writer-info frame (srdump --info shows it)
o.sync = sync_policy::dictionary;        // none | dictionary (default) | checkpoint | frame — when to fdatasync
o.program = "md-capture";                // recorded in the writer-info frame
o.info_host = true;                      // hostname, pid, start time (false: reproducible files)
o.info_thread = false;                   // the opening thread's id (a logger's thread_policy::once)
capture_file f{"md.ser", o};
f.checkpoint();                          // one now
```

A checkpoint frame records how many data frames and bytes precede it, the wall clock, the offset of the latest
dictionary and the registry version; checkpoints form a forward chain, so a reader can seek into a file of any size
(`srdump --info`, `tools::info`) and resynchronise after a damaged frame (`srdump --resync`). `sync_policy::frame`
is for tests and writers that must not lose one message; the default makes each dictionary delta durable before
the frames that need it.

**Compile-time policy and scopes.** The template behind the aliases is
`basic_capture_file<Scope = void, capture_policy P = {}>`:

```cpp
using capture_file = basic_capture_file<>;                                  // the whole registry
template<class Tag> using scoped_capture_file = basic_capture_file<Tag>;    // a dictionary scope
basic_capture_file<void, capture_policy{.dictionary_updates = false}> f;    // never dlopens: no per-frame check
```

`capture_policy::dictionary_updates = false` removes the one relaxed load per frame that detects a registry
change; use it when nothing is ever `dlopen`ed. A **scoped capture file** declares `using dictionary_scope = Tag;`
(a `scoped_sink`): every call serialised into a sink of that scope registers a compile-time (scope, call) node,
and the file's dictionary — opening block and deltas — holds only those calls, closed over their argument types and
literals (`registry::export_image_scope`). A process with thousands of message types writes a log or an audit file
whose dictionary is the dozen calls it actually contains; its writer-info item `dictionary` says
`scoped:<Tag's name>`. The scope costs nothing per message.

For **streams** (pipes, sockets, rings) the dictionary travels as dictionary frames:

```cpp
dictionary_tracker dt;   // remembers the registry version at the last dictionary frame
dt.update(fd);           // call from your idle path: the full dictionary first, then a delta when the registry
                         // changed (one relaxed load otherwise)
write_dictionary_frame(fd);      // or do it explicitly: the full image …
write_dictionary_frame(fd, v);   // … or the delta since registry version v
```

## 3.5 `print` / `println` — binary structured logging

```cpp
print  (out, "hello {} {} {}"_, "new", 100, "worlds"_);   // 32-byte frame; nothing formatted here
println(out, "px={:.2f} qty={:>4} side={}"_, px, qty, side);
println<"order {}">(out, order);                          // template-argument spelling
print_with(opts, out, "…"_, …);
```

- The format string is a literal (0 bytes on the wire); the arguments are ordinary records; the reader
  (`tools::render_frame`, `srdump`) runs the formatting. The call string is `print(n){…}` / `println(n){…}`
  and the call ENTRY carries flags `print` / `newline`, so `println` costs nothing per frame.
- **Compile-time check by the standard library**: `std::format_string<render_t<Ts>...>{F}` is constructed in
  a constant expression — a wrong placeholder count, `{:.2f}` on an `int`, an unbalanced brace, all fail to
  compile at the call site with libstdc++'s own diagnostic. `render_t`: scalars as themselves (so numeric
  specs apply), literals/strings/enums/structs as `std::string_view` (enums render as the enumerator name,
  structs as their one-line text form, dynamic types likewise). `std::byte`, `char8_t/16/32`, `wchar_t` cannot be
  printed (they can be serialized).
- Typed readers: `deserialize_print(bytes, "fmt"_, outs…)`, `deserialize_println(…)`.
- Constants work as arguments: `print(out, "n={} side={}"_, 42_, k<Side::buy>)` is a 16-byte
  frame that renders as `n=42 side=buy`.

## 3.6 Determinism and options

`scrub_padding` (default on) zeroes every padding byte and normalises `bool` bytes to 0/1 so equal values give
equal bytes — the property `--diff` and any content hash rely on. Turning it off saves the memset for types
without padding (the gap table is empty then anyway). `full_records` writes a 16-byte header per record
(schema hash, length) so a reader without the call table can still cut the frame; it is the archival mode.
`canonical_unordered` (default on) sorts an unordered container — `std::unordered_map`/`_set`/`_multimap`, or a
map with a comparator other than `std::less` — by key on the wire, so identical values give identical bytes
whatever the bucket count or insertion history (§8.1b). `serialize_with({.canonical_unordered = false}, out, v)`
writes such a container in its iteration order instead: no sort, no allocation above 64 entries; the same value
and the same length, possibly different bytes for the same value. Every reader accepts both forms (a reader's own
container decides its order), and hashes, `frame_size` and the dictionary are unaffected; an ordered container
is written the same way either way.

## 3.7 Errors and `status`

`error` is only ever an error (`sink_full, truncated, bad_magic, bad_version, endianness_mismatch,
hash_mismatch, length_mismatch, unknown_type, unknown_literal, unknown_call, corrupt_dictionary, misaligned,
schema_conflict, io_error, not_found, unsupported, invalid_argument, abi_mismatch, invalid_value, crc_mismatch`; `to_string(e)` names it).
`status = std::expected<void, error>` is "ok or an error": what a sink's sticky `state()` reports and what
`capture_file::close()` returns. The writer functions return `result<std::size_t>` =
`std::expected<std::size_t, error>`. The typed readers return a `decode_error`: the code plus how far
the reader got (chapter 4 §4.1a).

## 3.8 `to_string`, `to_json` and `std::format` for any value

```cpp
#include <es/serreg/to_string.hpp>            // links es::serreg_tools
struct P { std::int32_t a; std::uint8_t b; double c; };   // tests/unit/test_to_string.cpp uses such a type
std::string s = es::serreg::to_string(P{7, 2, 1.5});   // "P{a: 7, b: 2, c: 1.5}" (the type's canonical name first)
std::string j = es::serreg::to_json(P{7, 2, 1.5});     // {"a":7,"b":2,"c":1.5} — srdump's JSON value
auto r = es::serreg::render(order, tools::format::json);   // result<std::string>: the error instead of "<error: …>"
template<> struct es::serreg::formattable<P> : std::true_type {};
std::println("{} {:j}", p, p);                 // through to_string / to_json
```

Any value the library can serialise — a struct, an enum (by enumerator name), a scalar, a string (quoted), a
dynamic type, a container — renders exactly as `srdump` renders it, from the process's own dictionary: the call
registers the type (the same `touch` a `serialize` performs, so the type is in the registry before `main()` even if
it is never serialised), encodes the value as a frame would carry it and hands it to the tools' renderer
(`tools::render_value` over `tools::process_dictionary()`, the registry image parsed once per registry version).
It allocates and takes a shared lock: a debugging and logging convenience, never for the hot path — serialise the
value and let a reader render it. The `std::formatter` is opt-in (`formattable<T>`) so it never hijacks a type that
already has one. Details: `files/include-es-serreg-to_string.hpp.md`.

## 3.9 The logger (format 1.7)

`println` (§3.5) is binary logging without an envelope. `log.hpp` adds the envelope — level, source
location, time, optionally thread and sequence — at the cost of 8 bytes per message:

```cpp
struct app_log {};                                         // a dictionary scope tag
using log_file = es::serreg::scoped_capture_file<app_log>; // its file holds only this scope's calls
log_file f{"app.log.ser", {.info_thread = true}};
es::serreg::logger<{}, log_file> lg{f, es::serreg::level::info};   // runtime minimum: info

ES_SERREG_LOG(lg, warn, "book {} depth {}"_, sym, book.depth());   // __FILE__/__LINE__ as zero-byte constants
if (lg.enabled(level::debug)) lg.debug("px={:.2f}"_, px);          // no call site; gate expensive arguments
lg.set_level(level::trace);                                        // one relaxed store
```

- **The envelope is part of the call signature.** A log frame is a print frame of the call
  `log(n){const u8 level, lit file, const u32 line, const u8 mask, u64 time, [u32 thread], [u64 sequence], lit
  format, args…}` (call flags `print | newline | log`). Level, file, line and mask are constants (format 1.5), so
  they cost nothing per frame; the timestamp is 8 bytes, a per-message thread id 4 and a sequence 8. A reader learns
  everything else from the dictionary, once.
- **Policies are compile time.** `log_options` is a structural type passed as the first template argument:
  `floor` (levels below it compile to nothing — no code, no registration; `ES_SERREG_LOG_MIN_LEVEL` sets a
  translation unit's floor), `thread` (`none`, `once` — the default: the id goes into the file's writer-info frame
  when the capture file is opened with `info_thread` — or `per_message`), `sequence` (`none`, `per_sink`), `clock`
  (`system` ns since the epoch, `steady`, `tsc`, `user` via `set_clock`), `dictionary` (`scoped` — the default,
  the sink must be a `scoped_sink` — or `full`, any sink). `logger<{.thread = thread_policy::per_message,
  .sequence = sequence_policy::per_sink}, S>` is a logger for a sink shared by threads.
- **The gate.** `ES_SERREG_LOG(lg, lvl, fmt, args...)` expands to `lg.enabled(lvl) && lg.write<lvl, __FILE__,
  __LINE__>(fmt, args...)`: nothing after the level is evaluated when it is off, and `enabled` is one relaxed load
  (false at compile time below the floor). The member functions `trace` … `fatal` write file `"-"`, line 0, and
  evaluate their arguments.
- **Format checking** is `println`'s: the standard library checks the format against the arguments at
  compile time; nested replacement fields (`{:{}}`) are refused because the readers do not render them.
- **Errors** do not interrupt the caller: `write` returns `bool`, `failed()` counts failures and `last_error()`
  keeps the last code.
- **Reading.** `srdump app.log.ser --level warn` prints one line per log frame at or above `warn`:
  `2026-10-07 14:03:11.123456789 WARN  main.cpp:42 book AAPL depth 7` (UTC for the system clock; raw ticks for
  the others); `--since`/`--until` select by time; `--json` gives `{"frame":N,"log":{…}}` objects;
  `frame_router::on_log(h)` receives `h(level, line)` for every log frame and `on_log(level::warn, h)` the
  frames at or above a level as a `tools::log_record` (envelope fields + message; chapter 4 §4.6d); Wireshark and
  `python3 -m serreg --level` render the same lines.

Details: `files/include-es-serreg-log.hpp.md`; tests: `tests/unit/test_log.cpp`.
