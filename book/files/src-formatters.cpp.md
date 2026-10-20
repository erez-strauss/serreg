# `src/formatters.cpp`

Everything that turns decoded values, dictionaries and print frames into text or JSON (spec §6.9; FR-D1/D2/D8,
FR-L3, FR-W8, FR-P4/P5). It implements the two `decoded_value_visitor`s (`text_visitor`, `json_visitor`), the
public `tools::dump()` (the streaming `write_fn` overload and the `std::string&` convenience, each over a span
and over a path read in windows, A6),
`tools::dump_dictionary()`, `tools::render_frame()`, `tools::render_log()`, `tools::value_text()`,
`tools::render_value()` and `tools::process_dictionary()` (what `to_string.hpp` calls) and the log helpers
`internal::decode_log()`, `level_name()`, `time_text()`, and the shared `internal::render_message()` declared in
`src/internal.hpp`. TOML output of *records* does not exist: under `format::toml` the dictionary is TOML and
records are JSON lines (`formatters.cpp:3`). The file never reads a frame byte itself — cutting is
`for_each_record` in `reader.cpp`, decoding is `visit_record` in `decoder.cpp` — and nothing in it throws past
its boundary: the one place `std::vformat` can throw is wrapped and converted to `error::invalid_argument`.

## Depends on / used by

Includes: `internal.hpp` (`record_info`, `frame_info`, `for_each_record`, `for_each_record_in_file`,
`record_source`, `add_problem`, `decode_problem`, `dec`, `hex64`,
`hex8`, `append_quoted`), `es/serreg/deserialize.hpp` (`read_frame_header` for `render_frame`),
`es/serreg/tools.hpp`, `<cmath>` (`isnan`/`isinf`), `<format>` (`std::vformat`, `std::make_format_args`).

Used by: `src/cli.cpp` (`dump`, `dump_dictionary`), visitors that call `tools::value_text`
(`examples/user_example1/tree_printer.hpp`), embedders that call `tools::render_frame` (the typed
`deserialize_print` in `deserialize.hpp` does not go through this file), `tests/unit/test_tools.cpp`,
`test_print.cpp` (FR-P4/P5), `test_dynamic.cpp` ("2.0 tools:
dump text/JSON of dynamic values and print with a dynamic argument"), `tests/golden/make_fixtures.cpp` and
`make_dynamic_fixtures.cpp` (they produce `orders.{text,json,toml}` / `dynamic.{text,json,toml}`),
`fuzz_capture`.

## Structure

- **Scalar rendering** (`formatters.cpp:22-153`): `load<T>` (memcpy), `float_text` (`to_chars`, `null` for
  NaN/Inf), `raw_hex`, `json_int64` + `fits_double_exactly` (gk 1.12: 64-bit integers are quoted in JSON),
  `scalar_number` (shared text/JSON number form; a size mismatch renders as `"<hex bytes>"`, except a 2-byte
  `wchar` from a Windows writer), `quote_chars`
  (text quoting: `\0`, `\n`, `\xNN`), `is_char_kind`, `scalar_text` (top-level scalar records).
- **`text_visitor`** (`formatters.cpp:155-407`): the human-readable form with a `ctx` stack; 1.6: the map, set
  and transparent events.
- **`json_visitor`** (`formatters.cpp:409-559`): compact JSON with the same stack shape plus `begin_variant`/
  `end_variant`; 1.6: maps and sets through the visitor defaults, transparent records as the bare value.
- **Dictionary text** (`formatters.cpp:562-774`): `kind_text` (recursive member-kind spelling, `map<K,V>` /
  `set<K>` for 1.6), `dict_text` (the `transparent` and `unfixed` markers of 1.6; a log call is
  listed as `log …`), `hex_lines`.
- **Print rendering** (`formatters.cpp:776-920`): `enum_value`, `struct_inline_text`, `arg_as_text`,
  `render_arg`, `render_problem`.
- **Log helpers** (`formatters.cpp:923-988`): `internal::decode_log` (the envelope at `log_layout`'s
  positions), `level_name`, `time_text` (`YYYY-MM-DD HH:MM:SS.nnnnnnnnn` for the system clock, the raw number for
  any other clock, by the mask's bits 2–3).
- **`internal::render_message`** (`formatters.cpp:990-1101`): the placeholder scanner; a log frame's
  envelope first (`<time> <LEVEL> <file basename>:<line> [tid] #seq `, the level padded to five), unless the caller
  passes `envelope = false` (JSON writes the envelope as fields).
- **`dump_source`** (`formatters.cpp:1206-1556`): the whole dump over an `internal::record_source` — a
  function that runs the record walk over a span or over a file — so the span and the path overloads share one
  body.
- **Public functions** (`formatters.cpp:1105-1710`): `render_frame` (`formatters.cpp:1108-1133`), `render_log`
  (`formatters.cpp:1135-1181`), `dump_dictionary` (`formatters.cpp:1183-1198`), `dump` over a span
  (streaming and string, `formatters.cpp:1558-1578`), `dump` over a path (A6: streaming and string,
  `formatters.cpp:1580-1605`), `value_text` (`formatters.cpp:1607-1611`), `render_value`
  (`formatters.cpp:1613-1672`) and `process_dictionary` (`formatters.cpp:1690-1708`).

## How it works

### `text_visitor`: block vs inline structs

```cpp
struct ctx
{
    bool is_array{}, block{};
    bool is_map{}, is_set{}, is_entry{}; // 1.6
    std::string name, buf, indent, close, chars, key, value;
    std::vector<std::string> items;
    bool all_chars{true};
};
```

(`formatters.cpp:152-159`.) A top-level struct and a struct field directly inside a block struct print in
**block** form — `Type {` … one `name: value` per line … `}` — using `field_indent`/`close_indent` passed by
`dump` (`"        "` / `"      "`). Anything inside an array, or inside an inline struct, is **inline**:
`Type{a: 1, b: 2}` accumulated in `ctx::buf` and handed to the parent by `emit()` on `end_struct()`. `emit()`
(`formatters.cpp:172-204`) is the one output path: no stack → a line; entry parent (1.6) → stored as the entry's
`key` or `value`; array parent → push to `items`; block
parent → indented `name: value` line; inline parent → append `, name: value` to `buf` (the `back() != '{'`
test guards the first field).

### The all-chars heuristic and array elision

```cpp
void end_array() override
{
    ctx c = std::move(stack.back());
    stack.pop_back();
    if (c.all_chars && !c.items.empty())
    {
        emit(c.name, quote_chars(c.chars));
        return;
    }
    std::string s = "[";
    const std::size_t n = full ? c.items.size() : std::min<std::size_t>(c.items.size(), limit);
    ...
    if (n < c.items.size())
        s += ", … (+" + dec(c.items.size() - n) + " more)";
```

(`formatters.cpp:262-282`.) Every element event inside an array either appends to `chars` (a 1-byte
`char_`/`char8` scalar) or clears `all_chars` (`not_chars()` from `begin_struct`/`begin_array`, and from
`enumerator`, `literal`, `string`, `null_value`, non-char scalars). An array that stayed all-chars prints as one
quoted string — `char name[16]` reads as `"ACME\0\0…"`. Otherwise the items are joined, and past
`dump_options::array_limit` (16) the rest is elided with `… (+N more)` unless `--full`. `json_visitor` applies
the same heuristic (a char array becomes a JSON string) but never elides.

`scalar()` (`formatters.cpp:283-305`) renders 1-byte chars quoted, `byte_` as `0xNN`, everything else through
`scalar_number`. `enumerator()` gives `Side::buy (1)` or `Side(7)` for an unknown value; `literal()` gives the
quoted text or `lit#<16 hex>` when unknown. The v2 events `string()` and `null_value()` are quoted text and
`null`; `begin_variant` falls back to the base class default, so a variant shows as a struct named
`variant<i:type>` with a `value` field.

### Maps, sets and transparent records in text (1.6)

```cpp
void begin_entry(std::uint32_t) override
{
    ctx c;
    c.is_entry = true;
    stack.push_back(std::move(c));
}
void end_entry() override
{
    ctx c = std::move(stack.back());
    stack.pop_back();
    emit({}, c.key + ": " + c.value);
}
```
(`formatters.cpp:351-362`.) A map opens an array-like context flagged `is_map`; each entry collects its `"key"`
and `"value"` events into one `key: value` item; `end_map` and `end_set` share `end_braced`
(`formatters.cpp:379-394`), which is `end_array` with braces and the same `array_limit` elision: `by_name: {"a":
1, "b": 2, "c": 3}`, `ids: {10, 20, 30}`, `names: {-1: "minus one", 4: "four", 5: "five"}`, and a nested value
inline, `msgs: {1: corpus::DynMsg{id: 1, name: "one", xs: [1, 1]}, …}` (the golden `dynamic.text`, frame 4).
Keys are rendered by the same `scalar`/`string` events as any value, so a string key is quoted and an integer
key is not. `begin_transparent`/`end_transparent` are empty in both visitors: the container's own events then
land where the struct's would have, so a top-level `std::vector<i32>` record prints as `[1] [3, 1, 2]` and an
empty one as `[1] []`, and a print argument renders as `ids=[3, 1, 2]`.

### `json_visitor`

Same stack, one `buf` per context, commas by `n++`. Object keys go through `append_quoted`. `scalar()` uses
`scalar_number(k, bytes, true)`: i64/u64 quoted as decimal strings (gk 1.12), chars and bytes as numbers.
`enumerator()` emits `{"name":"buy","value":1}` and quotes the value only when it does not fit a double
exactly. `literal()` emits `{"lit":"text","id":"0x…"}` (`"lit":null` when unknown). `begin_variant()`
(`formatters.cpp:526-539`) opens `{"alt":i,"type":"…",` — the type text is the part of `variant<i:type>` after
the colon — and sets `n = 1` so the following `emit("value", …)` gets its comma. Maps and sets (1.6) need no
override: the visitor defaults turn a map into a vector of `entry` structs and a set into a vector, which the
JSON visitor renders as `[{"key":"a","value":1},{"key":"b","value":2}]` and `[10,20,30]` — a map's key need not
be a string, so a JSON object was not an option. `begin_transparent`/`end_transparent` are empty, so a
transparent record is its value: `{"frame":5,"record":1,"type":"std::vector<i32>","value":[3,1,2]}`.

### `dict_text`

`kind_text()` (`formatters.cpp:557-584`) spells a member's kind recursively: `array<i32,4>`, `vector<struct<Order>>`,
`optional<…>`, `variant<i32|struct<Order>|string>`, `map<string,i32>` / `set<u16>` /
`map<u8,struct<corpus::DynMsg>>` (the shape, whatever the container was), `struct<Name>`/`enum<Name>` (or the
hash when the nested type is missing). `dict_text()` prints the header line (`dictionary  program_hash 0x…  (T types, L literals,
C calls)`) and — only when the dictionary's writer ABI differs from this host's, or two sources disagreed —
an `abi …  (differs from this host: …)` / `(MIXED: …)` line (FR-W4), then each type as `struct Name  schema 0x…  layout 0x…  size N  align A  flat|dynamic [transparent] [since S]
[deprecated D]` or `enum Name : kind  schema … [unfixed]` — `transparent` (1.6) marks a container argument's
type, `unfixed` an enum without a fixed underlying type (`formatters.cpp:651-653`) — with its fields `  name: kind @off [size,align]` or its enumerators `  name = value`, (for a flat, non-transparent struct the field list carries `pahole`-style accounting, `formatters.cpp:660-720`: a
`/* N bytes hole */` line where the next member does not start where the previous one ends, a `/* tail: N bytes
padding */` line, and a closing `/* size S: payload P (p%), padding Q (q%) = holes n/B, tail T, inside nested
members M */` summary — `payload_bytes()` (`formatters.cpp:593-618`) counts leaf bytes through nested flat structs
and arrays, so Q is exactly what `serialize` scrubs; a `layout` totals line follows the last type), each
literal as `literal 0x… "text"`, each call as `call|print|println 0x…  (lit, i32, string, Order)`, and each
dictionary conflict with both definitions (cc F15/F16). A constant argument (format 1.5) is listed as
`const <type>=<value>` (`formatters.cpp:728-736`): the scalar kind name, or the enum's name from the dictionary (its
schema hash when the enum is missing), then the `u64` value of the call's entry as 16 hex digits. The golden
`orders.text` has
`call 0x0ef1cf0a2766bb0e  (lit, const i32=0x000000000000002a, corpus::Order, const corpus::Side=0x0000000000000002, const f64=0xbff8000000000000)`.
The value is the stored bytes, not a decoded number; the decoded value appears in each frame's records
(`[1] i32 = 42`), where a constant is rendered like any scalar or enum record. The golden `orders.text`/`dynamic.text` fixtures
freeze this exactly.

### `dump`: two passes, streaming through `write_fn`

```cpp
std::string head;
const auto pre = src(false, nop_frame, nop_record);
if (!pre)
    return std::unexpected(pre.error());
if (const auto r = dump_dictionary(pre->dict, opt.fmt, head); !r)
    return r;
if (!out(head, ctx))
    return std::unexpected(error::io_error);
if (opt.dict_only)
    return pre->dict.conflicts.empty()
               ? result<void>{}
               : std::unexpected(error::schema_conflict); // cc F18: no record walk
```

(`formatters.cpp:1215-1265`.) The dictionary printed at the top must be "as of the end of the capture" because
dictionary frames may appear anywhere, so pass 1 walks with no-op callbacks only to collect it — or, under
`dump_options::verbose` (2.2.6), with `note_block`, which records every dictionary block (the opening block, each
dictionary frame: index, offset, and the growth of the dictionary since the previous callback) for the
`dictionary blocks` table after the dictionary and the marker line `block_line` prints where the frame sits in
pass 2; over a file, `block_options::meta_frames` is set so the windowed walker delivers the meta frames. Pass 2
(`formatters.cpp:1522`) walks with `stop_on_problem = true`. `src` is the `record_source`:
`for_each_record` over the span for the span overloads, `for_each_record_in_file(path, …, block_options)` for
the path overloads — so over a file both passes read it in windows and the dump's memory is the window plus
the dictionary, whatever the capture's size (A6; `files/src-reader.cpp.md`). `on_frame` first finishes any pending print
frame, flushes the `records` buffer through `out` (one chunk per frame — cc F19), returns `false` once past
`last_frame` (nothing after the range is walked), skips out-of-range frames, prints a dictionary-frame line,
and otherwise builds the frame header line `frame N  call|print|println 0x…  (R records[, full])`. Under a
`--type` filter the header is held in `pending_header` and only written when a matching record appears.

`on_record` (`formatters.cpp:1427-1492`) applies the filter, then renders one record: JSON as one line
`{"frame":N,"record":M,"type":…,"value":…}` (scalar kind name + number, `"string"`, `"literal"` +
`json_visitor::literal`, `null` + `"schema"` for an unknown type, or the type name + `visit_record`), text as
`  [M] ` followed by `i32 = 7`, `string = "…"`, the literal, `<unknown type 0x…> (N bytes)`, or the
`text_visitor` block. `--hex` (and always for an unknown type) appends `hex_lines` / `"hex"`. A `visit_record`
failure is written inline (`<decode error: …>` / `"error":"…"`) and pushed to `decode_problems`; the walk
continues. After the walk: `finish_print`, final `flush`, then the `problem:` tail (or `{"problem":…}` lines) —
at most `decode_limits::problems` lines, the walker's structural problems first, then the decode
problems through `internal::add_problem`, and one `problem: ... and N more` line (JSON:
`{"problems_omitted":N}`) for the rest (`formatters.cpp:1528-1547`) —
and the return value is `first_error` if any, else the code of the first record (review G2-10)
that failed to decode or render (`unknown_type`, `corrupt_dictionary`, `unsupported`, `length_mismatch`, …), else
ok.
The string overloads wrap `std::string::append` as the `write_fn`; the path overloads take a `block_options`
(`--window`, per-call limits) and read the problem cap from it.

### Print frames in `dump`, and `--raw`

```cpp
const dyn::call* const k = d.find_call(f.call_hash);
// ... 2.2.1: --level keeps only log frames at or above the level
if (k && k->is_print() && !filtered)
{
    print_call = k;
    print_frame = f;
}
```

(`formatters.cpp:1394-1407`.) When the frame's call has `call_flag_print`, records are collected into
`print_recs` and, in text mode without `--raw`, not printed individually (`formatters.cpp:1380-1396`). Each
collected record's payload is copied into `print_bytes` and the `record_info` repointed at the copy:
over a windowed source a record's bytes are valid only during the callback, and the frame is rendered when the
next one starts (print arguments are scalars, strings and small values). When the
next frame starts or the walk ends, `finish_print` renders the message with `render_message`: text prints
`  print: <message>` (trailing newline of a `println` removed), JSON adds a line
`{"frame":N,"message":"…"}` *in addition to* the records. A render failure becomes a problem via
`render_problem` (`frame N @off: print: <error>`). `--raw` keeps the records visible in text; JSON always has
both.

Meta frames (dictionary, checkpoint, writer info; 1.7) are skipped before this point: they count in the frame
numbering and are never listed (`formatters.cpp:1379-1382`).

**Log frames.** A call with `call_flag_log` is a print call whose leading constants are the log envelope.
`finish_print` (`formatters.cpp:1286-1355`) decodes the envelope with `internal::decode_log` first: a frame whose
time is outside `--since`/`--until` (`dump_options::since`/`until`) leaves no trace — not even its header, which
waits in `pending_header`. With `--level` (`dump_options::logs_only`, `min_level`) every non-log frame and every log
frame below the level is skipped by the frame callback, and text output is the bare rendered line
(`<time> <LEVEL> <file>:<line> [tid] #seq message`); without it a log frame prints its header (`frame N  log …`)
and `  log: <line>`. JSON writes one object per log frame:
`{"frame":N,"log":{"level":…,"time":…,"file":…,"line":…,["thread":…,]["sequence":…,]"message":…}}`; a log
frame's records are never listed individually.

### `render_arg`: one placeholder, run-time chosen argument type

```cpp
const std::string fs = spec.empty() ? std::string("{}") : "{:" + std::string(spec) + "}";
try
{
    if (r.ak == fmt::arg_scalar)
    {
        ...
        case kind::i64: {
            const long long v = ... load<std::int64_t>(b);
            return std::vformat(fs, std::make_format_args(v));
        }
```

(`formatters.cpp:846-901`.) The writer's compile-time check (`std::format_string<render_t<Ts>...>`, FR-P2)
validated the spec against the argument's C++ type; the reader does not have that type, only the record's
category and kind. So each placeholder is formatted alone with `std::vformat` and *one* argument whose type is
widened by category: signed integers as `long long`, `f32` as `float` and `f64` as `double`, `boolean` as `bool`, `char_` as
`char`, every other scalar as `unsigned long long`, and literal/string/enum/struct/unknown as
`std::string_view` from `arg_as_text()` — the literal's text, the string bytes, the enumerator name (or its
number), or the struct's one-line form from `struct_inline_text()`, which runs `text_visitor` seeded with a
pseudo array context so the record becomes an inline item. Integer specs are width-independent, so a spec
accepted at compile time is valid for the widened type. `std::vformat` reports a bad spec by throwing; the
`catch (...)` turns that into `error::invalid_argument` so the library's no-exceptions boundary holds.

An `f32` is formatted as a `float` (T-04):

```cpp
case kind::f32: {
    const float v = load<float>(b);
    return std::vformat(fs, std::make_format_args(v));
}
```
(`formatters.cpp:874-877`.) Widened to `double`, `"{}"` of `0.1f` would print `0.10000000149011612`, the
shortest form of the widened value, where the writer's `std::format` prints `0.1`. A spec with an explicit
precision gives the same text either way; the shortest forms (`{}`, `{:e}` without a precision) and `{:a}` do not. `print_capture`
has a float line (`0.1f`, `3.14f`) that the srdump and Wireshark checks compare.

### `render_message`: escaped braces, positional indices

`render_message()` (`formatters.cpp:990-1101`) requires a print call, `records.size() == k.args.size()` and a
literal at `records[0]`; its text must be in the dictionary (`unknown_literal`). It then scans the format text:
`}}` → `}`, `{{` → `{`, a lone `}` → `invalid_argument`; a placeholder is `{`, optional digits (the index,
capped at 0xffff), optional `:spec` up to the closing `}` (a nested `{` in the spec is rejected — nested
width/precision fields are not supported), then `}`. Mixing automatic and manual indexing is
`invalid_argument` as in `std::format`; an index at or beyond the argument count is `length_mismatch`. Each
placeholder is rendered by `render_arg(d, args[idx], spec)`, and `'\n'` is appended for `println`.

### `render_frame`

`render_frame()` (`formatters.cpp:1108-1133`) is the public primitive over one frame: `read_frame_header`,
refuse a dictionary frame, run `for_each_record` over exactly `frame_len` bytes with `stop_on_problem` and the
caller's dictionary as `extra`, fail on the first problem, look up the call, require `is_print()`, and hand the
collected records to `render_message`.

### `render_log`

`render_log()` (`formatters.cpp:1135-1181`) is the same walk for a log frame, returning a `tools::log_record`
instead of one line: after the same header, record and call checks it requires `is_log()`, takes the envelope
apart with `internal::decode_log` (level, file, line, mask, time, thread and sequence with their presence flags,
and the index of the format literal among the records), renders the message with `render_message(…, false)` —
the format literal with the arguments, *without* the envelope prefix — drops a trailing `'\n'`, and copies the
format literal's text from the dictionary into `format`. It is what `frame_router::on_log(min_level, h)` hands
to a per-level handler; `render_frame` on the same frame gives the one-line form that `on_log(h)` and
`srdump --level` print.

### `value_text`

```cpp
std::string value_text(const kind k, const std::span<const std::byte> bytes)
{
    return scalar_text(k, bytes);
}
```

(`formatters.cpp:1607-1611`.) The public face of `scalar_text` (`formatters.cpp:139-148`): a `char`/`char8`
quoted with the text escapes, a `byte` as `0xNN`, everything else through `scalar_number` (decimal integers,
`true`/`false`, the shortest round-trip floating point). It is exactly what srdump's text dump prints for a
top-level scalar record and, through `text_visitor::scalar`, for a scalar member, so a user visitor that calls it
in its `scalar` callback matches srdump without a copy of the rendering (`examples/user_example1/tree_printer.hpp`
uses it).

### `render_value` and `process_dictionary`

`render_value(d, ak, k, schema, payload, format)` (`formatters.cpp:1613-1672`) renders one record payload as a
compact frame would carry it: a scalar through `scalar_text`/`scalar_number`, a string (`u32 len` checked against
the payload) quoted, a type record — found by schema hash, its length checked against the type's size or its
image prefix — through `arg_as_text` (text: the one-line form a print frame shows) or a `json_visitor` (JSON: the
dump's value). It backs `to_string`/`to_json` (`include/es/serreg/to_string.hpp`).

```cpp
std::shared_ptr<const dyn::dictionary> process_dictionary()
{
    process_dictionary_cache& c = the_process_dictionary();
    const std::uint64_t now = registry::instance().version();
    {
        std::shared_lock lock(c.mu);
        if (c.version == now && c.dict)
            return c.dict;
    }
```
(`formatters.cpp:1690-1698`.) The process's own dictionary, parsed from `registry::instance().export_image()`
once per registry version and kept in a function-local cache; readers take a shared lock, the first call after a
change re-parses under the exclusive lock. The parse is handed out as a `shared_ptr`, so a concurrent re-parse
(another thread calling it after a `dlopen`) never pulls it away from a caller: the old dictionary lives until its
last holder drops it.

### `dump_dictionary` and the verify report

`dump_dictionary()` picks `dict_text`, `to_json` or `to_toml` by `format`. The **verify report** rendering
(`ok: N frames, R records, B bytes` / `FAILED (P problems): …` and the JSON `{"ok":…,"frames":…,"problems":[…]}`)
is not in this file — it is the `--verify` branch of `cli::execute_command` (`cli.cpp:897-1162`); this file only produces `dump`'s
`problem:` tail.

## Maintaining it

- **Fixture tests freeze this output byte for byte:** `golden_fixture_{text,json,toml}_byte_exact`,
  `golden_fixture_dynamic_*`, `golden_srdump_{json,toml,text}_matches_fixture`
  (via `tests/golden/compare_srdump.cmake`). Any change to spacing, quoting, key order or the elision text
  requires regenerating `tests/fixtures/golden/*.{text,json,toml}` with `make_fixtures`/`make_dynamic_fixtures`
  and a `CHANGELOG.md` line. The `dynamic.*` fixtures hold the map, set and transparent renderings
  (frames 5–9).
- **Behavioural tests:** `test_tools.cpp` ("cc F19 tools::dump streaming overload delivers per-frame chunks and
  stops when asked", "gk 1.12 JSON renders i64/u64 as quoted decimal strings"), `test_print.cpp` ("FR-P4
  tools::render_frame formats every argument category", "FR-P5 srdump text renders print frames, --raw shows
  records, JSON adds message"), `test_dynamic.cpp` ("2.0 tools: dump text/JSON …"), `test_containers.cpp`
  (the text and JSON of map, set and transparent records and `--print` rendering), `fuzz_capture` (text
  with `hex`/`flatten` toggled by size, and JSON).
- **`value_text` is public API over `scalar_text`**: a change to `scalar_text` changes the text fixtures and what
  every visitor using `value_text` prints; keep the two one function.
- **Rendering is not the wire format**, but the *dictionary* renderings `to_json`/`to_toml` come from
  `dictionary.cpp` and are canonical (FR-W5); `dict_text` here is the human form.
- **Keep the visitors allocation-tolerant, the decoder allocation-free**: `text_visitor`/`json_visitor` build
  strings (tools layer); `visit_record` in `decoder.cpp` must not allocate — do not push work from the
  visitors back into the walker.
- **`std::vformat` is the only throwing call in the library's tools layer**; keep it inside `render_arg`'s
  `try`. Do not add other formatting through `std::format` outside that block.
- **Streaming contract:** `out` must be called at most once per frame plus once for the dictionary and once for
  the tail; a `false` from it must stop the walk with `io_error`. The `records` buffer is cleared on every
  flush; do not hold references into it across a frame — and nothing may hold a `record_info::payload`
  across a frame either, because over a windowed source the bytes belong to the reader's buffer; a print frame's
  records are copied for that reason. State kept across frames must stay O(types), never O(frames)
  (`test_limits.cpp` "2.1.16 A6 every window size gives the whole-file output, byte for byte" compares the three
  renderings over a file with the span output).
- **`--frames A..B` must never walk past B** (cc F18): `on_frame` returns `false` after `last_frame`.
- **Threading:** all state is local to one `dump` call; `write_fn` is invoked on the calling thread.
