# `src/cli.cpp`

The `srdump` command line as a library: `es::serreg::cli::parse()`, `execute()`, `run()`, `usage()` and
`parse_config()` from `include/es/serreg/tools.hpp` (spec §6.9; FR-D0/D1/D2/D3/D6/D7, FR-D9 for `--config`).
`parse()` fills a `cli::options` from a `--config` TOML-subset file and then the flags; `execute()` dispatches
to `tools::` — `diff`, `load_dictionary_file`, `extract_registry_from_elf`, `dyn::to_rri`, `wireshark_dissector`,
`cxx_header`, `dump_dictionary`, `verify`, `dump` and `info` — under the hosting application's name (`cli::app`), streaming `dump` output to `stdout`
one frame at a time. Exit codes: 0 ok, 1 verify problems or a decode/tool error, 2 usage error. The file does
nothing `tools::` cannot do: no parsing of captures, no rendering of records, no file format knowledge beyond
two magic checks in `load_any`.

## Depends on / used by

Includes: `internal.hpp` (`dec`, `hex64`, `parse_u64`, `append_quoted`), `es/serreg/registry.hpp`
(`registry::instance().export_image()` for `--wireshark` without a source), `es/serreg/tools.hpp`,
`<sys/stat.h>` (`is_regular_file`), `<cstdio>`
(`fwrite`/`fflush` in `emit`), `<map>`, `<print>` (diagnostics: `std::println(stderr, …)`).
From `format.hpp`: `fmt::rri_magic`, `fmt::file_magic`, `fmt::file_header_size`, `format_major`/`format_minor`,
`hash_algo_xxh3_64`.

Library: `es::serreg_tools` (`libserreg_tools`, decision G).

Used by: `tools/srdump/main.cpp` (`cli::run(argc, argv)`), `examples/embedded_cli.cpp` (an `app` with
`on_extra`), `tests/unit/test_tools.cpp` ("cc F24 … cli::run knows --version", "cli::parse hands unknown flags
and extra operands to the application"), `test_print.cpp` (FR-P5 through `cli`), `tests/fuzz/fuzz_targets.cpp`
(`fuzz_config` → `cli::parse_config`), and every `srdump_*`, `golden_srdump_*`,
`srdump_elf_*` and `wireshark_*` ctest, and `example_embedded_cli_wireshark`.

## Structure

- **`usage_options`** (`cli.cpp:26-77`): the option block of `--help` (`--wireshark`; "or this
  program's own registry when none is given"; `--cxx`, `--cxx-router`, `--namespace`, `--direction`,
  `--window`, `--frame-limit`; `--info`, `--resync`, `--level`, `--since`/`--until`).
- **`is_regular_file()`** (`cli.cpp:80-84`): whether the dump may read its input twice (a regular
  file) or must slurp it (a pipe, a device).
- **`parse_size()`** (`cli.cpp:87-128`): a byte count with an optional `K`/`M`/`G` suffix (powers of
  two; `10M`, `64K`, `4096`), overflow-checked; for `--window`, `--frame-limit` and their config keys.
- **`set_direction()`** (`cli.cpp:155-168`): `old-data | new-data | both | none` →
  `tools::diff_direction`, for the flag and the config key.
- **`parse_time()`** (`cli.cpp:130-153`): a `--since`/`--until` time — an integer (the writer's clock
  units; ns since the epoch for the system clock) or an ISO-8601 UTC date-time `YYYY-MM-DD[THH:MM[:SS[.fff]]]`,
  parsed with `std::chrono::from_stream`.
- **Diagnostics** (`cli.cpp:170-178`): `fail(app, what, error)` → `"<name>: <what>: <error>"` on stderr;
  `usage_error(app, msg)` → message on stderr, returns 2. Both use `std::println(stderr, …)`.
- **TOML subset** (`cli.cpp:180-486`): `trim`, `normalize_key` (drops `-` and `_`), `struct value`
  (`str | integer | boolean | strarray`), `parse_string` (basic string with `\n \t \" \\`), `parse_value`,
  `apply(options&, key, value)` (the key table, `cli.cpp:308-447`; `cxx`, `cxxrouter`, `namespace`,
  `window`, `framelimit`, `direction`; `info`, `level`, `since`, `until`, `resync`), and the line reader
  `parse_config_text` (`cli.cpp:450-486`).
- **`load_config()`** (`cli.cpp:488-500`): read the file, parse, report `"<path>:<line>: <reason>"`.
- **`parse_args()`** (`cli.cpp:502-667`): the flag loop with `next()` (operand fetch) and `extra()`
  (`app::on_extra` hand-off); `--info`, `--resync`, `--level`, `--since`, `--until` at `cli.cpp:544-562`;
  `--wireshark` at `cli.cpp:590-594`; `--cxx`, `--cxx-router` and `--namespace old=new` at
  `cli.cpp:595-607`; `--window`, `--frame-limit` and `--direction` at `cli.cpp:633-655`.
- **`parse_frames()`** (`cli.cpp:669-682`): `A..B`, `A..`, `..B`, `A`.
- **`load_any()`** (`cli.cpp:685-698`): RRI or ELF by magic, for `--diff`.
- **`collect_dictionary()`** (`cli.cpp:707-775`): the dictionary a generator describes — the capture's
  own (for a 1.7 file the opening block plus the delta chain through `tools::info`, by seeks; a 1.6 file, a
  stream or a pipe is walked), then `--dict`/`--elf`/`--rri`, or the process's own registry when no source
  is given — shared by `--wireshark` and `--cxx`; conflicts and an empty dictionary are refused here.
- **`write_generated()`** (`cli.cpp:778-794`): the generated text to a file (with the one-line summary on
  stderr) or to stdout for `-`.
- **`write_wireshark()`** (`cli.cpp:797-806`): `collect_dictionary` + `tools::wireshark_dissector`.
- **`write_cxx()`** (`cli.cpp:810-836`): `collect_dictionary`, the duplicate-name refusal, the
  `--namespace` rules parsed into `cxx_options`, `tools::cxx_header`.
- **`emit()`** (`cli.cpp:838-843`): `fwrite` + `fflush` to stdout.
- **Public functions** (`cli.cpp:847-1178`): `usage`, `parse`, `run`, `execute` (a wrapper that checks
  standard output; applies `--frame-limit` to the process-wide `decode_limits` first), `parse_config`;
  the dispatch itself is the internal `execute_command()` (`cli.cpp:904-1170`).

## How it works

### `parse`: two passes over argv

```cpp
int parse(const int argc, char** const argv, options& o, const app& a)
{
    for (int i = 1; i + 1 < argc; ++i) // --config first, so explicit flags override it
        if (std::string_view(argv[i]) == "--config" &&
            !std::string_view(argv[i + 1]).starts_with("--"))
        {
            if (const int rc = load_config(a, o, argv[i + 1]))
                return rc;
        }
    return parse_args(a, o, argc, argv);
}
```

(`cli.cpp:857-867`.) Pass 1 finds every `--config <file>` and applies it in order; pass 2 (`parse_args`)
applies the flags, so a flag on the command line overrides the file. In pass 2 `--config` only consumes its
operand (`cli.cpp:561-565`). `next()` refuses an operand that starts with `--` (`--config --verify` is a usage
error, not a file named `--verify`, gk §2). Lists (`--dict`, `--elf`) accumulate across both passes.

### `cli::app` and `on_extra`

```cpp
const auto extra = [&] {
    if (!ap.on_extra)
        return false;
    std::span<char* const> rest(argv + i + 1, static_cast<std::size_t>(argc - i - 1));
    if (!ap.on_extra(a, rest))
        return false;
    i = argc - static_cast<int>(rest.size()) -
        1; // the loop's ++i lands on the first unconsumed
    return true;
};
```

(`cli.cpp:508-517`.) An unknown `-flag` or a second positional operand is offered to the application's
`on_extra(arg, rest)` before it becomes a usage error. The callback consumes values by shrinking `rest`
(`rest = rest.subspan(1)`); the loop index is recomputed from what is left. `app::name` prefixes every
diagnostic and the usage banner, `app::version` is printed by `--version`, `app::extra_usage` is appended to
`--help`. All fields default to srdump's, so `cli::run(argc, argv)` *is* srdump.

### The TOML subset (`parse_config_text`, `apply`)

```cpp
if (line.front() == '[')
{ // 2.1.12 (review D-09): a comment may follow the header
    const std::size_t close = line.find(']');
    const std::string_view rest =
        close == std::string_view::npos ? std::string_view{} : trim(line.substr(close + 1));
    if (close == std::string_view::npos || (!rest.empty() && rest.front() != '#'))
        return dec(line_no) + ": malformed table header";
    continue;
}
const std::size_t eq = line.find('=');
if (eq == std::string_view::npos)
    return dec(line_no) + ": expected key = value";
const std::string_view key = trim(line.substr(0, eq));
if (key.empty() || key.find('.') != std::string_view::npos || key.front() == '"')
    return dec(line_no) +
           ": unsupported key syntax (dotted/quoted keys are not in the v1 subset)";
```

(`cli.cpp:462-477`.) Per spec §11 the subset is: `#` comments, blank lines, `[section]` headers (accepted
and ignored), and `key = value` with a basic string, an integer, `true`/`false`, or an array of strings. Two
details follow TOML (D-09): a `# comment` may follow a table header
(`[srdump] # the tool`; any other text after the `]` is a malformed header), and the items of an array must
be separated by commas (`["a" "b"]` is rejected; `parse_value` accepts a `,` or the closing `]` after each item).
Dotted or quoted keys, multi-line strings, inline tables and every other TOML construct return an error naming
the line. `parse_value` also accepts a trailing `# comment` after a value. `apply()` maps the normalised key
(`dict-only` → `dictonly`, `rri_out` → `rriout`) onto `options`: booleans for `stdin`, `dictonly`, `verbose` (2.2.6), `verify`,
`hex`, `raw`, `flatten`, `full`, `json`/`toml`/`text`; strings for `capture`, `rriout`, `rri`, `frames`,
`type`, `format` (`"json"|"toml"|"text"`), `cxx`, `direction` (`"old-data"`, `"new-data"`,
`"both"` or `"none"`), `level`, `since`, `until`; booleans also for `info` and `resync`; a string or string array
for `dict`, `elf` and `namespace`; a two-element array for `diff`; a boolean for `cxxrouter`. Two keys take a size: `window` (at least
4096) and `framelimit` (at least 16), either as an integer (`window = 4096`) or as a string with a `K`/`M`/`G`
suffix (`window = "4M"`), through `parse_size` (`cli.cpp:87-128`). A wrong type or an unknown key is an error
with the key name. `parse_config()` (`cli.cpp:1173-1178`) runs the same parser against a scratch `options`
for `fuzz_config`.

### `execute`: dispatch order

`execute()` (`cli.cpp:882-900`) first applies `--frame-limit`, when given, to the process-wide limits
(`get_decode_limits()`, `frame_bytes = o.frame_limit`, `set_decode_limits()`; FR-D5), then calls
`execute_command()` (`cli.cpp:904-1170`), which runs in this order,
returning at the first branch taken:

1. `--help` → `usage(a)`, 0. `--version` → `"<name> <version> (format <major>.<minor>, hash_algo <n>)"`, 0
   (`srdump_version` matches `format 1.7, hash_algo 2`).
2. `--diff a b` → `load_any` both (RRI by `"ESRG"`, ELF by `"\x7fELF"`, else `bad_magic`),
   `tools::diff(*da, *db, o.direction)` (decision E: `old-data` unless `--direction` or the config key
   `direction` said otherwise), print `to_text()`, exit `breaking ? 1 : 0`.
3. Build the side dictionary `extra` from every `--dict` (`load_dictionary_file`), `--elf`
   (`extract_registry_from_elf`, a wrapper for `extract_embedded_dictionary`: a non-ELF file is
   `unsupported`, an ELF file without the section `not_found`) and `--rri`, merging in that order; `have_extra` remembers whether any was
   given. `--rri-out` writes `dyn::to_rri(extra)` immediately.
4. `--wireshark <f.lua|->` → `write_wireshark(o, a, extra, have_extra)` (below) and return;
   `--cxx <f.hpp|->` → `write_cxx(o, a, extra, have_extra)` likewise.
5. No capture and no `--stdin`: without `extra` that is a usage error; `--program-hash` prints
   `extra.program_hash` or, when the sources had no header (ELF sections), `computed_program_hash()`;
   otherwise `dump_dictionary(extra, fmt)` is printed and, if `extra.conflicts` is non-empty, each conflict
   goes to stderr and the exit code is 1 (`srdump_elf_conflicting_plugins_exit_1`, cc F15/F16).
6. From here on the capture is read in windows (A6): `source` is the path or `"-"`, and a
   `tools::block_options window` carries `--window` (`block_bytes`; 0 = `decode_limits::window_bytes`, 10 MiB)
   and `--resync` (`window.resync`). Nothing below reads a capture whole, except the one case in
   step 9.
6a. `--info`: `tools::info(o.capture)` and `to_text`/`to_json` of the result; exit 0, or 1 when the file
   cannot be read. A pipe is a usage error (`--info needs a file (a pipe cannot be seeked)`): the chains are followed
   with seeks (`cli.cpp:1022-1034`).
7. `--program-hash` with a capture: `tools::verify(source, extra, window)` must at least parse the container;
   the hash is `verify_report::program_hash` (the FILE header's), falling back to the side dictionary's
   when the capture is a bare stream.
8. `--verify`: `tools::verify(source, extra, window)`; render the report (below); exit `ok() ? 0 : 1`.
9. Otherwise `tools::dump` with a `dump_options` built from the flags (`parse_frames`, `--type` as
   `0x…` hash or a canonical name; `--level` sets `logs_only` and `min_level` — an unknown name is a usage
   error — and `--since`/`--until` go through `parse_time` into `since`/`until`; a time range alone also sets
   `logs_only`, at level trace — `cli.cpp:1115-1135`), streamed to stdout: `dump(source, …, window)` over a regular file (two
   windowed passes — the dictionary is printed first and is "as of the end"); standard input or a pipe cannot be
   read twice, so that input is slurped with `read_file` (within `decode_limits::input_bytes`, 1 GiB) and
   dumped from memory (`cli.cpp:1151-1162`). That in-memory walk cannot resync, so `--resync` has no effect on a
   piped dump.

`execute()` then checks (T-07) that standard output was written:

```cpp
int execute(const options& o, const app& a)
{
    if (o.frame_limit != 0)
    { // 2.1.16 (FR-D5): --frame-limit narrows or widens the process-wide limit for this run
        decode_limits l = get_decode_limits();
        l.frame_bytes = o.frame_limit;
        set_decode_limits(l);
    }
    const int rc = execute_command(o, a);
    // 2.1.12 (review T-07): output that did not reach standard output (a full disk, a closed pipe) is a failure,
    // not exit 0. The error is reported once and cleared, for applications that call execute() again.
    if (std::fflush(stdout) != 0 || std::ferror(stdout))
    {
        std::clearerr(stdout);
        std::println(stderr, "{}: writing standard output failed", a.name);
        return rc == 0 ? 1 : rc;
    }
    return rc;
}
```

(`cli.cpp:882-900`.) Output that did not reach stdout (a full disk, a closed pipe, `/dev/full`) makes the call fail:
`<name>: writing standard output failed` on stderr, and exit code 1 when the command had otherwise succeeded (a
non-zero code of the command is kept). `clearerr` resets the stream so an application that calls `execute()`
again starts clean.

### `--wireshark` and `--cxx`: `collect_dictionary` (shared)

```cpp
        const auto walk = tools::for_each_frame_in_file(
            source, [](const dyn::dictionary&, const tools::frame_ref&) { return true; },
            tools::block_options{o.window_bytes, nullptr});
```

(`cli.cpp:731-733`.) A regular 1.7 file takes a shortcut first: `tools::info` collects its opening
dictionary and every delta of the forward chain by seeks (`cli.cpp:721-727`), so the dictionary of a huge file
costs one read per delta. Otherwise — a 1.6 file, a stream, a pipe — the dictionary is taken from the windowed walker
`tools::for_each_frame_in_file` (A6): the FILE header and the embedded dictionary first (`pread` at
`dict_offset`, also when back-patched to the end), then every frame through windows of `--window` bytes, so memory
stays bounded whatever the capture's size; the pass is O(size) in time, and for a very large capture `--rri`,
`--elf` or `--dict` describe the same types at once.
The callback returns `true` (T-08), so the walk covers the whole capture and
`walk->dict` is the FILE's embedded dictionary merged with every dictionary frame of the stream, including those
that arrive after data frames (a `dlopen`, a `dictionary_tracker` update); stopping at the first data frame would
lose the types announced later (`runtime_selftest` checks it, T-08). A capture that does not parse is `fail()` + exit 1. Then
`extra` (the merged `--dict`, `--elf`, `--rri` dictionaries, step 3) is merged over it, and every source is recorded
in `sources` (`capture <path>`, `--dict <path>`, `--elf <path>`, `--rri <path>`), which `--wireshark` passes on as
`wireshark_options::sources` with `generator = a.name`.

```cpp
if (sources.empty())
{ // an application embedding the CLI describes its own registry
    if (const auto own = tools::load_dictionary_rri(registry::instance().export_image());
        own && (!own->types.empty() || !own->calls.empty()))
    {
        d = *own;
        sources.push_back("the registry of " + std::string(a.name));
    }
    else
        return usage_error(a, std::string(flag) + " needs a capture, --dict, --elf or --rri");
}
```

(`cli.cpp:751-762`.) With no source at all, the dictionary is the running process's own registry: its
RRI image (`registry::instance().export_image()`, every type, call and literal registered by the program and its
loaded DSOs) read back with `load_dictionary_rri`. That is what an application embedding the command line wants
(`embedded_cli --wireshark app.lua`, ctest `example_embedded_cli_wireshark`, which expects `the registry of
embedded_cli` in the header comment and a `serreg.app_Tick.*` field). srdump itself registers no types or calls, so its
registry image is empty and the result is the usage error as before (exit 2, `--wireshark needs a capture, --dict,
--elf or --rri`).

A merged dictionary with `conflicts` prints `<name>: schema conflict: <type> <hash>` per conflict and
exits 1 without writing, as the dictionary-only dump does. A dictionary without any entry is an
error too (T-08):

```cpp
if (d.types.empty() && d.calls.empty() && d.literals.empty())
{ // a generator that knows nothing is not a result
    std::println(stderr, "{}: {}: the sources hold no dictionary entries", a.name, flag);
    return 1;
}
```

(`cli.cpp:769-773`.) A bare stream without a dictionary frame would otherwise produce a dissector that knows
nothing, with exit 0. Otherwise `write_generated` writes `tools::wireshark_dissector(d, wo)` — or, for `--cxx`,
`tools::cxx_header(d, co)` — to the file (`write_file`; a failure is `fail()` + exit 1) followed by
`<name>: wrote <file>: N types, N call signatures, N literals` on stderr, or with `-` emitted to stdout. The
generated files are documented in `files/src-wireshark.cpp.md` (with `docs/wireshark.md`) and
`files/src-cxxgen.cpp.md`. `write_cxx` first refuses a merged dictionary in which two types share a name
(`--cxx: duplicate type name <name>; pick one source (--program-hash) or --dict`, exit 1: two programs' types of
one name cannot both be declared), then turns every `--namespace old=new` into a `cxx_options::namespaces` pair
(a value without `=`, or with nothing before it, is a usage error) and sets `router` from `--cxx-router`.

### The verify report rendering

```cpp
for (const auto& p : rep->problems)
    out += "problem: " + p + "\n";
if (rep->problems_omitted)
    out += "problem: ... and " + dec(rep->problems_omitted) + " more\n";
for (const auto& w : rep->warnings)
    out += "warning: " + w + "\n";
out += (rep->ok() ? "ok: "
                  : "FAILED (" + dec(rep->problems.size() + rep->problems_omitted) +
                        " problems): ") +
       dec(rep->frames) + " frames, " + dec(rep->records) + " records, " +
       dec(rep->bytes) + " bytes\n";
```

(`cli.cpp:1089-1099`.) Text: one `problem:` line each, one `warning:` line each (input every reader repairs,
e.g. a `bool` byte 0x02 read as true; warnings do not change the exit code), then `ok: N frames, R records, B
bytes` or `FAILED (P problems): …`. A walk keeps at most `decode_limits::problems` lines (1000);
the rest are counted in `problems_omitted` and rendered as one `problem: ... and N more` line, and `P` counts
them. `--json`: `{"ok":…,"frames":…,"records":…,"bytes":…,"problems":[…]}`, with a `"problems_omitted":N`
member when lines were dropped and
a `"warnings":[…]` member after `problems` only when there are warnings (so the output of a clean capture is
unchanged). The ctests
`srdump_verify_capture`, `golden_srdump_verify` and `golden_srdump_verify_dynamic` match the `^ok: …` prefix.

### Streaming to stdout

```cpp
const auto to_stdout = [](const std::string_view chunk, void*) {
    return chunk.empty() || std::fwrite(chunk.data(), 1, chunk.size(), stdout) == chunk.size();
};
result<void> r;
if (o.use_stdin || !is_regular_file(source))
{ // a pipe: no second pass, so the input is read whole (decode_limits::input_bytes)
    const auto capture = tools::read_file(source);
    …
    r = tools::dump(*capture, extra_p, d, +to_stdout, nullptr);
}
else
    r = tools::dump(source, extra_p, d, +to_stdout, nullptr, window);
std::fflush(stdout);
```

(`cli.cpp:1147-1163`.) `dump` calls `to_stdout` once per frame (cc F19); a short write returns `false`, which
`dump` turns into `io_error`. A decode problem inside the capture makes `dump` return an error *after* the
walk, so the full rendering is printed and the exit code is 1. Over a regular file the path overload reads the
file twice in windows (A6: the first pass collects the dictionary, the second renders the frames), so a
dump's memory does not grow with the capture; a pipe is the one input `srdump` still reads whole.

### Exit codes

| code | when |
|---|---|
| 0 | success; `--verify` with no problems; `--diff` without a breaking entry for its direction (under the default `old-data` a removed type, call or literal is breaking — `--direction none` counts neither removals nor additions) |
| 1 | `--verify` found problems; `--diff` is breaking; any `tools::` error (unreadable file, `bad_version`, decode problems, ELF conflicts) — reported by `fail()`; `--wireshark`/`--cxx` with conflicting sources or with no dictionary entries; `--cxx` with two types of one name; standard output could not be written |
| 2 | usage: unknown option, missing operand, bad `--frames`/`--type`, no capture and no dictionary source (also for `--wireshark` and `--cxx`, unless the program's own registry has entries), a `--namespace` value that is not `old=new`, a `--window` below 4096 or `--frame-limit` below 16 bytes or without a parsable size, a `--direction` other than the four words, `--config` unreadable or rejected |

## Maintaining it

- **Tests:** `test_tools.cpp` (`--version`, `on_extra` hand-off; `--cxx` duplicate names, `--namespace`,
  the config keys, "2.1.16 decision E: --diff --direction decides whether removals or additions are breaking"),
  `test_limits.cpp` ("2.1.16 A6 srdump reads a capture in windows: --window and --frame-limit"),
  `test_print.cpp` (FR-P5 `--raw`/`--json`
  through `cli`), `tests/wireshark/CMakeLists.txt` (`wireshark_generate`, `wireshark_dissector_parity` —
  `--wireshark` with a capture, `--dict` and `--elf`), `tests/cxxgen/CMakeLists.txt` (`--cxx` from
  `--rri` and from a capture, with `--cxx-router` and `--namespace`), `examples/CMakeLists.txt`
  (`example_embedded_cli_wireshark`, `--wireshark -` from the process's registry), the `srdump_*` ctests in `tests/unit/CMakeLists.txt`, `tests/golden/CMakeLists.txt`
  (`golden_srdump_*`, program-hash agreement between capture, RRI and ELF),
  `tests/dlopen/CMakeLists.txt` (`srdump_elf_conflicting_plugins_*`, `srdump_elf_single_plugin_exit_0`),
  `scripts/run-all-tests.sh` (`srdump --verify` on the golden capture), and `fuzz_config` with
  `tests/fixtures/corpus/config/selftest.toml`.
- **A new flag touches four places:** `usage_options`, `parse_args`, `apply` (its config key, normalised),
  and `options` in `tools.hpp`; and usually a `dump_options` field in `execute`. Keep the config key equal to
  the long flag without dashes (FR-D9). `--wireshark` has no config key: `apply` does not know it;
  `--cxx`, `--cxx-router`, `--namespace`, `--direction`, `--window` and `--frame-limit` have `cxx`,
  `cxxrouter`, `namespace`, `direction`, `window` and `framelimit`. A size flag goes through `parse_size`
  with the same floor in both places (4096 for the window, 16 for the frame limit).
- **`--frame-limit` is process-wide:** `execute()` writes it into `decode_limits` before the command runs, so
  an application that calls `execute()` several times keeps the last value; `--window` is per call
  (`block_options::block_bytes`).
- **No `printf`/`puts` family** (the maintainer's rule): diagnostics use `std::println(stderr, …)`; `emit`
  and the dump stream use `fwrite` on already-rendered text. The library's own load-time and destructor
  diagnostics use `detail::report_line` instead (`files/include-es-serreg-sink.hpp.md`); the CLI runs in an
  ordinary program and may throw.
- **Never widen the TOML subset silently** — §11 fixes it; a widening is a spec change to propose, not to
  slip in. Error messages name the line and are compared by tests.
- **Exit codes are contract**; `fail()` + `return 1` for tool errors, `usage_error()` (2) for argument
  problems. Do not print usage on a tool error.
- **`emit()` flushes**; the streaming path flushes once at the end. Do not buffer the whole dump in a string
  in `execute` (cc F19).
- **Diagnostics go to stderr under `a.name`**; stdout carries only the requested output, so `srdump … | tool`
  pipelines stay clean.
- **Wire format:** `load_any` peeks at two magics; `--program-hash` takes `verify_report::program_hash`.
  If the magics ever change, update
  `load_any` with the `format_minor` bump.
- **Threading / allocation:** single-threaded, allocates freely; tools layer only.
