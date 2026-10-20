# 10. `srdump` and the tools layer

`srdump`'s `main` is one line: `return es::serreg::cli::run(argc, argv);`. Everything it can do is a public
function in `es::serreg::tools` or `es::serreg::cli`, so your own program can do the same (`examples/embedded_cli.cpp`).

## 10.1 `srdump` command line

```
usage: srdump [options] <capture.ser>
       srdump --elf <binary> [--elf ...] [--rri-out <file>]
       srdump --rri <image.rri>
       srdump --diff <a> <b>            (each an .rri file or an ELF binary)
options:
  --dict-only          print only the dictionary
  --verbose            with the dictionary: its blocks (opening block, every dictionary frame: offset,
                       what it added); with the frames: a marker line per dictionary frame
  --text | --json | --toml   output format (default text; records are JSON under --toml)
  --verify             check frames/records against the dictionary; exit 1 on problems
  --info               1.7: the file's header, dictionary chain, checkpoints and writer info,
                       by seeks (no frame walk); --json for machine reading
  --resync             1.7: after an unparsable frame, continue at the next checkpoint frame
  --level <lvl>        1.7: only log frames at or above trace|debug|info|warn|error|fatal, one
                       rendered line each (--json: {"log": {...}} objects)
  --since <t> / --until <t>   1.7: log frames whose timestamp is in range (implies --level trace); <t> is
                       ns since the epoch, or YYYY-MM-DD[THH:MM[:SS[.fff]]] (UTC)
  --dict <file.rri>    extra dictionary (repeatable)
  --elf <binary>       extra dictionary from the binary's es_serreg_dict section (repeatable)
  --rri-out <file>     with --elf and no capture: write the merged dictionary as an RRI
  --wireshark <f.lua>  write a Wireshark dissector for the capture's / --dict / --elf / --rri
                       dictionary, or this program's own registry when none is given
                       (docs/wireshark.md); "-" writes it to standard output
  --cxx <types.hpp>    write C++ definitions of the same dictionary's types, self-checking
                       (compiling the file verifies every size, offset and hash); "-" writes
                       it to standard output
  --cxx-router         with --cxx: add install_handlers(frame_router&), one handler per call
  --namespace old=new  with --cxx: declare the types of namespace old in new (repeatable; the
                       canonical name is kept through [[=es::serreg::name("...")]])
  --direction <d>      with --diff: which reader the verdict is for — old-data (default: a reader
                       built from the new side reads old data; removals break), new-data (an old
                       reader reads new data; additions break), both, none
  --window <size>      read the capture this many bytes at a time (default 10M; K/M/G suffixes);
                       a capture's size is bounded by the file system, not by this tool
  --frame-limit <size> largest frame accepted (default 64M); a larger frame_len is a problem
  --frames A..B        only frames A..B (inclusive, 0-based)
  --type <name|0xhash> only records of this type
  --hex                also print raw payload bytes
  --raw                show print frames as records, not the rendered message
  --flatten            present base-class members inline
  --full               do not elide long arrays
  --stdin              read the capture from standard input
  --config <file.toml> key = value settings (keys = long flag names without dashes)
  --program-hash       print the 0x-hex program hash of the capture / --rri / --elf dictionary and exit
  --version            print "<name> <version> (format <major>.<minor>, hash_algo <n>)"
  --help
```

Exit codes: 0 ok; 1 verify problems, decode/tool error, breaking diff (breaking *for the chosen direction* —
under the default `old-data` a removed type, call or literal is breaking; `--direction none` counts neither
removals nor additions), or schema conflict between `--elf` dictionaries (also for `--wireshark` and `--cxx`),
`--wireshark`/`--cxx` sources without any dictionary entry, two types of one name under `--cxx`, or standard
output that could not be written; 2 usage error. `cli::execute` (T-07) flushes stdout at the end and checks
`ferror`: output lost to a full disk, a closed pipe or `/dev/full` prints `srdump: writing standard output failed`
on stderr and turns an exit 0 into 1. Inputs: a FILE capture, a bare frame stream (needs `--dict`/`--elf`), an
`.rri`, an ELF executable/shared object/object file (`--elf`). `--elf` goes through
`tools::extract_embedded_dictionary`, so a file that is not ELF is reported as `unsupported`, and an ELF binary without an `es_serreg_dict` section — stripped of it, or built with
`ES_SERREG_IMAGE_FORMAT=none` — as `not_found`; use the program's `.rri` (`ES_SERREG_DUMP_REGISTRY`,
`export_image`) with `--dict` instead. `--diff` operands are still recognised by magic in `cli.cpp` (RRI, ELF, else
`bad_magic`). `--config` TOML subset: comments, `[tables]` (ignored; a `# comment` may follow the header),
`key = value` with strings, integers, booleans and arrays of strings (items separated by commas, D-09); keys are
the long flags without dashes
(`verify = true`, `json = true`, `format = "toml"`, `frames = "0..10"`, `type = "ex::Order"`,
`dict = ["a.rri"]`, `elf = "app"`, `hex = true`, `raw = true`, `diff = ["a", "b"]`, `cxx = "types.hpp"`,
`cxx_router = true`, `namespace = ["old=new"]`, `direction = "new-data"`, `window = "4M"` or `window = 4194304`,
`framelimit = "64M"`, `info = true`, `resync = true`, `level = "warn"`, `since = "2026-10-07"`,
`until = "…"`); explicit flags override the file.

Typical uses:

```sh
srdump orders.ser                       # dictionary, then every frame decoded (print frames rendered)
srdump orders.ser --verify              # ok: 11 frames, 34 records, 576 bytes (data frames; meta frames not counted)
srdump big.ser --info                   # header, dictionary blocks, checkpoints, writer info — by seeks
srdump big.ser --verify --resync        # read past a corrupt frame from the next checkpoint
srdump app.log.ser --level warn --since 2026-10-07T14:00   # log lines at warn and above, from 14:00 UTC
srdump orders.ser --json | jq …         # one JSON object per record line, {"frame":N,"message":…} for print calls
srdump --elf ./app --toml               # the schema of a binary, no execution
srdump --elf ./app --rri-out app.rri    # freeze it
srdump stream.bin --dict app.rri        # a bare stream with an out-of-band dictionary
srdump --diff old.rri ./new_app         # what changed; exit 1 if breaking for a new reader of old data
srdump --diff old.rri ./new_app --direction new-data   # … for an old reader of new data; both | none
srdump --program-hash orders.ser        # 0xd6bdb39e65bcecb7
srdump week.ser --verify                # read in 10 MiB windows — any size, bounded memory
srdump week.ser --window 64M --frame-limit 128M --json   # a wider window; frames up to 128 MiB accepted
srdump --wireshark serreg.lua orders.ser   # a Wireshark dissector for these types (§10.4)
srdump --cxx types.hpp --elf ./app      # the types as C++, self-checking (§10.4b); --cxx-router adds handlers
```

Text output shape (from the golden fixture):

```
dictionary  program_hash 0xd6bdb39e65bcecb7  (3 types, 8 literals, 9 calls)
  …
struct corpus::Cancel  schema 0x35af6843b770c519  layout 0x99c125fd4fe8673c  size 16  align 8  flat
  id: u64 @0 [8,8]
  …
call 0x0ef1cf0a2766bb0e  (lit, const i32=0x000000000000002a, corpus::Order, const corpus::Side=0x0000000000000002, const f64=0xbff8000000000000)
  …
print 0xe97f49911b00565e  (lit, string, i32, lit)

frame 1  call 0xbcf8e25bde66fdb2  (2 records)
  [0] "new order"
  [1] corpus::Order {
        id: 42
        …
      }
frame 6  print 0xe97f49911b00565e  (4 records)
  print: hello new 100 worlds
frame 9  call 0x0ef1cf0a2766bb0e  (5 records)
  [0] "fill"
  [1] i32 = 42
  [2] corpus::Order {
        …
      }
  [3] corpus::Side::sell (2)
  [4] f64 = -1.5
frame 11  println 0x9557b837cf09db95  (3 records)
  print: limit=5000000000 on=true
```

Frame 0 is the writer-info frame, a meta frame: it counts in the numbering but is not listed. Frames 9 and 11 were
written with compile-time constants (format 1.5):
`serialize(f, "fill"_, 42_, order, k<Side::sell>, -1.5_)` and
`println(f, "limit={} on={}"_, 5'000'000'000_, k<true>)`. Frame 9 is 48 bytes — its header and the
`Order` — and frame 11 is its 16-byte header. In the dictionary a constant argument is listed as
`const <type>=<the stored u64>`; in a frame it is shown as the scalar or enum record it stands for, with the value
taken from the call's dictionary entry. `--json` and `--toml` give such an argument `"kind": "const"` and (B7) its
value typed as what it stands for — `"value": 42`, `"value": "sell"`, `"value": -1.5`, `"value": true` — with the
exact bits beside it, `"bits": "0x000000000000002a"` (chapter 6 §6.4). The text form keeps the
`const <type>=<u64>` spelling. `--verify` reports a full-records frame whose constant differs from the value in
its call's entry.

### 10.1c Maps, sets and container arguments in the dumps (format 1.6)

From the golden `dynamic.text` (frames 5–9):

```
struct std::vector<i32>  schema 0x02c87ef3a015eb45  layout 0x03d4f3aff8e167eb  size 8  align 4  dynamic  transparent
  : vector<i32> @0 [8,4]
enum corpus::Side : u8  schema 0x074e267cca5188e1  layout 0xfdccdf0b5609277b  size 1  align 1
struct corpus::DynMap  schema 0x1aa730519093de87  layout 0x5e48ff513e467a38  size 36  align 4  dynamic
  id: u32 @0 [4,4]
  by_name: map<string,i32> @4 [8,4]
  ids: set<u16> @12 [8,4]
  names: map<i32,string> @20 [8,4]
  msgs: map<u8,struct<corpus::DynMsg>> @28 [8,4]
  …
frame 5  call 0xcd9f5445da365471  (2 records)
  [0] "map"
  [1] corpus::DynMap {
        id: 9
        by_name: {"a": 1, "b": 2, "c": 3}
        ids: {10, 20, 30}
        names: {-1: "minus one", 4: "four", 5: "five"}
        msgs: {1: corpus::DynMsg{id: 1, name: "one", xs: [1, 1]}, 2: corpus::DynMsg{id: 2, name: "two", xs: [2]}}
      }
frame 6  call 0x9ed3aef1a696894d  (2 records)
  [0] "ids"
  [1] [3, 1, 2]
frame 7  call 0x9ed3aef1a696894d  (2 records)
  [0] "ids"
  [1] []
frame 9  println 0x7bc19f076918cb8d  (3 records)
  print: ids=[3, 1, 2] map=corpus::DynMap{id: 9, by_name: {"a": 1, "b": 2, "c": 3}, ids: {10, 20, 30}, names: {-1: "minus one", 4: "four", 5: "five"}, msgs: {1: corpus::DynMsg{id: 1, name: "one", xs: [1, 1]}, 2: corpus::DynMsg{id: 2, name: "two", xs: [2]}}}
```

In the dictionary a map or set member shows its *shape* (`map<string,i32>`, `set<u16>` — a `std::unordered_map`
member looks the same, chapter 6 §6.2), a container argument's type is a `struct` named after the container with
one unnamed field and the marker `transparent` after `dynamic`, and an enum without a fixed underlying type gets
`unfixed` after its line. In a frame a map renders as `{key: value, …}` and a set as `{a, b, …}` in the entries'
wire (= canonical) order, with the same `array_limit` elision as arrays (`… (+N more)`, `--full` to lift it); a
container argument's record is the value alone (`[1] [3, 1, 2]`, `[1] []`), and a print argument renders the same
way. `--json` gives a map as an array of entries — `"by_name":[{"key":"a","value":1},{"key":"b","value":2}]` —
because a key need not be a string, a set as a plain array, and a container argument as the bare value:
`{"frame":6,"record":1,"type":"std::vector<i32>","value":[3,1,2]}`; the dictionary part has `"transparent": true`
on the vector's type and `"fixed": true|false` on every enum (`--toml`: `transparent = true`, `fixed = …`).
`--verify` walks the entries like vector elements under the same slot rules; it does not check that keys are
sorted or unique (that is the typed decoder's `invalid_value`). `--wireshark` and `--cxx` follow in §10.4 and
§10.4b.

### 10.1d Format 1.7 files: `--info`, `--resync`, CRC trailers, meta frames

srdump reads format 1.6 and 1.7 files. Meta frames — dictionary frames (full or 1.7 deltas), checkpoint frames and
the writer-info frame — are consumed and not listed by default: they count in the frame numbering (`--frames`, the
`frame N` headers), so frame numbers match the file, but not in `--verify`'s frame and byte totals. `--verbose`
shows the dictionary's history: after the merged dictionary, one line per block — the opening block and every
dictionary frame, with its offset and what it added (`+0 types, +1 literals, +0 calls`) — and, in the frame dump,
the same line as a marker where the dictionary frame sits (`frame 2  dictionary frame  @35408  +0 types, …`).
With `--dict-only --verbose` the blocks alone; with `--json` one object per block
(`{"frame":2,"offset":35408,"dictionary":{"block":"frame","types":0,"literals":1,"calls":0}}`; the opening
block has `"frame":null`). The piped and the windowed readers print the same (`tools::dump_options::verbose`;
over a file, `block_options::meta_frames` makes the windowed walker hand the meta frames to the callback,
`frame_ref::kind` says which). A frame whose
CRC32C trailer does not match is reported (`frame N @off: crc mismatch (frame skipped)`) and skipped, never decoded;
`--verify` then exits 1.

`--info` describes a file without walking its frames (`tools::info` + `to_text`/`to_json`): the header (minor,
flags, program hash, ABI, size), the dictionary blocks — the opening block and every delta the forward chain
reaches, with the merged entry counts —, every checkpoint of the checkpoint chain (offset, data frames and bytes
before it, wall clock, latest dictionary, whether it carries a full dictionary) and the writer-info items (library
version, format, program, host, pid, start time, the options the file was written with). It needs a seekable file
(a pipe is a usage error) and costs one read per link, so it answers instantly on a multi-terabyte capture. A
chain link it cannot follow — one that points past the end of the file, does not go forward, or lands on a frame
of another kind — ends the chain **and is reported** (`warning     dictionary chain: link @1234 -> 99999 points
past the end of the file (7144 bytes)`; JSON `"warnings":[…]`, empty for an intact file); `--verify` then says
what the frame walk finds. The Python reader (`capture_info`) reports the same warnings. `--json`
gives the same as one object (`golden_srdump_info_features` runs it on `features.ser`).

```
$ srdump tests/fixtures/golden/features.ser --info
format 1.7  program_hash 0xd6bdb39e65bcecb7  size 7144  flags crc checkpoints info
dictionary  3 types, 8 literals, 9 calls; 1 block(s) at 48
checkpoints 3
  @2008  frames 5  bytes 280  dictionary @48 (full copy)  wall 1700000000000000000
  @3952  frames 10  bytes 640  dictionary @48 (full copy)  wall 1700000000000000000
  @5560  frames 11  bytes 664  dictionary @48 (full copy)  wall 1700000000000000000
writer      library = 2.2.2
writer      format = 1.7
writer      program = make_fixtures
writer      abi = ptr8 long8 wchar4 char-signed max-align16 ldbl16 le
writer      crc = 1
writer      checkpoint_bytes = 256
writer      checkpoint_ns = 0
writer      dictionary = full
writer      sync = dictionary
$ srdump tests/fixtures/golden/features.ser --verify
ok: 11 frames, 34 records, 664 bytes
```

`--resync` changes what happens at a frame header that cannot be parsed: instead of ending the walk with `bad
frame_len`, the reader scans forward for the next checkpoint frame and continues there, reporting the skipped
range as one problem (`--verify` still exits 1). It works for files read in windows (any regular file); a piped
dump is read whole into memory and is not resynchronised. A file written without checkpoints has nothing to
resynchronise to.

### 10.1e Log frames: `--level`, `--since`, `--until`

A capture written by a `logger` (chapter 3 §3.9) holds log frames: print frames whose call carries the `log`
flag and an envelope. Without options srdump shows each as a frame header `frame N  log 0x… (k records)` and the
line `  log: <time> <LEVEL> <file>:<line> [tid] #seq <message>`. With **`--level <lvl>`** only log frames at or
above that level are printed, as bare lines — no dictionary, no frame headers, no other frames:

```
2026-10-07 14:03:11.123456789 WARN  main.cpp:42 book AAPL depth 7
2026-10-07 14:03:11.123501002 ERROR main.cpp:57 [4711] #19 reject 42: px out of band
```

The time is UTC with nanoseconds for `clock_policy::system`, the raw number for steady, TSC or user clocks; the
level is padded to five; the file is shown by its base name and omitted for the member functions' `"-"`; `[tid]`
and `#seq` appear when the logger's policy writes them. **`--since <t>`** and **`--until <t>`** keep log frames
whose timestamp lies in the range (inclusive); `<t>` is a number in the writer's clock units (ns since the epoch for
the system clock) or a UTC date-time `YYYY-MM-DD[THH:MM[:SS[.fff]]]`. A time range on its own implies `--level
trace`. With `--json` each log frame is one object:

```
{"frame":12,"log":{"level":"warn","time":1759845791123456789,"file":"src/main.cpp","line":42,"message":"book AAPL depth 7"}}
```

(`"thread"` and `"sequence"` when present; `file` as written by the compiler). With a steady, TSC or user clock the
time is the raw number — the capture of `tests/python/log_capture.cpp` (`python_log_capture`) renders as

```
1700000000000002000 INFO  log_capture.cpp:70 started with 4 workers
1700000000000003000 WARN  log_capture.cpp:71 book AAPL depth    7
1700000000000004000 ERROR log_capture.cpp:72 [1568] #1 px=1.25 order=42 venue=nasdaq
1700000000000006000 TRACE no call site 0xff lit
``` The dictionary's text form labels a
log call `log …`, JSON and TOML `"form": "log"`. Library: `dump_options::logs_only`, `min_level`, `since`,
`until`; for a program, `frame_router::on_log` (chapter 4 §4.6d).

### 10.1a Captures of any size: `--window`, `--frame-limit` (A6; FR-D5, FR-D11)

Every `srdump` path over a capture — `--verify`, the text/JSON/TOML dumps, `--frames`, `--type`,
`--program-hash` — reads the file in windows of `--window` bytes (`decode_limits::window_bytes`, 10 MiB, unless
set; 4 KiB floor; `K`/`M`/`G` suffixes are powers of two). A frame that straddles a window is completed from
the next, a frame larger than the window grows it to that frame within the frame limit, and `verify`/`dump`
keep state O(types), never O(frames): a capture of a trading week is bounded by the file system, not by the
tool. The dump reads a regular file twice (the dictionary printed first is "as of the end"), so a dump of
standard input or a pipe is the one path that still reads its input whole (within `decode_limits::input_bytes`,
1 GiB); `--verify` on a pipe streams. `--frame-limit <size>` sets `decode_limits::frame_bytes` for the run (64 MiB
by default, 16-byte floor): a `frame_len` above it is a `bad frame_len` problem and the walk stops — unless
`--resync` finds a checkpoint to continue at (§10.1d). A walk keeps at most 1000 problem lines (`decode_limits::problems`); `--verify` prints
`problem: ... and N more` and counts them in `FAILED (P problems)`, `--json` adds `"problems_omitted":N`. The
`golden_srdump_*` ctests run the windowed path against the fixtures; "2.1.16 A6 srdump reads a capture in
windows: --window and --frame-limit" (`tests/unit/test_limits.cpp`) checks the flags and their exit codes (a
window below 4096 or an unparsable size is exit 2). Chapter 4 §4.6a lists the limits.

### 10.1b `--diff --direction` (decision E)

```
$ srdump --diff old.rri new.rri
identical       corpus::Order  0x9a3c… -> 0x9a3c…
removed         d::Cancel  0x5b8a… -> -  breaking: removed: a reader built from the new side cannot read old frames of this type typed
call added      call 0x4d10…  - -> 0x4d10…
3 types: 1 identical, 0 names-only, 0 metadata-only, 0 breaking, 0 added, 1 removed, 0 duplicate-name; 1 calls: 1 added, 0 removed, 0 changed; 0 literals: 0 added, 0 removed, 0 changed  BREAKING (direction old-data: 0 changed, 1 removed, 0 added)
```

`--diff` reports **identity**: did a type's bytes or names move, is a call or a literal still there — the seven
type verdicts (`identical | names-only | metadata-only | breaking | added | removed | duplicate-name`) and the
six call/literal ones. It does not report *decodability* (whether a given reader could still walk a given frame).
`--direction` answers the question "breaking for whom?": a removal breaks a reader built from the new side that replays old captures, an addition
breaks an old reader on a live stream of the new side, and only the user knows which deployment is meant. So
`--direction` names it: `old-data` (the default) makes removals breaking, `new-data` makes additions breaking,
`both` marks both, `none` neither. A layout change, a changed call and a changed literal
are breaking in every direction. A breaking entry carries `breaking: <why>`, the summary line says
`BREAKING (direction old-data: C changed, R removed, A added)` (or `(direction …)` when nothing breaks), and the
exit status follows. Config key `direction`; the library form is `tools::diff(a, b, direction)` with
`diff_entry::breaking`/`note`, `diff_report::direction` and the three counts (`files/src-diff.cpp.md`).

## 10.2 `es::serreg::cli` as a library

The CLI, the Wireshark generator, the C++ generator and the dictionary diff are compiled into `libserreg_tools`
(decision G): link `es::serreg_tools` (CMake, component `tools`) or `serreg-tools` (pkg-config) to call
`cli::run`, `tools::diff` or `tools::wireshark_dissector`. It links `es::serreg` PUBLIC; a program that only
serialises and reads needs `es::serreg` alone (`files/build-system.md`).

```cpp
struct cli::options { std::string capture, rri, rri_out, diff_a, diff_b, frames, type, wireshark, cxx;
                      tools::diff_direction direction = old_data; std::size_t window_bytes; std::uint64_t frame_limit;
                      std::vector<std::string> dicts, elfs, namespaces;
                      bool use_stdin, dict_only, verify, hex, raw, flatten, full, help, version, program_hash, cxx_router;
                      std::string level, since, until; bool info, resync; tools::format fmt; };
struct cli::app { std::string_view name = "srdump"; std::string_view version = tools::library_version;
                  std::string_view extra_usage; std::function<bool(std::string_view arg, std::span<char* const>& rest)> on_extra; };
int cli::parse(int argc, char** argv, cli::options&, const cli::app& = {});   // --config first, then flags; 2 on usage error
int cli::execute(const cli::options&, const cli::app& = {});                  // help/version/diff/dictionary/verify/dump
int cli::run(int argc, char** argv, const cli::app& = {});                    // parse + execute
std::string cli::usage(const cli::app& = {});
```

`on_extra` receives every argument `cli` does not own (an unknown `--flag`, a second operand) with the
remaining argv, may consume values (`rest = rest.subspan(1)`), and returns `true` when handled. Usage and
diagnostics carry `app.name`. `embedded_cli --trace --selftest out.ser --verify out.ser` is the example.

## 10.3 `es::serreg::tools`

| function | purpose |
|---|---|
| `read_file(path)` / `write_file(path, bytes)` / `write_all(fd, bytes)` | files, pipes, `-` = stdin; inputs above `decode_limits::input_bytes` (1 GiB) refused (`src/file_io.cpp`) — for RRI, ELF and config inputs; captures go through the windowed readers |
| `load_dictionary_rri(bytes)` / `load_dictionary_file(path)` | RRI → `dyn::dictionary` |
| `extract_embedded_dictionary(path)` / `extract_embedded_dictionary(bytes)` | the dictionary embedded in a program image. Format by magic: ELF → the `es_serreg_dict` section → `dyn::dictionary`; any other format → `unsupported`; fewer than 4 bytes → `truncated`; ELF without the section → `not_found` |
| `extract_registry_from_elf(path)` | the same as `extract_embedded_dictionary(path)`; the older name, kept for existing callers |
| `extract_section_from_elf(path, name)` / `extract_section_from_image(bytes, name)` | any section of an ELF file or image; bounds- and overflow-checked |
| `verify(capture, extra = nullptr)` / `verify(path, extra = nullptr, block_options = {})` → `verify_report{frames, records, bytes, program_hash, problems, problems_omitted, warnings}` | structural + decode check of a capture or bare stream; over a path, read in windows; CRC trailers checked, meta frames consumed and not counted, `block_options::resync` |
| `info(path)` → `capture_info`, `to_text(info)`, `to_json(info)` | what a FILE says about itself — header, dictionary chain (merged), checkpoints, writer info — by seeks, no frame walk (`srdump --info`) |
| `for_each_frame(capture, f, extra)` / `for_each_frame_in_file(path, f, block_options{block_bytes, limits}, extra)` | every data frame with the dictionary as known at that frame, over a span or read in windows (`decode_limits::window_bytes` unless `block_bytes` says otherwise; per-call `limits`) |
| `for_each_frame(path, f, file_options{blocks = true, block_bytes, populate, limits}, extra)` | a capture on disk in one call — in windows by default; `{.blocks = false}` maps the whole file (`mapped_file`); `"-"`, pipes and devices are always windows |
| `walk_values<Ts...>(capture \| path, on_value, rest, on_frame, [file_options,] extra)` | every value, typed where `Ts` match, the rest through a `decoded_value_visitor` (`walk.hpp`) |
| `walk_values(capture \| path, visitor, on_frame, [file_options,] extra)` | the same without compiled types (a dictionary-only reader) |
| `project<T>(writer, dict, bytes, report = nullptr)` → `result<T>`, `projection_report` | schema evolution level 1: a flat `T` filled by member name from the bytes of a writer value of another version of the type (`projection.hpp`, chapter 4 §4.1d) |
| `dump(capture, extra, dump_options, out)` / `dump(path, extra, dump_options, out, block_options = {})` / streaming overloads with `write_fn` | text / JSON / TOML rendering, frame range, type filter, `--hex`, `--raw`, print rendering; over a path, two windowed passes |
| `dump_dictionary(dict, format, out)` | dictionary only |
| `visit_record(payload, type, dict, visitor, field_name, flatten)` | the descriptor-driven decoder (style 4), including the `begin_map`/`begin_entry`/`end_entry`/`end_map`, `begin_set`/`end_set` and `begin_transparent`/`end_transparent` events, all defaulted |
| `render_frame(frame, dict)` | the rendered message of a print frame (`"\n"` appended for `println`) |
| `render_value(dict, ak, kind, schema, payload, format)` / `process_dictionary()` | one value rendered from a dictionary as srdump renders it; the process's own dictionary, parsed once per registry version — what `to_string`/`to_json` (`to_string.hpp`) use |
| `value_text(kind, bytes)` | one scalar as srdump's text shows it (decimal integers, shortest round-trip floating point, `true`/`false`, a quoted `char`, `0xNN` for `byte`) — for a visitor's `scalar` callback |
| `diff(a, b, direction = old_data)` → `diff_report` (`entries`, `breaking`, `direction`, the three breaking counts, `to_text()`) | schema comparison; `diff_direction::{none, old_data, new_data, both}` decides whether removals or additions are breaking (§10.1b) |
| `get_decode_limits()` / `set_decode_limits(l)` (`es::serreg`, `limits.hpp`) | the process-wide reader limits — frame 64 MiB, dictionary 256 MiB / 1 000 000 entries, image depth 64, whole-input 1 GiB, window 10 MiB, 1000 problem lines; per call through `block_options::limits` / `file_options::limits` (chapter 4 §4.6a) |
| `wireshark_dissector(dict, wireshark_options{generator, sources})` | a Wireshark Lua dissector for the types, calls and literals of `dict` (§10.4, `src/wireshark.cpp`) |
| `cxx_header(dict, cxx_options{router, namespaces, include})` | C++ definitions of every type of `dict`, with static_asserts for every size, offset, name and hash; `router` adds a `frame_router` skeleton (§10.4b, `src/cxxgen.cpp`) |
| `library_version` | the release version string (`es::serreg::library_version` in `version.hpp`), printed by `srdump --version` and written into the writer-info frame |

`dump_options{fmt, dict_only, hex, flatten, full_arrays, array_limit = 16, first_frame, last_frame,
type_filter, type_name_filter, raw, logs_only, min_level, since, until}` (§10.1e for the last four).

## 10.4 Wireshark dissectors: `srdump --wireshark`

```sh
srdump --wireshark serreg.lua capture.ser                        # the capture's own dictionary
srdump --wireshark serreg.lua --elf ./app --elf ./libplugin.so   # the programs' embedded dictionaries
srdump --wireshark serreg.lua --dict app.rri                     # an RRI (export_image, ES_SERREG_DUMP_REGISTRY)
srdump --wireshark - capture.ser > serreg.lua                    # to standard output
myapp --wireshark serreg.lua                                     # an app embedding cli::run, its own registry
tshark -X lua_script:serreg.lua -r capture.ser -Y 'serreg.corpus_Order.qty >= 100'
```

`--wireshark <file.lua>` writes one Lua file: a Wireshark dissector for the types, calls and literals of a
dictionary. Wireshark then opens `.ser` captures directly (packet 1 is the FILE header and dictionary, then one
packet per frame), and also decodes pcap/pcapng files with link type USER0, and UDP datagrams and TCP streams of
frames — on the ports set in the preferences, and without any port setting (below). Every value is a
filter field named `serreg.<type>.<member>` (`serreg.corpus_Order.qty`); print frames show their rendered message
(`serreg.message`), and every frame its signature as `serreg.call.name`. The file is generated tables followed by a
fixed Lua runtime that applies the same slot rules as `srdump` (`files/src-wireshark.cpp.md`). A map member (format
1.6) is a `member: map[n]` subtree
of `[i]` entries, each with a `key` and a `value` item that are filter fields of their own —
`serreg.corpus_DynMap.by_name.key == "b"`, `serreg.corpus_DynMap.names.value contains "four"` — and the member's
text is `{k: v, …}`; a set is `member: set[n]` with its keys as elements (`{a, b}`); a container argument's record
is shown as its value under the argument's name (`ids: [3, 1, 2]`), with no struct around it. The parity test
runs the dissector over the golden `dynamic.ser` against srdump.

For format 1.7 the FILE reader skips the CHAIN after the opening dictionary; dictionary deltas are
compared with the generated tables like any dictionary frame; checkpoint and writer-info frames are shown as such
(`checkpoint` with its prev/next offsets, counts, wall clock and latest dictionary; `writer info (N items)` with
one line per item); a CRC trailer is shown (`crc32c 0x…`) and kept out of the records, but not recomputed; a log
frame's message carries the prefix `LEVEL file:line [tid] #seq` (the timestamp is a record of its own in the tree);
and the UDP/TCP heuristics also recognise the checkpoint and writer-info sentinels. The parity tests run on the
1.7 golden files; the `backpatched` check uses the 1.6 corpus copy of `orders.ser`.

**Sources.** The dictionary is the merge of: the capture's own dictionary (the operand, or `--stdin`), read
through `tools::for_each_frame` over the whole capture (T-08: the FILE's dictionary and every dictionary frame,
also those that follow data frames, e.g. after a `dlopen`); every `--dict`; every `--elf`; and `--rri`. They can be
combined, and all are listed in the file's header comment. For `srdump` at least one is required (otherwise exit
2, `--wireshark needs a capture, --dict, --elf or --rri`). An application that embeds the command line
(`cli::run`, `examples/embedded_cli.cpp`) and is given no source writes the dissector of its own registry: the
dictionary is `load_dictionary_rri(registry::instance().export_image())`, used when it has types or calls, and the
header comment names `the registry of <app name>` (ctest `example_embedded_cli_wireshark`). srdump registers no
types, so it needs a source. Several versions of a program can share one dissector:
two versions of a type have different hashes, so generate from all the binaries at once.

**Conflicts.** Two sources that define one schema hash with different layouts are refused, as for the
dictionary-only dump: one `schema conflict: <type> <hash>` line per conflict on stderr, exit 1, nothing written.
A merged dictionary (T-08) without types, calls or literals (e.g. a bare frame stream without a
dictionary frame) is an error too: `--wireshark: the sources hold no dictionary entries`, exit 1, nothing written.

**Output.** With a file name, `srdump` writes it and prints `wrote <file>: N types, N call signatures, N literals`
on stderr; with `-` the Lua goes to stdout. The dissector is a snapshot of the dictionary: it decodes frames only
from its own tables, matched by the XXH3-64 ids on the wire, and compares a capture's embedded dictionary with them
(known/unknown entries, layout mismatches, program hash, ABI) instead of using it. A frame it does not know is shown
as bytes with an expert info. Regenerate it when the program's types change. `--wireshark` is handled before any
dump, verify or program-hash output and has no `--config` key.

**Print messages.** `print`/`println` frames are rendered as `std::format` renders them: argument ids
(`{1} {0}`), fill and alignment, sign, `#`, `0`, width, precision, the integer bases `d x X o b B` (64-bit values
through Wireshark's `UInt64`/`Int64`), the floating-point types `a A e E f F g G` (`{}` is the shortest round
trip, as in srdump), and `bool`/`char` as text or as numbers. Nested replacement fields (`{:{}}`) are not rendered;
such a field is shown as written; `print` rejects them at compile time (T-05), so only a foreign writer
can produce one. An 8-byte enum value is shown with its exact decimal digits through `Int64`/`UInt64` (T-02), also
above 2^53. The parity test requires the message of every frame of `tests/wireshark/print_capture.cpp` (15 lines of
format specs, among them a line of `float` arguments and a 64-bit enum value above 2^53) to equal srdump's.

**Float arguments.** srdump formats an `f32` print argument as a `float`, not a `double` (T-04): `"{}"` of `0.1f`
is `0.1`, as the writer's `std::format` would print it, not `0.10000000149011612`. The parity test's `prints:`
check compares that line with the dissector's rendering.

**Heuristics.** The dissector registers heuristic dissectors for UDP and TCP: a datagram, or the start of a
TCP stream, whose first 16 bytes are a frame header (`frame_len` ≥ 16 and a multiple of 8, known flags) with a call
hash from the dissector's tables — or a dictionary frame — is taken as es::serreg without a port setting. On TCP the
rest of the conversation then goes to the stream dissector, which reassembles frames across segments. A 64-bit call
hash leaves no practical room for false positives; the heuristics can be disabled in Analyze → Enabled Protocols.

**`serreg.call.name`.** Each frame whose call hash is in the tables gets its signature as a filterable
string, generated per call: `("new order", corpus::Order)`, `println("px={:.2f} qty={}", f64, u32)` (filter
`serreg.call.name contains "Order"`).

From C++: `std::string tools::wireshark_dissector(const dyn::dictionary&, const tools::wireshark_options&)`.
Installing the plugin, every filter field, the expert infos, the differences from srdump and the tested Wireshark
versions (4.2.2 with Lua 5.2, 4.6.8 with Lua 5.4) are in `docs/wireshark.md`. Tests: `wireshark_generate` and
`wireshark_dissector_parity` (label `wireshark`; needs tshark and Python 3; with the checks `prints:`,
`udp-heuristic` and `tcp-heuristic`), `example_embedded_cli_wireshark`, and
`wireshark_dissector_lua52` / `wireshark_dissector_lua54`: the FILE-capture checks with the dissector under plain
Lua 5.2 and 5.4 through `tests/wireshark/wslua_stub.lua`, no Wireshark needed. Under Lua 5.4 a float's text is
`7.0`, so the runtime keeps octal and binary digits and 8-byte enum values integral.

## 10.4b Generating C++ from a dictionary: `srdump --cxx`

```sh
srdump --cxx types.hpp --elf ./app                    # the program's embedded dictionary
srdump --cxx types.hpp capture.ser                    # a capture's dictionary (every dictionary frame of it)
srdump --cxx types.hpp --rri app.rri --cxx-router     # plus install_handlers(frame_router&), one handler per call
srdump --cxx - --dict app.rri --namespace app=replay  # the types declared in namespace replay, to standard output
```

`--cxx <file.hpp>` (`tools::cxx_header`, `src/cxxgen.cpp`) writes one header that declares every type of the
dictionary — the same sources and the same refusals as `--wireshark` (§10.4) — so that a program compiled with
it has exactly the types the writer had: enums as `enum class E : <underlying>` with every enumerator, structs with
their members in order (fixed-width scalars from the kinds, `std::array` for arrays, `std::string` /
`std::vector` / `std::optional` / `std::variant` for dynamic members, `std::map<K, V>` for a `map`
member and `std::set<K>` for a `set` member, bases from the `<base:…>` pseudo-fields,
`[[=since]]`/`[[=deprecated]]` carried), in dependency order inside their namespaces. The dictionary records a
map member's *shape*, not its container, so an `std::unordered_map` member comes back as `std::map` — with the
same schema and layout hashes, so the header's self-checks pass and the regenerated type decodes the same frames
(`docs/deviations.md` "Containers"). A container passed as an argument is a type with a `std::` name and is
spelled back from its recorded vocabulary name, never defined (`std::unordered_map<std::int32_t, std::string>`
with `<unordered_map>` included), like the rest of the std vocabulary. Where a flat type's recorded
offsets leave a gap the natural layout would not (a `[[=skip]]`ped member of the original, a non-standard
alignment), a `[[=es::serreg::skip]] std::byte _skipN[k];` member fills it: a skipped member is absent from the
canonical strings and scrubbed on the wire, exactly what a gap is; `alignas` is added when the recorded alignment
exceeds every member's. A name a plain declaration cannot spell — a template specialisation (`corpus::Vec<i32,3>`),
a class nested in a class, a type moved by `--namespace` — is declared under a sanitized identifier
(`Vec_i32_3`) with `[[=es::serreg::name("corpus::Vec<i32,3>")]]`, which makes the library use the recorded
canonical name. The std vocabulary (`std::string`, `std::monostate`, `std::pair<…>`, …) is spelled back, never
defined.

**The output checks itself.** After the declarations the header holds, for every type, `static_assert`s of
`sizeof`/`alignof` (flat types; the image's fixed part through `descriptor_v<T>` for dynamic ones), of `offsetof`
for every member of a flat type without bases, of `canonical_name_v<T>`, and of `schema_hash_v<T>` and
`layout_hash_v<T>` against the dictionary's values. Compiling the header is the proof that the regeneration is
exact; a layout the library cannot reproduce (a packed struct, a foreign ABI) is named in a `// NOTE:` line at the
top and fails its assert. `--cxx-router` appends `inline void install_handlers(es::serreg::frame_router&)` with one
`r.on(…)` / `r.on_print(…)` / `r.on_println(…)` per call signature — a literal first and values after it as
`on("lit"_, h)`, anything else (constants, a literal in the middle) as `on<lit<"…">, constant<42>, T…>(h)` — whose
bodies are `/* TODO */`. `--namespace old=new` (repeatable) declares the types of namespace `old` under `new`; the
hashes do not move because the name annotation keeps the canonical name. A merged dictionary with two types of one
name (two programs) is refused: `--cxx: duplicate type name <name>; pick one source (--program-hash) or --dict`.
Tests (`tests/cxxgen/`, label `cxxgen`): `cxxgen_golden_compiles` and `cxxgen_golden_dynamic_compiles` generate from
the golden `orders.rri` / `dynamic.ser` and compile the output with the project compiler and warnings;
`cxxgen_golden_namespace_compiles` compiles a `--namespace corpus=regen` header beside `corpus.hpp` and asserts
equal hashes; `cxxgen_golden_roundtrip` / `cxxgen_golden_dynamic_roundtrip` link the generated types into a program
that routes the golden captures through them and compares the values (all 9 data frames of `dynamic.ser`,
`DynMap`'s containers and the top-level vectors included; the generated TU is compiled and linked in two steps
since GCC 16.2 miscompiles the constant evaluation when `-fsanitize` appears twice on one command line).

## 10.5 `file(1)` support

`share/serreg.magic` (installed to `share/serreg/serreg.magic`) teaches `file` the two magics:

```
$ file -m share/serreg.magic orders.ser orders.rri
orders.ser: es::serreg capture (FILE container), format 1.7, XXH3-64 hashes, abi ptr8/long8/wchar4, writer info, program hash 0xd6bdb39e65bcecb7
orders.rri: es::serreg registry image (RRI), format 1.7, 20 entries, program hash 0xd6bdb39e65bcecb7, XXH3-64 hashes, abi ptr8/long8/wchar4
```

The FILE flags are shown after the ABI: `, crc`, `, checkpoints` and `, writer info` for a 1.7 file
(`features.ser` shows all three), `, dictionary back-patched (1.6)` for a back-patched 1.6 file; an RRI delta adds
`, delta`. `srdump --info` shows the rest.

Append it to `/etc/magic` or `~/.magic` to make it permanent.

## 10.6 Design rules of the tools layer

- `srdump` never does anything `tools::` cannot; new CLI features start as a `tools::` function with a test.
- Every reader is bounds-checked before it allocates; counts on the wire are checked against remaining bytes
  (`count_fits`), depth is limited, arithmetic that could overflow is done in 64 bits or refused.
- Problems, not exceptions: a malformed record is a `problems[]` entry and a non-zero exit; the walk continues to
  the next frame when it can (`frame_len` always allows skipping).
- Text and JSON renderings are frozen by the fixtures (`golden_srdump_*_matches_fixture`) — changing a single
  character of the output is a deliberate act with a CHANGELOG line.

## 10.7 `to_string` and `to_json` in a program

The renderers behind `srdump` are also available for single values inside a program: `to_string(v)` gives the
one-line text a print frame shows for an argument, `to_json(v)` the dump's JSON value, `std::format("{}", v)` both
for a type marked `formattable` (`include/es/serreg/to_string.hpp`, chapter 3 §3.8). They go through
`tools::render_value` over `tools::process_dictionary()`, so a value and a captured frame of it render alike.

## 10.8 The pure-Python reader: `python3 -m serreg`

`python/serreg` is a second, independent reader in Python (standard library only, Python ≥ 3.10): it reads 1.6 and
1.7 FILE captures, bare streams with `--dict x.rri`, and RRI images, and prints what `srdump` prints:

```sh
python3 -m serreg orders.ser                  # dictionary, then every record (text)
python3 -m serreg orders.ser --json           # srdump --json's lines
python3 -m serreg orders.ser --verify         # ok: 11 frames, 34 records, …; exit 1 on problems
python3 -m serreg features.ser --info         # header, dictionary chain, checkpoints, writer info
python3 -m serreg app.log.ser --level warn    # log lines
python3 -m serreg --rri orders.rri            # a dictionary on its own
```

It covers CRC trailers, deltas, checkpoints, writer info, compact and full records, every dynamic shape, maps,
sets, transparent types and print/log rendering with a `std::format` subset, and never raises on corrupt input. It
does not recompute XXH3, read ELF sections, diff, resync or generate code. The ctest `python_reader_parity`
compares it with `srdump --json` on every golden and corpus capture and a generated log capture. Its own page:
`book/python-serreg.md`.

## 10.9 The Compiler Explorer examples are tests

The eight `ce-examples/` programs — the FR-H4 cross-check of names, hashes, blobs and frames on any compiler
Compiler Explorer offers — are compiled and run by every build (`ce_example1_builds_and_runs` …
`ce_example5_builds_and_runs`, label `ce`): each `main.cpp` is compiled with only `-I.`, so its own header
symlinks into `include/` are what is tested, with `-O1 -W -Wall -Wextra -Wshadow -Werror`, and its output must
report no golden mismatch. A header that gains an `#include` needs the matching symlink in every example that uses
it, and this test is where a missing one shows first. Page: `book/ce-examples-tests.md`.
