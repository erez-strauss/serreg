# `include/es/serreg/tools.hpp`

The library services behind `srdump` (spec §6.9, FR-D0), and the CLI itself as a library
(`es::serreg::cli`). Everything the tool can do — load a dictionary from an RRI/ELF/file, verify a capture,
dump it as text/JSON/TOML, diff two dictionaries, render a print frame, walk one record through a visitor,
generate a Wireshark dissector, walk a capture on disk by path, generate C++ definitions,
verify or dump a capture of any size in windows — is a function here, implemented in `libserreg` (`src/*.cpp`); `cli::*`, `tools::diff`,
`tools::wireshark_dissector` and `tools::cxx_header` are implemented in the second library `libserreg_tools` (`es::serreg_tools`,
`src/cli.cpp`, `src/diff.cpp`, `src/wireshark.cpp`, `src/cxxgen.cpp`), which links `es::serreg` PUBLIC (decision G). This is the **tools layer**; it works on captures and
dictionaries from *any* program and has **no reflection dependency**. `tools/srdump/main.cpp` is one line:
`return es::serreg::cli::run(argc, argv);`.

## Depends on / used by

Includes: `<cstddef>`, `<cstdint>`, `<functional>` (`cli::app::on_extra`), `<span>`, `<string>`, `<string_view>`,
`<utility>`, `<vector>`, `es/serreg/dictionary.hpp`, `es/serreg/error.hpp`, `es/serreg/limits.hpp`
(`decode_limits` in `block_options`/`file_options`).

Included by: `src/cli.cpp`, `src/decoder.cpp`, `src/dictionary.cpp`, `src/diff.cpp`, `src/elf_reader.cpp`,
`src/file_io.cpp`, `src/formatters.cpp`, `src/reader.cpp`, `src/registry.cpp` (the env-dump hooks use `write_all`/`write_file`),
`src/wireshark.cpp`,
`include/es/serreg/walk.hpp`, `include/es/serreg/router.hpp` (`frame_ref`, `frame_callback`),
`tests/fuzz/fuzz_targets.cpp`, `tests/unit/runtime_selftest.cpp`, and via `serreg.hpp` the tests and examples
(`examples/embedded_cli.cpp` shows `cli::run` with an `app`).

## What it defines

### `namespace es::serreg::tools`

| name | kind | meaning |
|---|---|---|
| `format` | `enum class` | `text`, `json`, `toml` |
| `dump_options` | struct | `fmt, dict_only, hex, flatten, full_arrays, array_limit = 16, first_frame/last_frame, type_filter (schema hash), type_name_filter, raw` (show print frames as records); the log filters (1.7) `logs_only` (only log frames — `srdump --level`), `min_level` (0 trace … 5 fatal), `since`/`until` (the frame's timestamp in the writer's clock units, inclusive); `verbose` (2.2.6: the dictionary blocks after the dictionary and a marker line per dictionary frame — `srdump --verbose`) |
| `verify_report` | struct | `frames, records, bytes, problems, warnings` — `frames`/`bytes` count data frames only: dictionary, checkpoint and writer-info frames are consumed, not counted; `program_hash` (the capture header's, 0 for a bare stream) and `problems_omitted` (problems beyond `decode_limits::problems`, counted, not kept); `ok()` ⇔ no problems and none omitted (warnings do not count) |
| `diff_direction`, `to_string(diff_direction)` | `enum class : uint8_t`, `constexpr` function | decision E: `none`, `old_data`, `new_data`, `both` — whose reader a `--diff` verdict is for; the text forms `none`, `old-data`, `new-data`, `both` |
| `diff_entry`, `diff_report` | structs | `name, schema_a, schema_b, verdict`; `breaking` (this entry breaks the report's direction) and `note` (why); `entries, breaking`, `direction`, `breaking_removals`, `breaking_additions`, `breaking_changes`; `to_text()` |
| `library_version` | `using es::serreg::library_version` (defined in `version.hpp`) | printed by `srdump --version`, written into the writer-info frame |
| `load_dictionary_rri(span)` / `load_dictionary_file(path)` | functions | RRI bytes / `.rri` file → `dyn::dictionary` |
| `extract_embedded_dictionary(path)` / `extract_embedded_dictionary(span)` | functions | A4: the dictionary embedded in a program image, file or in memory (FR-D6). The format is recognised by its magic: ELF → section `es_serreg_dict`; any other format → `unsupported`; fewer than 4 bytes → `truncated`; an ELF image without the section (built with `ES_SERREG_IMAGE_FORMAT=none`, or stripped) → `not_found` |
| `extract_registry_from_elf(path)` | function | same as `extract_embedded_dictionary(path)`; the older name, kept for existing callers |
| `extract_section_from_elf(path, section)` / `extract_section_from_image(span, section)` | functions | raw section bytes, from a file or an in-memory image (fuzz target, embedders) |
| `value_text(kind, bytes)` | function → `std::string` | one scalar as srdump's text output shows it — integers and floating point in decimal (shortest round trip), `bool` as `true`/`false`, `char` as a quoted character, `byte` as `0xNN`; for `decoded_value_visitor::scalar` (implemented in `src/formatters.cpp`, it is the text formatter's `scalar_text`) |
| `wireshark_options` | struct | `std::string generator = "srdump"` (named in the header comment and in `SERREG.generator`), `std::vector<std::string> sources` (the dictionary sources, listed in the header comment) |
| `cxx_options` | struct | A5: `bool router = false` (also emit `install_handlers(frame_router&)`), `std::vector<std::pair<std::string, std::string>> namespaces` (`old` → `new` namespace prefixes; the name annotation keeps the canonical name), `std::string_view include = "<es/serreg/serreg.hpp>"` |
| `cxx_header(dict, options = {})` | function → `std::string` | A5: C++26 source declaring every type of `dict` so that each regenerated type has the recorded schema and layout hash — enums, structs in dependency order inside their namespaces, `std::array`/`std::string`/`std::vector`/`std::optional`/`std::variant` members, bases, `since`/`deprecated`, skipped padding members for a flat type's gaps, `[[=es::serreg::name("…")]]` for names a declaration cannot spell, the std vocabulary spelled back — with `static_assert`s of every size, offset, name and hash at the end, and the router skeleton with `router` (`src/cxxgen.cpp`; `srdump --cxx`) |
| `wireshark_dissector(dict, options = {})` | function → `std::string` | a Wireshark Lua dissector generated from `dict`: one filter field per value (`serreg.<type>.<member>`), the types, enumerators, call signatures and literals, and a fixed runtime that reads FILE captures, USER0 pcaps and TCP/UDP ports, walks records with the slot rules of `src/decoder.cpp`, and compares a capture's own dictionary with `dict` by hash (`src/wireshark.cpp`, `docs/wireshark.md`; `srdump --wireshark`) |
| `verify(capture, extra = nullptr)` / `verify(path, extra = nullptr, block_options = {})` | functions | structural check **and** a decode of every record through the descriptor walker; literal ids must equal the signature's; dictionary conflicts are problems. The path overload (A6) reads the file (or `"-"`) in windows — a capture of any size with bounded memory |
| `write_fn` | `bool (*)(string_view chunk, void* ctx)` | streaming sink for `dump` |
| `render_frame(frame, dict)` | function | 1.1: the formatted message of one print frame; for a log frame, the full line with the envelope prefix |
| `log_record` | struct | a log frame taken apart: `level`, `file` (the writer's `__FILE__`, or `"-"`), `line`, `mask`, `time_ns` (the raw timestamp of the writer's clock policy), `thread` / `sequence` with `has_thread` / `has_sequence` (present when the writer's mask asked for them), `format` (the format literal's text) and `message` (the rendered line *without* the envelope prefix and without a trailing newline) (`tools.hpp:266-278`) |
| `render_log(frame, dict)` | function → `result<log_record>` | the `log_record` of one log frame; a frame whose call is not a log call → `error::invalid_argument` (`src/formatters.cpp`); what `frame_router::on_log(min_level, h)` hands to its handlers |
| `dump(capture, extra, opt, write_fn, ctx)` / `dump(capture, extra, opt, std::string&)` | functions | streaming / buffered dump; a record that fails to decode is a problem and makes `dump` return that error *after* the walk |
| `dump(path, extra, opt, write_fn, ctx, block_options = {})` / `dump(path, extra, opt, std::string&, block_options = {})` | functions | A6: the same over a file (or `"-"`), read in windows; two passes over the file (the dictionary is printed first and is "as of the end") — so a pipe cannot be dumped this way (`srdump` slurps a pipe) |
| `dump_dictionary(dict, format, std::string&)` | function | dictionary only |
| `diff(a, b, direction = diff_direction::old_data)` | function | type/call/literal verdicts; which removals or additions are breaking follows `direction` |
| `decoded_value_visitor` | abstract class | §6.4 style 4: the descriptor-driven walk callbacks |
| `visit_record(payload, type, dict, visitor, field_name = {}, flatten = false)` | function | walk one record's payload |
| `value_location` | struct | `{bytes, image, image_offset, native}` — where a struct value sits (native object bytes of a flat record, or a position in a canonical image); `budget`, the walk's `fmt::image_budget`, which a typed decode of the value charges too |
| `decoded_value_visitor::noncanonical_bool(field, raw)` | virtual, default no-op | G2-12: a `bool` byte other than 0/1; the `scalar` event that follows carries the repaired byte 1 |
| `decoded_value_visitor::take_struct(field, type, where)` | virtual, default `false` | called before each struct value is walked; returning `true` takes the value over (no begin/fields/end events). `walk_values` uses it |
| `decoded_value_visitor::begin_map(field, count)` / `begin_entry(index)` / `end_entry()` / `end_map()`, `begin_set(field, count)` / `end_set()` | virtuals with defaults | format 1.6: a map is `begin_map`, then per entry `begin_entry` / the key as `"key"` / the value as `"value"` / `end_entry`, then `end_map`; a set is `begin_set`, its keys unnamed, `end_set`. Defaults: a vector of `"entry"` structs, a vector |
| `decoded_value_visitor::begin_transparent(type_name, field)` / `end_transparent()` | virtuals with defaults | a container argument's transparent type — the record is the container; the default walks it as a struct named after the container, the renderers show the value alone |
| `block_options` | struct | `std::size_t block_bytes` (0 = `decode_limits::window_bytes`, 10 MiB); `const decode_limits* limits = nullptr` (the process-wide limits unless set); `bool resync = false` — after a frame that cannot be parsed, scan forward 8 bytes at a time for the next checkpoint frame and continue there (one problem line for the skipped range) instead of stopping (`srdump --resync`); `bool meta_frames = false` — 2.2.6: hand the opening dictionary block and every dictionary / checkpoint / writer-info frame to the callback too, `frame_ref::kind` (`frame_kind::opening/dictionary/checkpoint/info`) saying which, `call` null; off, data frames only as before — the options of every windowed reader |
| `writer_info`, `checkpoint_info`, `capture_info` | structs | what a FILE says about itself: the writer-info items (key → text), one checkpoint (`offset, prev, next, data_frames_before, bytes_before, wall_clock_ns, dictionary_offset, registry_version, full_dictionary`), and the whole picture — `minor, flags, program_hash, abi, file_size, dict` (opening dictionary merged with every delta the chain reaches), `dictionary_offsets`, `checkpoints`, `writer`; `writer_item(key)`, `has_writer_info()` (`tools.hpp:213-243`); `capture_info::warnings` (2.2.6): a chain link that could not be followed — past the end of the file, not going forward, not a frame of the chain's kind — one line each, rendered as `warning …` / JSON `"warnings"` |
| `info(path)`, `to_text(capture_info)`, `to_json(capture_info)` | functions | reads the header, follows the dictionary chain and the checkpoint chain with seeks — O(links), never O(frames) — and reads the writer-info frame; a 1.6 file yields its (possibly back-patched) dictionary and no chains; a pipe is `unsupported` (`src/reader.cpp`; `srdump --info`) |
| `render_value(d, ak, k, schema, payload, format)` | function (DECISIONS A-10) | one value rendered from a dictionary: the one-line text form print uses for a struct argument (`Type{field: value, …}`) or the dump's JSON value; what `to_string`/`to_json` call (`src/formatters.cpp`) |
| `process_dictionary()` | function | the process's own dictionary — `registry::instance().export_image()` parsed, re-parsed when the registry version moved; lazily built, shared, under a `shared_mutex`; not for the hot path (`to_string.hpp`) |
| `frame_ref`, `frame_walk`, `frame_callback` | structs / alias | one data frame (`index, offset, bytes, call_hash, n_records, full_records, call`, `is_print()`; `index` counts every frame of the capture, meta frames included, so it matches `srdump`'s numbering); the walk summary (`dict, frames, typed, fallbacks, problems` — `typed`/`fallbacks` filled by `walk_values`; `problems_omitted`); `std::function<bool(const dyn::dictionary&, const frame_ref&)>` |
| `for_each_frame(capture, f, extra = nullptr)` | function | every data frame of a FILE or bare stream in order, with the dictionary as known at that frame; meta frames are consumed (dictionary frames and 1.7 deltas merged; checkpoint and writer-info frames skipped); a frame whose CRC fails is a problem and is skipped; `f` returns false to stop |
| `for_each_frame_in_file(path, f, block_options = {}, extra)` | function | the same, reading the file in windows (bounded memory; pipes and `"-"` too); a back-patched FILE's dictionary is read with `pread` first. The reader (A6) under every capture path of the tools; a frame straddling a window is completed from the next, a frame larger than the window grows it within `decode_limits::frame_bytes` |
| `mapped_file::open(path, populate = false)` | class | read-only `mmap` of a whole file (`MADV_SEQUENTIAL`, optional `MAP_POPULATE`); `bytes()` feeds `for_each_frame` / `walk_values` / `verify` / `dump` with no copy, for a file that fits the address space |
| `file_options` | struct | `bool blocks` (**`true`**), `std::size_t block_bytes` (0 = `decode_limits::window_bytes`), `bool populate = false` (mapped: `MAP_POPULATE`); `const decode_limits* limits = nullptr` — how a path is read |
| `for_each_frame(path, f, file_options = {}, extra = nullptr)` | function | a capture on disk in one call. The default (A6) is the windowed reader (`for_each_frame_in_file`, `block_bytes` at a time); `{.blocks = false}` maps the whole file instead (`mapped_file` + `for_each_frame(span)`), falling back to windows for `"-"` and anything that is not a regular file; frame offsets are file offsets either way (`src/reader.cpp`) |
| `read_file(path)` / `write_file(path, data)` / `write_all(fd, data)` | functions | file helpers (`src/file_io.cpp`); `read_file` accepts `"-"` (stdin) and refuses more than `decode_limits::input_bytes` (1 GiB) with `unsupported` |

### `namespace es::serreg::cli`

| name | kind | meaning |
|---|---|---|
| `options` | struct | everything `srdump` understands after `--config` and the command line; `wireshark` (the `--wireshark` output path, `"-"` = stdout); `cxx` (the `--cxx` output path), `cxx_router`, `namespaces` (`old=new` strings), `direction` (`--diff --direction`, `old_data` by default), `window_bytes` (`--window`; 0 = `decode_limits::window_bytes`), `frame_limit` (`--frame-limit`; 0 = the process-wide `decode_limits::frame_bytes`); `level`, `since`, `until` (`--level`, `--since`, `--until`, as text), `info` (`--info`), `resync` (`--resync`) |
| `app` | struct | how the hosting program presents itself: `name = "srdump"`, `version = library_version`, `extra_usage`, `on_extra` (callback for flags cli does not own) |
| `parse(argc, argv, options&, app = {})` | function | loads `--config` first, then flags; 0 or 2 (usage error) |
| `execute(options, app = {})` | function | runs them; exit codes 0 ok, 1 verify problems / decode or tool error, 2 usage |
| `run(argc, argv, app = {})` | function | `parse` + `execute` |
| `usage(app = {})` | function | the `--help` text |
| `parse_config(text, err)` | function | one `--config` document (FR-D9 TOML subset); exposed for `fuzz_config` |

## How it works

### Verify decodes everything

```cpp
// `verify` checks the container structure *and* decodes every record through the descriptor walker (cc F2 /
// gk 1.4); literal ids on the wire must equal the call signature's; dictionary conflicts are problems too.
// 2.1.16 (A6): the same over a file (or "-"), read in windows — a capture of any size with bounded memory.
result<verify_report> verify(std::string_view path, const dyn::dictionary* extra = nullptr,
                             const block_options& o = {});
result<verify_report> verify(std::span<const std::byte> capture,
                             const dyn::dictionary* extra = nullptr);
```
(`tools.hpp:184-190`). A capture is either a FILE (header + dictionary + frames) or a bare FRAME stream; `extra`
dictionaries (from `--dict`/`--elf`) are merged over the embedded one — a bare stream needs one or a leading
dictionary frame. `verify` returns `ok` with counts, or a report whose `problems` list is what `srdump --verify`
prints; the fuzz target `fuzz_capture` runs `verify` and both `dump` renderings over mutated captures, so every
decode path must fail with a problem, never a crash. The report also has `warnings`: input every
reader repairs (a `bool` byte other than 0/1), listed but not failing the report; `srdump --verify` prints them as
`warning:` lines, and `--json` adds a `"warnings"` array only when there are any. `verify` and `dump` handle compile-time constants
(format 1.5): a constant is counted and decoded as the scalar or enum record
it stands for, with its value from the call's dictionary entry in a compact frame, and `verify` reports a
full-records frame whose constant differs from that entry ("constant differs from the call's value",
`src/reader.cpp`). The report also carries `program_hash` (what `srdump --program-hash` prints for
a capture) and `problems_omitted`: a walk keeps at most `decode_limits::problems` lines (1000) and counts the
rest, so `ok()` is "no problems and none omitted". The path overload (A6) is the one `srdump --verify`
uses: the file is read `block_bytes` at a time (`decode_limits::window_bytes`, 10 MiB, unless set), the state
between frames is the dictionary and the counters, and `block_options::limits` can replace the process-wide
`decode_limits` for this call.

### Wireshark dissector

```cpp
struct wireshark_options
{
    std::string generator = "srdump"; // named in the header comment and in SERREG.generator
    std::vector<std::string> sources; // the dictionary sources, listed in the header comment
};
std::string wireshark_dissector(const dyn::dictionary& d, const wireshark_options& o = {});
```

(`tools.hpp:151-156`.) A pure function of the dictionary: it returns the whole Lua file as a string and writes
nothing. The caller chooses and merges the dictionary (`srdump --wireshark` merges the capture's own dictionary with
`--dict`/`--elf`/`--rri` and refuses conflicts first; an embedding application with no source uses its
own registry, `files/src-cli.cpp.md`); `generator` and `sources` only
fill the header comment and `SERREG.generator`. The generated file and its runtime are described in
`files/src-wireshark.cpp.md`.

### C++ definitions (plan A5)

```cpp
struct cxx_options
{
    bool router = false; // also emit install_handlers(frame_router&), one handler per call signature
    // "old" -> "new" namespace prefixes (--namespace old=new); a remapped type keeps its canonical name
    // through the name annotation, so its hashes do not move
    std::vector<std::pair<std::string, std::string>> namespaces;
    std::string_view include = "<es/serreg/serreg.hpp>"; // the library include line
};
std::string cxx_header(const dyn::dictionary& d, const cxx_options& o = {});
```

(`tools.hpp:169-177`.) The same shape as the Wireshark generator: a pure function of the dictionary that returns
the whole header as a string. The header declares every type of `d` and ends with `static_assert`s of every size,
offset, canonical name, schema hash and layout hash, so compiling it proves the regeneration; `router` appends
`install_handlers(es::serreg::frame_router&)` with one handler per call signature; `namespaces` moves declarations
without moving hashes; `include` is the first `#include` line. `srdump --cxx` collects the dictionary as
`--wireshark` does and refuses two types of one name (`files/src-cli.cpp.md`); the rules and limits of the output
are in `files/src-cxxgen.cpp.md`.

### Streaming dump

`dump` calls `out` once per frame (and once for the dictionary); returning `false` stops the walk with
`error::io_error` (cc F19). The buffered overload appends to a string. `dump_options.first_frame/last_frame`,
`type_filter`/`type_name_filter` select; `hex` adds raw bytes; `flatten` presents base-class members inline (§6.1b
option d) instead of `<base:Name>` sub-records; `array_limit`/`full_arrays` elide long arrays; `raw` shows a print
frame's records instead of the rendered message. `dump` also takes a path (A6): the file is read
in windows, twice — the dictionary printed at the top is "as of the end", so one pass collects it and one
renders the frames — and a print frame's records are copied until the frame is rendered, because a window's
bytes live only during the callback. The problem tail is capped like `verify`'s (`problem: ... and N more`;
`{"problems_omitted":N}` in JSON). The two passes are why `srdump` still reads a pipe whole: a pipe cannot be
read twice.

### A capture on disk in one call (windows by default)

```cpp
struct file_options
{
    bool blocks = true;
    std::size_t block_bytes = 0;           // 0: decode_limits::window_bytes
    bool populate = false;                 // mapped: MAP_POPULATE
    const decode_limits* limits = nullptr; // nullptr: the process-wide limits
};
result<frame_walk> for_each_frame(std::string_view path, const frame_callback& f,
                                  const file_options& o = {},
                                  const dyn::dictionary* extra = nullptr);
```

(`tools.hpp:518-527`.) Without it every reader of a file would write the same branch: open a `mapped_file` and
call `for_each_frame(bytes)`, or call `for_each_frame_in_file` for blocks, stdin and pipes. This overload is that
branch, implemented once in `src/reader.cpp`. The default (A6) is `blocks` = `true`, so the file is read in windows of `block_bytes` (`decode_limits::window_bytes`, 10 MiB, when 0) and a
capture of any size walks with bounded memory; `{.blocks = false}` asks for `mapped_file::open(path, populate)`
and the span walk — no copy, the page cache decides — with an `unsupported` result (not a regular file) falling
back to the windows, and any other error (`not_found`, `io_error`) returned. `"-"` is always windows.
`walk_values(path, …)` in `walk.hpp` makes the same choice. `frame_router::callback()`
(`router.hpp`) is the usual callback: `tools::for_each_frame("capture.ser", router.callback())`.

```cpp
//   tools::for_each_frame("capture.ser", router.callback());                 // windows of 10 MiB
//   tools::for_each_frame(path, on_frame, {.block_bytes = std::size_t{64} << 20});
//   tools::for_each_frame(path, on_frame, {.blocks = false});                 // map the whole file
```

(`tools.hpp:375-377`.) What a window means for the callback: `frame_ref::bytes` is valid only during the call in
both branches (it was already so for the mapping, which lives on the stack of the overload), and a frame that
does not fit the window grows the reader's buffer for that frame, within `decode_limits::frame_bytes`.

### The visitor and its defaults

```cpp
struct decoded_value_visitor
{ // §6.4 style 4: descriptor-driven walk; zero allocations in the walker
    virtual void begin_struct(std::string_view type_name, std::string_view field_name) = 0;
    virtual void end_struct() = 0;
    virtual void begin_array(std::string_view field_name, std::uint32_t count) = 0;
    virtual void end_array() = 0;
    virtual void scalar(std::string_view field_name, kind k, std::span<const std::byte> bytes) = 0;
    virtual void enumerator(std::string_view field_name, std::string_view enum_type, std::string_view name, std::int64_t value) = 0;
    virtual void literal(std::string_view field_name, std::uint64_t id, std::string_view text) = 0;
    // v2 dynamic members: defaults express them through the 1.0 events
    virtual void string(std::string_view field_name, std::string_view s);        // begin_array + scalar(char)× + end_array
    virtual void begin_vector(std::string_view field_name, std::uint32_t count); // begin_array
    virtual void end_vector();                                                   // end_array
    virtual void null_value(std::string_view field_name);                        // literal(field, 0, "null")
    virtual void begin_variant(std::string_view field_name, std::uint32_t index, std::string_view alt_text); // begin_struct(alt_text, field)
    virtual void end_variant();                                                  // end_struct
    // 1.6 (2.2.0): maps and sets; defaults: a vector of "entry" structs, a vector
    virtual void begin_map(std::string_view field_name, std::uint32_t count);    // begin_vector
    virtual void begin_entry(std::uint32_t index);                               // begin_struct("entry", {})
    virtual void end_entry();                                                    // end_struct
    virtual void end_map();                                                      // end_vector
    virtual void begin_set(std::string_view field_name, std::uint32_t count);    // begin_vector
    virtual void end_set();                                                      // end_vector
    // 1.6: a container argument's transparent type; default: a struct named after the container
    virtual void begin_transparent(std::string_view type_name, std::string_view field_name); // begin_struct
    virtual void end_transparent();                                              // end_struct
    virtual void noncanonical_bool(std::string_view field_name, std::byte raw);  // 2.1.3: no-op
};
```
(`tools.hpp:312-407`, condensed). The seven flat-value callbacks are pure virtual; the six dynamic-member ones and the
eight container ones have bodies that translate into
them, so a visitor that implements only the seven keeps working on any capture: a string becomes an array of
`char` scalars, a vector an array, an empty optional or valueless variant a literal `"null"`, an engaged variant a
struct named `variant<i:type>` holding `"value"`, a map a vector of `entry` structs with `key` and `value`, a set a
vector of keys, a transparent record a struct named `std::vector<i32>` with one unnamed member. The text/JSON formatters in `src/formatters.cpp` override the
dynamic and container events to render `name: "…"`, `xs: [1, -2, 3]`, `side: null`, `v: variant<2:string> { value: "var" }`,
`by_name: {"a": 1, "b": 2}`, `ids: {10, 20, 30}` and a transparent record as `[3, 1, 2]`
(JSON: strings, arrays, `null`, `{"alt":2,"type":"string","value":…}`, `[{"key":"a","value":1}]`, `[10,20,30]`,
the bare value). `visit_record` takes the record *payload*
(for a dynamic type: the image, prefix stripped — `record_view::payload` gives exactly that) and the `dyn::type`
from the dictionary; the walker allocates nothing and checks every slot with the rules the typed decoder uses
(`fmt::image_budget`), so a corrupt image is a verify problem exactly when it is `length_mismatch` for
the typed readers. A valueless variant is `null_value` here; a typed read picks `std::monostate` or reports
`invalid_value`. A visitor that prints scalars the way srdump does calls `tools::value_text(k, bytes)` in
its `scalar` callback instead of carrying its own copy of the rendering (`examples/user_example1/tree_printer.hpp`).

### `diff`

```cpp
enum class diff_direction : std::uint8_t
{
    none,
    old_data,
    new_data,
    both
};
```

(`tools.hpp:81-87`.) Type verdicts: `identical | names-only | metadata-only | breaking | added | removed | duplicate-name`; call and
literal verdicts (gk 1.11): `call added/removed/changed`, `literal added/removed/changed`. For enums any
enumerator value change or removal is breaking. Whether (decision E, review T-09) a *removal* or an
*addition* breaks anything is decided by the `direction` the caller names: under `old_data` (the default — a
reader built from `b` reads data written by `a`, replay) removed types, calls and literals are breaking and
additions are not; under `new_data` (a reader built from `a` on a live stream of `b`) the reverse; `both` marks
both; `none` neither. A layout change, a changed call and a changed literal break in every
direction. A breaking entry has `diff_entry::breaking` and a `note`; the report counts them
(`breaking_changes`, `breaking_removals`, `breaking_additions`) and `diff_report::breaking` aggregates;
`srdump --diff a.rri b.rri [--direction d]` prints `to_text()` and exits 1 when breaking. The verdicts describe
identity, not decodability (`files/src-diff.cpp.md`).

### Rendering contract of the text form

Two text shapes come out of `src/formatters.cpp`, both compared byte-for-byte against fixtures. The dump form
(`tests/fixtures/golden/orders.text`) is multi-line:

```
frame 0  call 0x737913edee6e1ae7  (2 records)
  [0] "new order"
  [1] corpus::Order {
        id: 42
        px: 1050000
        qty: 100
        side: corpus::Side::buy (1)
        sym: "AAPL\0\0\0\0"
      }
```
(enumerators as `Type::name (value)`, `char` arrays as escaped strings when every element is a char — the
heuristic resets on composite elements, so an array of structs is never mistaken for a string; arrays
elided after `array_limit` unless `full_arrays`; base classes as `<base:Name>` sub-records unless `flatten`;
`(2 records, full)` marks full-records frames). The one-line form is what `render_frame` substitutes for a struct
argument of a print call (`corpus::Order{id: 42, …}`, enumerators by bare name: `print: px=2.50 qty=7 side=sell`).
The dictionary listing (`struct corpus::Order  schema 0x…  layout 0x…  size 32  align 8  flat`, `dynamic` for
dynamic types, `print`/`println` call lines) heads every text dump. JSON renders `i64`/`u64` as quoted decimal strings
(gk 1.12) so values above 2⁵³ survive JavaScript consumers.

### Exit codes and where they come from

| situation | `cli::execute` returns |
|---|---|
| `--help`, `--version`, a clean dump/verify/diff | 0 |
| `verify_report.ok() == false` (problems listed), a `tools::` error (`to_string(e)` printed), a `diff` with `breaking` | 1 |
| usage error from `parse` (unknown flag not taken by `on_extra`, missing operand, bad `--config` line) | 2 |

`tests/dlopen/CMakeLists.txt` and `tests/unit/CMakeLists.txt` assert these (`srdump_elf_conflicting_plugins_exit_1`,
`srdump_elf_single_plugin_exit_0`, `golden_srdump_verify` with `PASS_REGULAR_EXPRESSION`).

### The CLI as a library

```cpp
int main(int argc, char** argv)
{
    int trace = 0;
    return es::serreg::cli::run(argc, argv, {
        .name = "myapp", .version = "2.3", .extra_usage = "  --trace     more output\n",
        .on_extra = [&](std::string_view a, std::span<char* const>&) { return a == "--trace" ? (++trace, true) : false; }});
}
```
(`tools.hpp:545`). `app::on_extra` is consulted for every argument cli does not own before it becomes a usage
error; consuming a value means advancing `rest`. `parse` fills `options` (after any `--config`), `execute` runs
them exactly as `srdump` would, `usage` renders `--help` under `a.name`. Nothing here does what `tools::` cannot.

## Maintaining it

- **`library_version` must match the project version** (`CMakeLists.txt`, `CHANGELOG.md` heading);
  `tests/unit/test_tools.cpp` "cc F24 tools::library_version matches the project and cli::run knows --version" and
  ctest `srdump_version` check it. Library and wire versions are separate numbers by design — `--version` prints
  both.
- **Rendering is golden.** `dump` text/JSON/TOML output is byte-compared against `tests/fixtures/golden/
  {orders,dynamic}.{text,json,toml}` (`golden_srdump_*_matches_fixture`). Any change to a formatter regenerates the
  fixtures (through
  `tests/golden/make_fixtures.cpp` / `make_dynamic_fixtures.cpp`, ctest `golden_fixtures_generate` →
  `golden_fixture_*_byte_exact`) and needs a CHANGELOG line.
- **Visitor ABI.** Adding a pure-virtual callback breaks every external visitor; add new events with a default
  body expressed through existing events, as the dynamic and container events do. Keep the walker allocation-free.
- `verify`/`dump` must return an error *after* the walk for a bad record (the walk continues; `srdump` exits 1);
  do not turn that into an early return — `tests/unit/test_tools.cpp` "cc F19 tools::dump streaming overload
  delivers per-frame chunks and stops when asked" and "gk 1.4 a flipped literal id makes tools::verify report a
  problem" cover the two behaviours.
- `read_file`'s cap (`decode_limits::input_bytes`, 1 GiB) and the stdin `"-"` convention are CLI contracts
  (`--stdin`, `use_stdin`); no capture path of `srdump` reads a file whole except a dump of a pipe
  (the dictionary collection of `--wireshark`/`--cxx` walks the capture through the same windows).
- **`file_options::blocks` defaults to `true`:** memory stays bounded by default, and `frame_ref::bytes` lives
  only during the callback (the contract in any case). A caller that wants the mapping writes `{.blocks = false}`.
- **Limits are read once per walk.** A new reader option that bounds work goes into `decode_limits`
  (`limits.hpp`), is reachable per call through `block_options::limits`/`file_options::limits`, and has a case in
  `tests/unit/test_limits.cpp`.
- `parse_config` accepts only the FR-D9 TOML subset (§11 default); it is fuzzed by `fuzz_config`. Extending the
  option set means `options`, `parse`, `parse_config`, `usage` and the `--config` documentation together.
- ELF reading (`src/elf_reader.cpp`) parses section headers from a file or image; `fuzz_elf` mutates images into
  `extract_section_from_image` and `extract_embedded_dictionary`. The reader looks for `es_serreg_dict` by
  name; it does not process relocations. A reader for another executable format is a new branch on the magic in
  `extract_embedded_dictionary`; `extract_registry_from_elf` stays a wrapper.
- `wireshark_dissector`'s runtime is a copy of the wire format and of the decoder's slot rules in Lua; a format or
  decoder change updates it in the same commit (`files/src-wireshark.cpp.md`), and the `wireshark_*` ctests check
  it.
- `value_text` is the text formatter's `scalar_text`; a change to it changes srdump's text fixtures too
  (`files/src-formatters.cpp.md`).
- `for_each_frame(path)` and `walk_values(path)` must keep choosing the same strategy (windows unless
  `{.blocks = false}` on a regular file); `tests/unit/test_readers.cpp` "2.1.10 for_each_frame(path) and walk_values(path):
  mapped and in blocks agree" compares the two, and `tests/unit/test_limits.cpp` "2.1.16 A6 every window size
  gives the whole-file output, byte for byte" compares `verify`/`dump` over a path with the span output.
- Tests: `tests/unit/test_tools.cpp` (all cases; "2.1.16 decision E: --diff --direction decides whether
  removals or additions are breaking", the two `[tools][cxx]` cases), `tests/unit/test_limits.cpp` (the
  limits, the windowed `verify`/`dump`, `--window`/`--frame-limit`), `tests/cxxgen/`, `tests/wireshark/` (`wireshark_generate`,
  `wireshark_dissector_parity`), `tests/unit/test_readers.cpp` (the path walks), `tests/unit/test_capture.cpp` (verify/dump of `capture_file`
  output, dictionary frames, `extract_registry_from_elf(/proc/self/exe)`, `ES_SERREG_DUMP_REGISTRY_FD`),
  `tests/unit/runtime_selftest.cpp` (ELF reader; `extract_embedded_dictionary` on ELF, `MZ` and 3-byte
  inputs), `tests/link/image_none_main.cpp` (`not_found` without the section),
  `tests/unit/test_print.cpp` ("FR-P4 tools::render_frame …", "FR-P5 srdump text renders print frames …"),
  `tests/unit/test_log.cpp` ("A log: the envelope by policy … renders in srdump": `render_log` on one frame and
  through `frame_router::on_log(level, h)`),
  `tests/unit/test_dynamic.cpp` ("2.0 tools: dump text/JSON of dynamic values …"), the dlopen `srdump_elf_*` tests,
  `tests/unit/CMakeLists.txt` `srdump_*` tests, and the four fuzz targets.
