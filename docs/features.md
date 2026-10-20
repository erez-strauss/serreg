# Features — what `es::serreg` does, why it is different, and where it stops

A tour for two readers: the **user** (writes `serialize(out, "evt"_, order)` and reads captures with `srdump`)
and the **developer** (wants to know what happens under that call and what it costs). Each feature has an
example that runs as written against the current release. The last section is the honest part: risks and
limitations of the library and its tools.

---

## 1. The unique advantage: the type describes itself, before `main()`, with nothing else to write

Other binary serializers need one of three things from you: an IDL and a generator (Protobuf, FlatBuffers, SBE,
Cap'n Proto), a macro or trait per type (cereal, Boost.Serialization, most "reflection" libraries), or a
contract that your struct is a plain aggregate you promise never to change (raw `memcpy`). `es::serreg` needs
none: **a plain C++ class, with private members, bases and standard containers, is serializable as it is**, and
the complete description of it — names, kinds, offsets, enumerators, nested types — is computed by **C++26
reflection** at compile time and **registered in a process-wide table during static initialisation**, as a side
effect of the first `serialize()` that names it.

```cpp
namespace ex {
enum class Side : std::uint8_t { buy = 1, sell = 2 };
struct Hdr   { std::uint32_t seq; std::uint16_t len; };
struct Order : Hdr {
    std::uint64_t id; std::int64_t px; Side side; char sym[8];
    [[=since(2)]] std::uint32_t venue;      // schema metadata, travels in the dictionary
    [[=skip]]     std::uint32_t cache;      // never serialised; its bytes are scrubbed to 0
};
}
capture_file file("orders.ser");                  // header + the program's dictionary, written now
serialize(file, "new order"_, order);             // 16-byte header + 48 bytes of Order. That is the whole API.
```

What that one line produced, with no generator, macro, registration call or schema file:

- a **schema hash** and a **layout hash** (XXH3-64 of canonical strings such as
  `dyn<ex::Order>[48,8]{<base:ex::Hdr>:…;id:u64@8[8,8];px:i64@16[8,8];side:enum<ex::Side,u8>@24[1,1];…}`), stable
  across compilers and standard libraries because the names are built by hand from reflection, never from a
  compiler's spelling;
- an **ENTRY** describing `Order`, `Hdr`, `Side`, the literal `"new order"` and the call signature, placed in the
  registry before `main()` and copied into the ELF section `es_serreg_dict` of the binary;
- a **dictionary** at the head of `orders.ser` that lets a reader who has *never seen your headers* decode every
  frame: `srdump orders.ser`, the Python reader, Wireshark.

**User view:** "I write a struct, I call `serialize`, the file explains itself." **Developer view:** the
description is a `consteval` computation (`reflection-services.md`) materialised into `.rodata`; registration is
an odr-use of a static guard (`registry-services.md` §2); the per-message cost is a `memcpy`.

## 2. Zero per-message cost for flat types; canonical images for the rest

A trivially copyable class is written as its native bytes after a 16-byte frame header; padding is scrubbed
(the gap table is known at compile time), `bool`s are normalised, `[[=skip]]` members zeroed. Measured on the
reference box: **2.5 ns** to serialize a 32-byte struct, **~2 ns** to deserialize it, ~60 ns for `inspect`/
`dispatch` of a frame (one registry lookup).

A class with `std::string`, `std::vector`, `std::optional`, `std::variant`, `std::map`/`set` (and the whole
vocabulary: `deque`, `list`, `forward_list`, `stack`, `pair`, `tuple`, `multimap`, …) is written as a
**canonical image**: fixed part at canonical offsets, variable parts out of line, unordered containers sorted —
so **identical values give identical bytes**, which makes captures diffable and deduplicable.

```cpp
struct Quote { std::uint32_t id; std::string sym; std::vector<std::int32_t> depth;
               std::optional<double> last; std::map<std::string, std::uint64_t> flags; };
serialize(out, "quote"_, Quote{1, "AAPL", {1,2,3}, 2.5, {{"halted", 0}}});   // same call, image encoding
```

**Developer view:** `flat_type<T>` vs `dynamic_type<T>` is decided at compile time; the three image walks
(measure, encode, decode) share one positional view of the class (the parts layer), so a new container kind is
one specialisation.

## 3. Literals and constants: identity in the type, zero bytes on the wire

```cpp
serialize(out, "order rejected"_, o);                        // the literal's 64-bit id is in the call hash
serialize(out, "fill"_, 42_, order, -7_, 1.5_, k<Side::sell>); // four constants: 0 bytes in the frame
println(out, "px={:.2f} qty={} side={}"_, px, qty, side);  // 32-byte frame, formatted by the reader
```

A string literal, a numeric constant (`42_`, `1.5_`, `k<V>`) or a format string is part of the **call
signature**, hashed once, stored once in the dictionary, and costs nothing per message. The reader names the same
constants and the frame is refused if they differ. `println` is **binary structured logging**: the
format string is checked at compile time by `std::format_string`, nothing is formatted on the hot path, and
`srdump` prints `px=2.50 qty=7 side=sell`.

## 4. Readers for every situation — and one that needs no types at all

| Style | Call | When |
|---|---|---|
| typed, copying | `deserialize(frame, "fill"_, o, qty)` | the receiver knows the message; `decode_error` says which item failed and why |
| typed, zero-copy | `view<Order>(record)` | aligned flat records, no copy at all |
| typed, by message | `dispatch<Order>(frame, handler)`, `frame_router` with `.on("new order"_, [](const Order&){…}).on_println("fill {} @ {:.2f}"_, …)` | a feed with many message kinds |
| dictionary-driven, typed where possible | `tools::walk_values<Order, Fill>(path, on_value, rest)` | a mixed capture: your types where you have them, generic values elsewhere |
| dictionary-driven, no types | `srdump`, `tools::visit_record`, the Python reader | tooling, support, other teams, other languages |

```cpp
frame_router r;
r.on("new order"_, [&](const ex::Order& o) { book.add(o); })
 .on_println("fill {} @ {:.2f}"_, [&](std::uint64_t id, double px) { fills.push_back({id, px}); })
 .on_log([](std::uint8_t lvl, std::string_view line) { std::println("[{}] {}", lvl, line); });
tools::for_each_frame("capture.ser", r.callback());    // mapped, or in 10 MiB windows for any size / a pipe
```

```sh
srdump capture.ser                 # dictionary, then every frame decoded — no headers, no build
srdump capture.ser --json | jq '.[] | select(.type=="ex::Order")'
srdump capture.ser --verify        # hashes, lengths, CRCs, frame bounds; exit 0 iff clean
srdump capture.ser --dict-only     # the schema alone, with pahole-style holes / tail padding / payload % per flat type
srdump capture.ser --dict-only --verbose   # … plus its blocks: opening block, every dictionary frame, what each added
srdump capture.ser --info          # header, dictionary chain, checkpoints, writer info; warns on a broken chain link
python3 -m serreg capture.ser --json   # the pure-Python reader, parity-tested against srdump
```

**Developer view:** every reader checks the frame header, the call hash and the record lengths against
`frame_len` first (G2-1); a **flat** record is then validated whole (lengths, `bool` bytes) before any output is
written, while a **dynamic** record is checked against the slot rules *as it is decoded*. So a failed
multi-argument `deserialize(bytes, a, b, c)` leaves the arguments before the failing one complete and the failing
one partially written — valid, unspecified values: the basic guarantee, and `decode_error{expected, actual,
failed}` says how far it got (R-8). The readers never allocate on the typed path except into `std::string`/dynamic
outputs, and they never crash on wrong data (§8).

## 5. The schema is everywhere you need it

- **In the file**: the opening dictionary, delta frames when a plugin adds types mid-run (forward-linked, never
  rewritten), checkpoints that carry a full copy for seeking and resynchronisation.
- **In the binary**: `srdump --elf ./your-app` prints the schema of any program or `.so` built with the library,
  without running it — `--rri-out`, `--json`, `--toml`, `--cxx` (C++ definitions back from the dictionary,
  self-checking), `--wireshark` (a Lua dissector for the capture's types).
- **In the process**: `registry::instance().export_image()`, `ES_SERREG_DUMP_REGISTRY=path` at exit,
  `ES_SERREG_DUMP_SIGNAL=USR1` on demand.
- **Across versions**: `srdump --diff old.rri new.rri` tells which types changed and how (`--direction` for the
  reader's or the writer's view).

```sh
srdump --elf ./trading-engine --program-hash      # the identity of this build's type set
srdump --elf ./trading-engine --cxx types.hpp     # regenerate the message headers for a consumer
srdump --elf ./trading-engine --wireshark feed.lua && tshark -X lua_script:feed.lua -r capture.pcap
```

## 6. Plugins, reload and dictionaries that follow the program

A `dlopen`ed library registers its types when it loads; the capture writer hears about it (`on_change`) and the
next frame is a dictionary delta. When the plugin unloads, its entries are **retained**: frames it wrote still
decode, lookups still work, exports still carry them. A reload revives them; a changed type is a new hash. Two
loaded definitions with the same schema but different layout are reported as a **conflict** (`warn`/`abort`),
never silently merged. `examples/dictionary_deltas/` shows the resulting file byte by byte.

**Dictionary scopes** keep files small: a sink type that declares `using dictionary_scope = log_scope;` gets a
dictionary of the forty calls it carries, not the program's four thousand archive types.

## 7. Binary logging with a compile-time gate

```cpp
struct log_scope {};
using log_file = scoped_capture_file<log_scope>;
constexpr log_options traced{.thread = thread_policy::per_message, .sequence = sequence_policy::per_sink};
logger<traced, log_file> lg{file, level::info};

ES_SERREG_LOG(lg, debug, "never built: {}"_, expensive());    // below the level: the argument is not evaluated
ES_SERREG_LOG(lg, warn,  "book {} depth {}"_, "AAPL", 7);     // one small frame; file:line and level as constants
lg.error("pos {}"_, Pos{17, 2.5});                             // any serialisable value as an argument
```

The envelope (level, file:line, thread id, sequence) is **constants of the call** — zero bytes per line. The reader
formats: `srdump log.ser --level warn --since 10:00`, `frame_router::on_log`, or `to_string`/`to_json`/
`formattable<T>` for any value in your own output.

## 8. Never crash on wrong data, and prove it

Every reader — typed, dictionary-driven, Python — is written to the same contract: a frame that does not fit, a
length that lies, a hash that is unknown, a `bool` byte of 7, a valueless variant, a 2 GiB length field: each
is a reported error, never a crash, hang or allocation storm. The guards are `decode_limits` (frame bytes, image
bytes, nesting depth 64, walk steps, input size; process-wide and per call) and the image byte budget. It is
checked by four fuzz targets (RRI, capture, ELF, config) under ASan/UBSan at 200 000 runs per release, the
sanitizer presets, and the `tests/fixtures/negative` corpus.

## 9. Integrity, crash safety and captures of any size (format 1.7, all opt-in)

`capture_options{.crc = true, .checkpoint_bytes = 64 << 20, .checkpoint_dictionary = true}`: a CRC32C trailer
per frame (a corrupt frame is skipped, not decoded), checkpoint frames with the latest dictionary for seeking
and `--resync` after truncation, a writer-info frame (`srdump --info`: versions, options, host), and a file
that is **never rewritten** — valid up to its last complete frame after a crash. Readers walk a file of any size
in 10 MiB windows (`for_each_frame_in_file`), or mapped when it fits. Defaults write exactly what the previous
format wrote: zero bytes per frame when off.

## 10. Schema evolution, level 1

A reader built from a newer or older `Order` reads the other side's frames by **member name**: widening
(`u32 → u64`, `f32 → f64`, `uN → wider iN`), defaults from `T{}` for new members (default member initialisers
honoured), writer-only members skipped, arrays that grew, nested flat classes by name, renames declared with
`[[=renamed_from("px")]]`. Narrowing and family changes are never guessed (R7: reported, default kept).

```cpp
struct Order { std::uint64_t id; [[=renamed_from("price")]] Px px; std::uint64_t qty /* was u32 */;
               std::uint32_t venue = 7; /* new */ };
tools::projection_report rep;
deserialize(projection_context{&dict, &rep}, frame, out);   // old frames project; rep.notes says what happened
```

The plan for a (writer type, reader type) pair is built once and cached; the per-frame path neither allocates
nor locks. Today for flat types; dynamic members, the router and `--diff --compat` follow (`roadmap.md`).

## 11. IPC on the same frames

`es::ipc` (header-only, C++23, no dependency on the serializer): variable-size SPSC queues and SPMC broadcast
rings in shared memory, with `es::ipc::sink` so that `serialize()` builds the frame **inside the ring record**
and the consumer `dispatch`es it from there — the reference half-RTT benchmarks (`scripts/bench-hrtt-*.sh`)
measure the serializer's cost as the difference between raw bytes and serreg frames over the same ring. Huge
pages and `mlock` are options; the ring validates its headers by the same never-crash rules.

## 12. Tooling that comes from the dictionary, not from your code

`srdump` (dump, verify, JSON/TOML, `--elf`, `--diff`, `--cxx`, `--wireshark`, `--info`, `--resync`, `--level/
--since/--until`), an embeddable CLI (`examples/embedded_cli`: your program *is* srdump for its own files), the
Python reader, the Wireshark dissector generator, `registry_measure` and the benchmarks, `tests/golden` that
freeze every hash and byte, and a book (`book/`) that is checked against the code on every run.

## 13. Two perspectives in one table

| | User | Developer |
|---|---|---|
| Getting started | include one header, call `serialize`; the first `srdump` of your file shows everything | `descriptor_v<T>`, `schema_string_v<T>`, `schema_hash_v<T>` are `constexpr`; `static_assert` them to freeze a format |
| Changing a type | add a member with `[[=since(N)]]`; old readers refuse by hash, level-1 readers project | the hash moves for any change of name/kind/offset; `srdump --diff` in CI tells you before a reader does |
| Debugging | `srdump --json`, `to_string(value)`, Wireshark | `decode_error{code, expected, actual, failed}`, `tools::verify`, the negative fixtures |
| Performance | nothing to tune on the hot path | `flat_type` ⇒ memcpy; dynamic ⇒ one measure + one encode pass; a flat record is validated before any write, a dynamic one as it is decoded (basic guarantee; `decode_error` says how far) |
| Deployment | one shared or static library + headers, Linux x86-64 | SONAME `major.minor`, `find_package(serreg 2.2)`, `pkg-config`, vcpkg port; `ES_SERREG_*` env hooks |

---

## 14. Risks and limitations

Stated plainly, because the features above only hold inside these lines.

### Language and toolchain

- **C++26 reflection is required** (`-std=c++26 -freflection`). Today that means GCC 16 and Bloomberg's
  clang-p2996 fork; upstream Clang and MSVC have not shipped P2996 yet. This is a timing fact about the
  ecosystem, not a property of the library: reflection is in the C++26 standard, every major compiler has
  announced or started it, and the library is written against the standard's spellings with a two-compiler
  cross-check, so it follows the compilers as they arrive. Until then, a codebase that cannot build with GCC 16
  cannot use it — and there is deliberately no reflection-free shim, because a shim would mean macros or a
  generator, which is exactly what the library exists to remove.
- The reflection papers are **young**: spellings (`template for`, `define_static_array`, `annotations_of`) have
  moved between drafts and may move again before the standard is final; the library tracks GCC 16 and keeps a
  Compiler Explorer cross-check, but a future compiler may need source changes (the per-construct inventory in
  `reflection-services.md` is the map for that).
- Compile time grows with the number of serialized types: every type instantiates its walks and tables. For
  programs with thousands of message types this is minutes, not seconds, on first build (`registry at scale`
  in the stories document has numbers and the mitigations: dictionary scopes, fewer TUs instantiating the same
  types).

### Platform

- **Linux, x86-64, little-endian, ELF64, glibc** — only. The native wire mode writes the writer's ABI (member
  sizes/offsets, `wchar_t` width, `char` signedness go into the ABI tag), so a file from another ABI is readable
  by the dictionary-driven tools but refused by typed readers. A portable wire mode and other platforms are 3.0
  work (`roadmap.md` §3, DECISIONS P-18, G-6).
- The embedded dictionary needs **binutils ≥ 2.36** (`[[gnu::retain]]`); the ELF section reader understands
  ELF64 little-endian only; `image=none` builds lose `srdump --elf`.
- Plugin semantics depend on glibc's loader (`dladdr`, `dl_iterate_phdr`, `DF_1_NODELETE`); `-fno-gnu-unique` is
  required for real unloads.

### Wire format and compatibility

- **Format minors are not forward-compatible**: a 1.7 reader refuses a 1.8 file by version, even when the
  change is additive, until 3.0 fixes the "additive only" rule (P-19). Within 2.x the *library minor* is the
  compatibility unit (SONAME `2.2`); 2.2.1 shipped a format change in a patch release — recorded as the one
  exception (P-20).
- **Hashes are the contract.** Any change to a member's name, kind, offset or an enumerator moves the schema
  hash; a renamed enumerator moves it too. There is no "ignore unknown fields" below evolution level 1, and
  level 1 today covers flat types only — dynamic members, the router and dictionary-driven projection come in
  2.3.0. There is no level 2 (structural changes, conversions) at all yet.
- The canonical-name rules are hand-built and frozen by golden tests; a type that **has no linkage-unique name**
  (unnamed namespace, local class) must be named explicitly or is rejected. Types outside the vocabulary
  (pointers, smart pointers, unions, polymorphic classes, bit-fields, virtual bases, `std::bitset`, `complex`,
  `expected`, `priority_queue`, `chrono`) are rejected with a diagnostic; `as_flat<T>` is the escape hatch and it
  is manual.
- Unordered containers are **sorted on write** by default (canonical bytes); `serialize_options{.canonical_unordered
  = false}` writes them in iteration order (no sort, no allocation) at the price of byte identity — readers take
  both. Floating-point keys are ordered by `std::strong_order` (IEEE totalOrder, so a NaN key is fine); a
  user-defined key type must provide a strict total `<` (or be trivially copyable: then its bytes are compared).
- The enum range check (values an unfixed enum cannot hold) is **off by default** (A-16): reading such a value
  through the enum type is undefined behaviour the caller owns unless `ES_SERREG_VALIDATE_READ=1` is set. There
  are no value constraints (`range`, `max_size`) yet; that is the validation feature of 2.3.0.

### Runtime and registry

- One registry per process, filled at **static initialisation**: a type that is only ever read (never passed to
  `serialize`) must be registered explicitly (`register_types<T>()`, or a whole namespace with
  `ES_SERREG_REGISTER_NAMESPACE(ns[, selector])`) or it is unknown to typed readers.
- Published indexes are **never freed** (about 80 bytes per entry per generation): a program that churns
  plugins thousands of times grows (A-13 is documented as a limit); retained entries of unloaded plugins stay
  until exit (`forget_retained()` is planned).
- The registry budgets (10k types / 10 ms, 100k / 100 ms) hold on the reference box; a program with 100k types
  pays ~100 MB for ENTRY copies and should use scopes and delta exports.
- Static-initialisation order is the usual C++ hazard: using the registry from another static initialiser works
  (the registry is constant-initialised and guards push into it), but a *reader* that runs before the writer's
  image is loaded sees nothing yet.
- A `capture_file` has **one writer thread**: it is not thread-safe, and two threads that serialize into one file
  need their own serialisation (or one file each, or an `es::ipc` ring in front of the writer). `serialize()`
  itself is reentrant; it is the sink that is not.
- `on_change` callbacks run when the next index is published, on the thread that publishes it, and they **must
  not register types or publish** (no `serialize()` of a new type, no `register_types`, no `snapshot()` /
  `refresh()`): a callback that does re-enters the publication it is being told about. A callback whose code
  lives in a plugin is removed (`remove_on_change`) before that plugin unloads.

### Tools

- `srdump` and the Python reader decode **only** what the dictionary describes; a file whose dictionary frame is
  lost (a stream captured from the middle, a truncated opening block) is unreadable without an `.rri` from the
  producer (`--dict`). Level-3 ("dictionary-less") reading is a declared non-goal.
- The Wireshark dissector covers the capture framing and the dictionary's types; it does not reassemble frames
  across TCP segments.
- `--cxx` regenerates *definitions*, not the original source: names are canonical, enums are all `enum class`,
  no comments, no methods.
- The Python reader is pure Python: correct and parity-tested, not fast (tens of MB/s). A C-API-backed binding is
  a roadmap item.

### Project

- One maintainer, AI-assisted development with recorded provenance; the API is **not frozen until 3.0** (names,
  the `es::` prefix may change for a submission target, and the MIT license may follow such a target's rule — DECISIONS G-7/G-8). A written wire
  specification independent of the headers does not exist yet (D-5); the Python reader is the second
  implementation that keeps the headers honest.
- Security model: **one trust domain**. Input is expected to be wrong at times, never hostile by design; the
  never-crash contract and the fuzzing are about robustness, not about resisting an adversary (no authentication,
  no encryption, CRC is not a MAC).
