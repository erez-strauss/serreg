# `src/reader.cpp`

The capture walker shared by every tool that reads frames (spec §6.5; FR-W1/W2/W6/W7, FR-D5, FR-D11). It implements
`internal::for_each_record()`, `internal::for_each_record_in_file()`, `internal::add_problem()`
and `internal::decode_problem()` (declared in `src/internal.hpp`), the process-wide `get_decode_limits()` /
`set_decode_limits()` of `limits.hpp`, and the public
`tools::verify()` (over a span and over a path), `tools::for_each_frame()` (over a span and over a path),
`tools::for_each_frame_in_file()`, `tools::mapped_file` and `tools::info()` with its `to_text`/`to_json`
(declared in `include/es/serreg/tools.hpp`). A capture is either a FILE (48-byte header, embedded RRI, since format
1.7 the 16-byte CHAIN, frames) or a bare frame stream; it reads formats 1.6 and 1.7. Dictionary frames — full
images or 1.7 deltas — are merged in the order they appear; 1.7 checkpoint and writer-info frames are consumed
(counted in the frame numbering, never delivered); a frame with a CRC32C trailer that does not match is reported
(`crc mismatch (frame skipped)`) and skipped; compact
frames are cut into records with the call signature from the calls table, full frames with their per-record
headers. The walker never trusts a length before checking it against the bytes that remain — a `frame_len`, a
`payload_len`, a string's `u32 len`, a dynamic image's `u32 len` — and never guesses at a newer minor or an
unknown frame flag: a newer minor stops the walk, a frame with an unknown flag is reported and skipped. The
frame header is read by one function (review G2-9), `classify_frame`, and the merged dictionary's
problems are listed by one function, `dictionary_problems`, both shared with the windowed reader
(`for_each_frame_in_file`), so the two walkers report the same things. That windowed reader (A6) is
the default under every path overload and under `srdump`: a capture is read `decode_limits::window_bytes`
(10 MiB) at a time, so its size is bounded by the file system, not by the tools (FR-D11). It decodes no record body itself; that is `visit_record()` in
`decoder.cpp`, which `verify` calls through a counting visitor.

## Depends on / used by

Includes: `internal.hpp` (`frame_info`, `record_info`, `walk_result`, `frame_fn`, `record_fn`, `record_source`,
`dec`, `hex64`),
`es/serreg/hash.hpp` (`fingerprint64("string")`), `es/serreg/tools.hpp` (`verify_report`, `decoded_value_visitor`,
`visit_record`, `block_options`; through it `limits.hpp`), `<mutex>` (the limits). Through `internal.hpp`: `dictionary.hpp` (`dyn::dictionary::add_rri`, `merge`, `find_call`,
`find_type`, `find_literal`, `record_size`, `conflicts`, `id_conflicts`, `abi_mixed`), `format.hpp` (`fmt::cursor`, header sizes,
flags, `string_len_prefix`, `image_prefix`, `align8`).

Used by: `src/formatters.cpp` (`tools::dump` calls `for_each_record` or, over a path, `for_each_record_in_file`
twice; `tools::render_frame` once),
`src/cli.cpp` (`tools::verify(path, …)` for `--verify` and `--program-hash`; `for_each_frame(span)` for
`--wireshark`/`--cxx`), `src/decoder.cpp`, `src/dictionary.cpp`, `src/file_io.cpp` (`get_decode_limits`),
`include/es/serreg/walk.hpp` (`walk_values` over `for_each_frame` / `for_each_frame_in_file` / `mapped_file`),
readers built on `frame_router` (`tools::for_each_frame(path, router.callback())`),
`examples/user_example1/reader1.cpp` (`for_each_frame(path, …, file_options)`), `tests/fuzz/fuzz_targets.cpp`
(`fuzz_capture` = `verify` + two `dump`s), `tests/unit/test_tools.cpp`, `test_capture.cpp`, `test_print.cpp`,
`test_dynamic.cpp`, `runtime_selftest.cpp`, and every `srdump … --verify` ctest.

## Structure

- **The process-wide limits** (`reader.cpp:23-41`): `get_decode_limits()` / `set_decode_limits()` over
  a `constinit decode_limits` behind a mutex (a reader takes one copy per walk, never per frame); and
  **`internal::add_problem()`** (`reader.cpp:45-52`): append a problem line while the list is below the limit,
  otherwise count it in `omitted`.
- **`rri_length()`** (`reader.cpp:59-78`): byte length of an RRI (header + `n_entries` entries) at a given
  position, used to skip the original dictionary area of a back-patched (1.6) FILE. Every entry (G2-10) must be
  a multiple of 8 bytes, as `add_entries` requires, so a corrupt original area cannot start the frames at an
  unaligned offset (`corrupt_dictionary`).
- **`classify_frame()`** (`reader.cpp:84-105`): reads a 16-byte frame header into a `frame_info` and
  classifies it: `ok`, `bad_len` (below 16, not a multiple of 8, or more than `room` bytes — the walk stops, or
  with `--resync` scans for the next checkpoint; 1.7: a CRC frame shorter than header + trailer), `unknown_flags`
  (reported and skipped). It sets `is_dict`, `is_full` and (1.7) `has_crc`, `is_checkpoint`, `is_info`.
- **`check_crc()`** (`reader.cpp:134-139`): `crc32c` of the frame without its last 8 bytes against the
  stored `u32`; both walkers call it before a frame is believed.
- **`dictionary_problems()`** (`reader.cpp:102-125`): the problems of a merged dictionary, in order:
  schema conflicts, literal and call conflicts (`dyn::dictionary::id_conflicts`, G2-10), the count of omitted
  conflicts (D-03), mixed ABIs.
- **`struct walker`** (`reader.cpp:150-576`): the whole algorithm as one object holding the capture span, the
  merged `dyn::dictionary dict`, the accumulating `walk_result res`, the two callbacks, a `stopped` flag, its
  copy of the `decode_limits` and `shared`, the windowed reader's dictionary when the walker cuts
  frames that reader delivers (`D()` picks one). Members in order: `problem()`, `emit()`, `dict_frame()`,
  `full_frame()`, `compact_frame()`, `frames()`.
- **`parse_file_header()`** (`reader.cpp:579-605`): the FILE header, shared with the windowed reader and `info`;
  accepts minors `format_minor_oldest` (6) to `format_minor` (7).
- **`for_each_record()`** (`reader.cpp:607-682`): FILE/bare-stream detection, header validation, embedded RRI,
  side dictionary (`extra`), the frame walk, and `dictionary_problems`.
- **`decode_problem()`** (`reader.cpp:684-688`): the shared problem text for a `visit_record` failure.
- **`tools::bool_warning`**, **`tools::counting_visitor`** (`reader.cpp:698-744`), **`tools::for_each_frame()`**
  over a span (`reader.cpp:747-774`), the windowed reader (`read_some`, `read_full`, `rri_length_at`,
  `for_each_frame_in_file`; `reader.cpp:778-1120`, with the 1.7 `--resync` scan), **`tools::info()`**
  (`reader.cpp:1174-1343`, with `read_frame_at` and `u64_at` before it), `to_text`/`to_json` of a
  `capture_info` (`reader.cpp:1345-1425`), **`mapped_file::open()`** (`reader.cpp:1427-1454`),
  **`tools::for_each_frame(path)`** (`reader.cpp:1455-1468`), **`internal::for_each_record_in_file()`**
  (`reader.cpp:1475-1538`: the record walk over a file, window by window), the `mapped_file` move
  operations and destructor (`reader.cpp:1544-1563`), **`verify_source()`** (`reader.cpp:1568-1609`: the
  one `verify` over either source) and the two **`tools::verify()`** overloads (`reader.cpp:1612-1621`).

## How it works

### FILE or bare stream: `"ESSR"` is also a frame length

```cpp
// "ESSR" is also a legal frame_len (0x52535345, gk §2): when the header fields do not read as a v1 FILE and
// the capture is large enough for such a frame, it is a bare frame stream. Smaller inputs with the magic are
// FILEs (truncated or of another version), so they keep their precise diagnostic.
bool is_file =
    capture.size() >= 4 && std::memcmp(capture.data(), fmt::file_magic.data(), 4) == 0;
if (is_file && capture.size() >= fmt::file_header_size)
{
    std::uint16_t major{};
    std::uint32_t hash_algo{}, magic_as_len{};
    std::memcpy(&major, capture.data() + 4, 2);
    std::memcpy(&hash_algo, capture.data() + 12, 4);
    std::memcpy(&magic_as_len, fmt::file_magic.data(), 4);
    if ((major != fmt::format_major || hash_algo != fmt::hash_algo_xxh3_64) &&
        capture.size() >= magic_as_len)
        is_file = false;
}
```

(`reader.cpp:614-629`.) A bare stream whose first frame happens to be 0x52535345 bytes long starts with the
FILE magic. The tie-break: if the bytes at the header's `major`/`hash_algo` positions do not read as a v1 FILE
*and* the capture is at least that long, it is a stream; a shorter input with the magic keeps the FILE path and
its precise `truncated`/`bad_version` diagnostic.

For a FILE (`reader.cpp:630-675`) the header goes through `internal::parse_file_header()` (`reader.cpp:579`),
which both capture walkers and `tools::info` share: `major` must equal `format_major` and `minor` must lie in
`format_minor_oldest`…`format_minor` (`bad_version` otherwise — 1.6 and 1.7 are read:
1.5 predates containers, 1.4 constant arguments, 1.3 may carry the canonical names from before format 1.4,
formats ≤ 1.2 used FNV-1a ids, a newer minor is never read by guesswork), `file_flag_le` must be set (`endianness_mismatch`), `hash_algo` must be 2 =
XXH3-64 (`unsupported`). The program hash and the writer's ABI tag land in `w.dict` (`abi`, `abi_known`). With `file_flag_dict` and a non-zero `dict_len`, the
`(dict_offset, dict_len)` window is bounds-checked and parsed with `add_rri`. Where the frames start depends on the minor:

```cpp
if (fh->minor >= 7)
{ // 1.7: the CHAIN follows the opening dictionary; deltas are inline frames, nothing is relocated
    frames_begin = fmt::align8(dict_offset + dict_len) + fmt::chain_size;
}
else if (dict_offset == fmt::file_header_size)
{
    frames_begin = fmt::align8(dict_offset + dict_len);
}
```
(`reader.cpp:655-662`.) A 1.7 file skips the 16-byte CHAIN; the sequential walk does not follow it — it meets every
dictionary delta inline, in order, before the frames that need it (`tools::info` follows the chain with seeks
instead). A 1.6 file whose dictionary sits right after the header starts its frames at
`align8(dict_offset + dict_len)`; otherwise it was back-patched by a 1.6 writer: `rri_length()` measures the
*original* dictionary area so frames start after it, and frames end at `dict_offset` where the relocated
dictionary begins.

The side dictionary from `--dict/--elf` is merged after that (`reader.cpp:676-677`); `merge` carries its ABI tag, so a
side dictionary from another ABI marks the result `abi_mixed`. After the frames, `dictionary_problems` appends
schema conflicts, literal/call conflicts (`schema_conflict`) and a mixed ABI (`abi_mismatch`) to `problems`.

### The frame loop

```cpp
const frame_class fc = classify_frame(
    cap.subspan(off), std::min<std::uint64_t>(end - off, limits.frame_bytes), f);
if (fc == frame_class::bad_len)
{
    problem(f, "bad frame_len " + dec(f.frame_len) + " (capture has " + dec(end - off) +
                   " bytes left)");
    return; // cannot resynchronise
}
++res.frames;
res.bytes += f.frame_len;
if (fc == frame_class::unknown_flags)
{ // G2-9: recorded and skipped, as the block reader does; never parsed or delivered
    if (!problem(f, "unknown frame flags " + dec(f.flags) + " (frame skipped)",
                 error::bad_version))
        return;
    off += f.frame_len;
    continue;
}
const std::span<const std::byte> whole = cap.subspan(off, f.frame_len);
if (f.has_crc && !check_crc(whole))
{ // 1.7: a frame whose trailer does not match is reported and skipped, never decoded
    f.crc_ok = false;
    if (!problem(f, "crc mismatch (frame skipped)", error::crc_mismatch))
        return;
    off += f.frame_len; // not delivered either (the windowed reader does the same)
    continue;
}
```

(`reader.cpp:528-551`.) `f.index` counts every frame, meta frames included (`frame_ref::index`), and
`res.frames`/`res.bytes` count data frames only — dictionary, checkpoint and writer-info frames are consumed, as in
the windowed reader (`reader.cpp:561-569`). The record body ends before the trailer of a CRC
frame. Fewer than 16 bytes left → "stray bytes" problem and stop. A `frame_len` below the
header size, not a multiple of 8, past the end, or (FR-D5) above `decode_limits::frame_bytes` (64 MiB)
cannot be resynchronised — the walk ends there with a `bad frame_len` problem. `is_dict`
is set by `frame_flag_dict` **or** the `dict_frame_sentinel` call hash; `is_full` by `frame_flag_full`. Any
other flag bit is a `bad_version` problem and the frame is skipped: it is counted, but neither parsed nor handed
to `on_frame`, exactly as in the windowed reader (review G2-9). A
dictionary frame is merged (`dict_frame`, using `add_rri` on the body and `dict.merge`) *before* `on_frame` runs,
so the callback sees the dictionary as of that frame. Then `full_frame` or `compact_frame` cuts the body, and
`off += f.frame_len`.

### Compact frames: cut by the call signature

`compact_frame()` (`reader.cpp:330-491`) needs the call: an unknown `call_hash` is an `unknown_call` problem
naming how many records are lost. `n_records` must equal the arity. Then per argument, five encodings:

```cpp
if (a.ak == fmt::arg_string)
{ // 1.1: u32 len, bytes — the length is read from the frame and bounds-checked here
    pos = fmt::align8(pos);
    if (body.size() - pos < fmt::string_len_prefix)
    { ... return; }
    std::uint32_t len;
    std::memcpy(&len, body.data() + pos, 4);
    if (body.size() - pos - fmt::string_len_prefix < len)
    { ... return; }
    record_info r;
    r.ak = fmt::arg_string;
    r.payload = body.subspan(pos + fmt::string_len_prefix, len);
    pos += fmt::string_len_prefix + len;
```

- **string** (`arg_string`): `u32 len` then bytes; both the prefix and `len` are checked against the bytes
  left; `payload` excludes the prefix.
- **dynamic type** (`arg_type` whose dictionary type is `!is_flat`, `reader.cpp:415-443`): the v2 encoding
  `u32 len, u32 0, image` — `fmt::image_prefix` (8) bytes checked, then `len` checked; `payload` is the image.
- **constant** (`arg_const`, format 1.5, `reader.cpp:348-387`): **0 bytes** in the frame. The record is built
  from the call's entry: `is_const` is set, `schema_hash` is the ARG hash, and it is presented as what it stands
  for — `ak = arg_scalar` with `k` when the ARG has a scalar kind, otherwise `ak = arg_type` with the enum looked
  up in the dictionary. `payload` is the first `size` bytes of `dyn::call_arg::value` (`dict.scalar_size(k)`, or
  the enum's size), so it points into the dictionary, not into the capture. An enum that is not in the dictionary
  is an `unknown_type` problem ("constant of unknown type"); a type that is not flat or is larger than 8 bytes,
  or a value with bytes above the type's size, is `corrupt_dictionary` ("constant does not fit its type"). Both
  end the frame.
- **everything else** goes through `dict.record_size(a)` (`reader.cpp:444-477`): a literal is **0 bytes**
  (FR-L6; its id is the signature's); a scalar is `dict.scalar_size(k)` bytes (`wchar` from the writer's ABI tag); a flat type is its dictionary `size`. An unknown
  type or a dynamic type that `record_size` refuses is a problem with that error code. The record is
  8-aligned (`align8(pos)`) and must fit.

After the last argument `align8(pos)` must equal the body size and the padding must be zero — trailing bytes
or non-zero padding are problems (`reader.cpp:479-490`).

### Full frames: cut by per-record headers

`full_frame()` (`reader.cpp:183-328`) can work without the call (the records are self-describing) but
cross-checks against it when present. Each record: `u64 schema_hash, u32 payload_len, u16 layout, u16 rsv`
read with `fmt::cursor`, then `c.bytes(payload_len, r.payload)` — a truncated header or an oversized payload
ends the frame. `layout != 0` is `unsupported`. The argument category comes from the signature, or, without
one, from the header slot: a literal is recognised by an 8-byte payload whose hash is a literal id and not a
type; a string by `fingerprint64("string")`; otherwise a type. A header hash that disagrees with the signature is
`hash_mismatch`. Then the same per-category length checks as compact: scalar `payload_len == kind_size`;
string prefix `inner == payload_len - 4`; literal `payload_len == 8` and the payload id equals the header and
the signature; flat type `size == payload_len`; dynamic type `inner == payload_len - 8` and the prefix
stripped. Records advance by `align8(16 + payload_len)`. Eight or more leftover bytes are reported.

A constant (format 1.5) is in a full-records frame as the scalar or enum record of its value, so it passes the
same checks as a run-time value. When the call is known and says `arg_const`, the walker marks the record
`is_const`, presents it as `arg_scalar` or `arg_type`, and compares the payload with the value in the call's entry:

```cpp
if (arg->hash == r.schema_hash &&
    (payload_len > fmt::const_value_size ||
     std::memcmp(r.payload.data(), &arg->value, payload_len) != 0) &&
    !problem(f,
             "record " + dec(i) + ": constant differs from the call's value " +
                 hex64(arg->value),
             error::hash_mismatch))
    return;
```
(`reader.cpp:247-254`). This is what makes `tools::verify` report a full-records frame whose constant is not the
one its call names. Without the call the record is just a scalar or a type record.

### Problems, `first_error`, stopping

```cpp
bool problem(const frame_info& f, const std::string& what,
             const error code = error::length_mismatch)
{
    add_problem(res.problems, res.problems_omitted, limits.problems,
                "frame " + dec(f.index) + " @" + dec(f.offset) + ": " + what);
    if (!res.first_error)
        res.first_error = code;
    if (stop_on_problem)
        stopped = true;
    return !stop_on_problem;
}
```

(`reader.cpp:170-180`.) Container-level failures (bad FILE header, bad embedded RRI) are returned as the
`std::unexpected` of `for_each_record` itself. Anything inside a frame is a *problem*: text in
`walk_result::problems`, and the code of the first one in `first_error`. The list (FR-D5) holds at
most `decode_limits::problems` lines (1000); every further problem only increments `problems_omitted`
(`add_problem`), so a corrupt multi-terabyte capture cannot accumulate unbounded text. `first_error` and
`stop_on_problem` behave the same whether the line was kept or counted. The `!problem(...)` idiom after a
check means "record it; if `stop_on_problem`, return now, otherwise carry on with this record". `verify`
walks with `stop_on_problem = false` (report everything), `dump` and `render_frame` with `true`. Frames that
cannot be resynchronised return unconditionally. After the walk, every `dyn::dictionary::conflicts` entry of
the merged dictionary becomes a `schema conflict:` problem with `first_error = schema_conflict` (cc F15/F16), and
every `id_conflicts` entry a `literal conflict:` or `call conflict:` problem (two ENTRYs, one id,
different bodies; never silently dropped — review G2-10). The dictionary (D-03) keeps at most `dyn::dictionary::max_conflicts` entries of each list; when it left some out
(`conflicts_omitted` is not 0), `dictionary_problems` adds one more `schema_conflict` problem, "N more conflicts
not listed". The `schema conflict:` text prints only the fields a conflict summary keeps (name, schema hash,
layout hash, size).

### `for_each_record` and its callbacks

`frame_fn` is called once per frame (after a dictionary frame was merged, before its records); `record_fn`
once per record with the `record_info` — `index`, `is_literal`/`literal_id`, `schema_hash`, `type` (nullptr
when not in the dictionary), `payload` (prefixes stripped), `ak`, `k` and `is_const`. A consumer
never sees `ak == arg_const`: a constant arrives as the scalar or enum record it stands for, which is why
`verify`, the text and JSON formatters and `render_frame` needed no change for constants. Returning `false` from either stops
the walk (`stopped = true`) without adding a problem. The result carries `frames`, `records`, `bytes` (sum of
`frame_len`), `program_hash` (the FILE header's, 0 for a bare stream), `problems`, `problems_omitted`,
`first_error` and the merged `dict` as of the end of the walk.

### `verify` — the counting visitor

```cpp
[&](const dyn::dictionary& d, const internal::frame_info& f,
    const internal::record_info& rec) {
    const std::string where = "frame " + internal::dec(f.index) + " @" +
                              internal::dec(f.offset) + ": record " +
                              internal::dec(rec.index) + ": ";
    if (rec.ak == fmt::arg_scalar && rec.k == kind::boolean && rec.payload.size() == 1 &&
        std::to_integer<unsigned>(rec.payload[0]) > 1)
        warnings.push_back(where + bool_warning(rec.payload[0]));
    if (rec.is_literal || !rec.type)
        return true; // unknown types are already structural problems
    if (const auto res = visit_record(rec.payload, *rec.type, d, v); !res)
        internal::add_problem(decode_problems, decode_omitted, problem_limit,
                              internal::decode_problem(f, rec, res.error()));
```

(`reader.cpp:1576-1588`.) `verify` must exercise the same decoder `dump` uses (cc F2 / gk 1.4), so every typed
record is walked by `visit_record` through `counting_visitor`, which only counts. This callback
lives in `verify_source`, which takes a `record_source` — a function that runs the walk over a span
(`for_each_record`) or over a file (`for_each_record_in_file`) — so the span overload and the path overload of
`verify` share one body; the structural problems come first in the report, then the decode problems, both
within `problem_limit`. It counts a string
as one value (the default `string()` event would have made one virtual call per byte — G2-6) and collects
`noncanonical_bool` events. Decode failures are appended after the structural problems; `verify_report::ok()` is
"no problems at all". A `bool` byte other than 0/1 — in a record or a top-level `bool` argument — is a
**warning** (`verify_report::warnings`, "…: on: bool byte 0x02 read as true"), not a problem: every reader repairs
it (G2-12).

### `for_each_frame`, `for_each_frame_in_file`, `mapped_file`

`for_each_frame` is `for_each_record` with a frame callback that skips dictionary frames and builds a
`tools::frame_ref` (the frame's bytes are a subspan of the capture) plus a record callback that accepts
everything. So its problems are exactly `verify`'s structural and record-level problems, without the decode pass.

`for_each_frame_in_file` is a separate, streaming implementation of the same container rules — (A6)
the **windowed reader** under every capture path of the tools:
- **Limits.** The walk takes its `decode_limits` once: `block_options::limits` when the caller set one, else
  the process-wide set (`reader.cpp:830-832`). `max_dict` = `dictionary_bytes` (256 MiB), `max_frame` =
  `frame_bytes` (64 MiB).
- **Buffer.** `buf[pos, have)` holds the unconsumed bytes, and `buf[pos]` is at file offset `buf_off`. The buffer
  starts at the window: `block_options::block_bytes`, or `decode_limits::window_bytes` (10 MiB) when that is 0,
  with a floor of 4096 bytes (`reader.cpp:836-837`). Consuming
  a frame only advances `pos`. The tail is moved to the front only when a read needs the room, so the memmove
  happens once per window rather than once per frame; a frame that straddles a window boundary is completed from
  the next read. A frame larger than the buffer grows it, but
  (D-07) not to the claimed length at once: each time the loop needs room it resizes to
  `min(n, max(2 × size, have + 4096))` and then reads, so the buffer doubles as bytes actually arrive: a
  16-byte header on a pipe that claims 1 GiB it never sends cannot make the reader allocate all of it up front. The
  buffer is not shrunk inside one walk; it is freed when the walk returns.
- **Reads, not mappings.** Windows are filled with `pread` (a regular file: `seekable`, after
  `posix_fadvise(SEQUENTIAL)`) or `read` (a pipe, stdin, a device), `read_some` retrying `EINTR`. `mmap`-ed
  windows were considered and not implemented (`docs/deviations.md`): the difference is one copy per window,
  the memory bound is the same, and `mapped_file` stays for callers who want zero-copy over a file that fits the
  address space.
- **Header.** The FILE header is parsed with the same checks as the in-memory walker: magic, major, newer
  minor, endianness, hash algorithm. A dictionary right after the header is read through the buffer (a 1.7 file:
  then the 16-byte CHAIN is skipped). A back-patched (1.6) one is read with `pread` at `dict_offset`, the original area's length is found entry by entry
  (`rri_length_at`) and skipped by seeking, and the frames end at `dict_offset`. A pipe cannot seek, so a
  back-patched FILE on a pipe is `unsupported`. A dictionary above `max_dict` is `unsupported` before anything
  is allocated (`reader.cpp:932-933`).
- **Frames.** `classify_frame` with `room = min(bytes left, max_frame)`: a bad `frame_len` stops the walk, a
  frame with an unknown flag is reported and skipped, exactly as in the in-memory walker; with
  `block_options::resync` a bad `frame_len` starts the checkpoint scan instead. A frame whose CRC trailer
  does not match is a problem and is skipped. Dictionary frames (full or 1.7 deltas) are merged; one above
  `max_dict` is skipped with a problem (`reader.cpp:1082-1083`); checkpoint and writer-info frames are consumed.
  Records are not pre-validated: whatever the callback runs on them (`deserialize`, `dispatch`, `inspect`,
  `walk_values`) validates them. `frame_ref::bytes` points into the buffer and is valid only during the
  callback (the next `need()` may move it). At the end, `dictionary_problems` adds the merged dictionary's
  conflicts (G2-9: a clean-looking `walk_values_in_file` must not hide a conflict that `verify` reports). Problems
  are capped at `limits.problems` through `add_problem` (`reader.cpp:992-994`).
- **Allocation bounds.** For a file, every length (dictionary, frame) is checked against the file size before
  anything is allocated. Everywhere, a dictionary above `dictionary_bytes` and a frame above `frame_bytes` are
  refused. So a walk's memory is the window, grown at most to the largest
  frame, plus the dictionary. The
  `capture` fuzz target runs this reader over every mutated input through a `memfd`; that is how the bound on
  hostile `dict_len` was found before release.

`mapped_file` is `mmap(PROT_READ, MAP_PRIVATE [| MAP_POPULATE])` + `madvise(MADV_SEQUENTIAL)`. A zero-length file
maps to an empty span, and non-regular files are `unsupported` (use the windowed reader for those).

### `for_each_frame(path)`

```cpp
result<frame_walk> for_each_frame(const std::string_view path, const frame_callback& f,
                                  const file_options& o, const dyn::dictionary* const extra)
{
    if (!o.blocks && path != "-")
    { // 2.1.16: mapping the whole file is the opt-in; windows are the default (A6)
        const auto m = mapped_file::open(path, o.populate);
        if (m)
            return for_each_frame(m->bytes(), f, extra);
        if (m.error() != error::unsupported)
            return std::unexpected(m.error());
        // not a regular file (a pipe, a device): read it in windows
    }
    return for_each_frame_in_file(path, f, block_options{o.block_bytes, o.limits}, extra);
}
```

(`reader.cpp:1455-1468`.) The branch every file reader would otherwise write, in one place.
`file_options::blocks` defaults to `true`, so this overload reads in windows unless the caller asks for the
mapping with `{.blocks = false}`; `mapped_file::open` returns
`unsupported` exactly for a descriptor that is not a regular file, so that case and `"-"` go to the windowed
reader whatever `blocks` says; `not_found` and `io_error` from `open` are returned as they are. `file_options::limits`
and `block_bytes` are passed on as the `block_options`. The mapping lives on the stack of this
call, so `frame_ref::bytes` is valid only during the callback in both branches, and the offsets are file offsets
in both. `walk_values(path, …)` (`walk.hpp`) repeats the same decision for the typed walk.

### Format 1.7: `--resync` and `tools::info`

**`--resync`** (`block_options::resync`) changes what the windowed reader does with a frame header it cannot
parse. Without it the walk stops at a `bad frame_len`, as it always did; with it the reader steps forward 8 bytes
at a time looking for a checkpoint frame:

```cpp
internal::frame_info cand;
if (internal::classify_frame(std::span<const std::byte>(at(0), fmt::frame_header_size),
                             std::min<std::uint64_t>(end - buf_off, max_frame),
                             cand) == internal::frame_class::ok &&
    cand.is_checkpoint && cand.call_hash == fmt::checkpoint_sentinel)
{
    found = true;
    break;
}
```
(`reader.cpp:1046-1053`.) A valid length, the checkpoint flag and the `"ESSRCKPT"` sentinel together are a
signature random data does not produce by accident; the skipped range becomes one problem line (`bad frame_len N:
skipped K bytes to the next checkpoint`) and the walk continues with the checkpoint (`reader.cpp:1027-1063`). Only
the windowed reader (every path overload, `srdump`) resyncs; the in-memory walker over a span stops. A file
written without checkpoints has nothing to resync to: the scan runs to the end.

**`tools::info(path)`** (`reader.cpp:1174-1343`) describes a capture without walking its frames: it reads the
header and the opening dictionary, then — for a 1.7 file — the CHAIN, the writer-info frame right after it (when
`file_flag_info` is set), every dictionary frame the forward chain reaches (each a delta, merged into
`capture_info::dict`; the link is the last 8 bytes of the frame before its CRC trailer), and every checkpoint of the
checkpoint chain. Each frame is read with one `pread` (`read_frame_at`, `reader.cpp:1125-1149`), so the cost is
O(links), not O(frames). A link must point forward (`next > last`) and at a frame of the right kind; a dangling
or looping link ends that chain and is recorded in `capture_info::warnings` (`dangling`: past the end of the
file, not going forward, or not a frame of that kind; a file cut while it was written has a zero or stale last
link — `verify` reports the rest). A 1.6 file yields its dictionary and nothing else; a pipe is `unsupported`.
`to_text` and `to_json` (`reader.cpp:1345-1425`) render the result for `srdump --info` and `--info --json`.

### The record walk over a file, and `verify(path)`

```cpp
result<walk_result> for_each_record_in_file(const std::string_view path,
                                            const dyn::dictionary* const extra,
                                            const bool stop_on_problem, const frame_fn& on_frame,
                                            const record_fn& on_record,
                                            const tools::block_options& o)
{
    const frame_fn nop_frame = [](const dyn::dictionary&, const frame_info&) { return true; };
    walker wk{{}, {}, {}, stop_on_problem, nop_frame, on_record, false};
    if (o.limits)
        wk.limits = *o.limits;
    auto fw = tools::for_each_frame_in_file(
        path,
        [&](const dyn::dictionary& d, const tools::frame_ref& fr) {
```

(`reader.cpp:1475-1487`.) The record-level walk that `verify` and `dump` are written against now exists over a
file too: a `walker` without a capture span, fed one frame at a time by `for_each_frame_in_file`. The callback
rebuilds a `frame_info` from the `frame_ref` (dictionary frames never arrive: the windowed reader consumed them,
so `is_dict` is false), points `wk.shared` at the reader's dictionary — the walker's `D()` reads it instead of
its own `dict` — calls `on_frame`, then `full_frame` or `compact_frame` on the body, and returns `!wk.stopped`.
After the walk the reader's dictionary is *moved* into the result (`record_info::type` pointers handed to
callbacks, e.g. a print frame rendered after the walk, point into its nodes), the reader's structural problems
come first and the walker's record problems after them, both through `add_problem`, and `problems_omitted` is
the sum. `internal::record_source` is the `std::function` shape both walks fit, so `verify_source` and `dump`
(`src/formatters.cpp`) take either. The state kept across frames is the dictionary, the counters and the problem
list: O(types), never O(frames).

`tools::verify(path, extra, block_options)` (`reader.cpp:1220-1230`) is `verify_source` over
`for_each_record_in_file`, with the problem cap from `o.limits` or the process-wide limits; `verify(span, extra)`
(`reader.cpp:1209-1218`) is the same over `for_each_record`. `tests/unit/test_limits.cpp` "2.1.16 A6 every window
size gives the whole-file output, byte for byte" checks that `verify` and all three `dump` renderings over the
golden captures are identical with a 4 KiB window, the default window and the whole file in memory.

## Maintaining it

- **Tests that lock this file:** `test_limits.cpp` ("2.1.16 A3 a frame above decode_limits::frame_bytes
  is a problem, not an allocation", "2.1.16 A3 the problem list is capped at decode_limits::problems; the rest
  are counted", "2.1.16 A6 every window size gives the whole-file output, byte for byte", "2.1.16 A6 a frame
  larger than the window grows it; a 64 MiB capture walks within a bounded RSS" — 1024 frames of a 64 KiB
  string with one 4 MiB frame in the middle, walked and verified with a 1 MiB window),
  `test_readers.cpp` "2.1.10 for_each_frame(path) and walk_values(path): mapped and
  in blocks agree" (100 frames at the same offsets mapped and with 256-byte blocks; a missing file is
  `not_found`), the `user_example1_reader*` and `user_example1_reader*_blocks` ctests, `test_dynamic.cpp` cases (`for_each_frame_in_file` agrees with
  `for_each_frame` on every capture fixture and on a hand-made back-patched FILE, with 4 KiB blocks; mmap vs
  blocks on a 60 KB stream; truncation), `test_tools.cpp` ("2.0.6 tools::for_each_frame …", "gk 1.4 a flipped literal id makes tools::verify report a
  problem" — compact, full, and the decode-error case), `test_capture.cpp` (FR-W1/W2/W7: file, full records,
  bare stream with a leading dictionary frame), `test_print.cpp` ("FR-L6 a compact literal occupies no bytes",
  "FR-H5 captures of an older minor or hash algorithm are refused, not guessed at"), `test_dynamic.cpp`
  ("2.0 corrupt images are rejected, never over-read"),
  `runtime_selftest.cpp` (hand-built frames and the checked-in `tests/fixtures/negative/*` bombs), the
  `golden_srdump_verify*` ctests over `tests/fixtures/golden/`, the `[g2-9]`/`[g2-10]` cases in
  `test_dynamic.cpp` (an unknown flag and a literal conflict seen the same way by both walkers; a back-patched
  FILE whose original area has an entry that is not a multiple of 8),
  `test_roundtrip.cpp` ("FR-A5 constants through the dictionary: RRI round trip, verify, dump, JSON, print (1.5)":
  `verify` accepts a compact and a full frame with constants and reports a full frame whose constant was changed),
  `test_containers.cpp` (map, set and transparent records through `verify`, `dump`, `inspect`, `dispatch`
  and `frame_router`), and `fuzz_capture` with the corpus in `tests/fixtures/corpus/capture/` (`orders-1.7.ser`,
  `dynamic-1.7.ser`, `features-1.7.ser` — the golden files, renamed with each format minor —, `dictionary_deltas-1.7.ser`
  (three dictionary blocks, `examples/dictionary_deltas`); `orders-1.6.ser` and
  `dynamic-1.6.ser` stay as the compatibility seeds of the oldest accepted minor — `bare_frames.bin`,
  `big_bare.bin`, `dict_frame_stream.bin`, `selftest.ser`); `tests/unit/test_format17.cpp` (CRC skipping,
  the delta chain, checkpoints, `info`, `--resync`, 1.6 compatibility).
- **Every new record encoding needs both cutters.** Add it to `compact_frame` (signature-driven, with its own
  bounds check before any `subspan`) and to `full_frame` (header-driven, with the `payload_len` consistency
  check), and add a corpus file. Anything that changes how bytes are cut is a wire-format change:
  `format_minor` bump, `tests/golden/blobs.cpp`, `tests/fixtures/golden/*`, `tests/canary/`.
- **The writer's ABI is load-bearing for scalars:** `record_size` sizes a top-level `wchar` from `dict.abi`, which
  comes from the FILE header or the first RRI; `merge` must keep it (and set `abi_mixed` on a different tag).
- **Never `subspan` before checking.** The pattern is: compute the needed length, compare with
  `body.size() - pos`, then slice. `fmt::cursor::read`/`bytes` already return `false` on short input; use them
  for headers.
- **Reject, do not guess:** a newer minor, a non-zero `layout`, unknown `kind` values (in `dictionary.cpp`) stop
  with a named error; a frame with unknown flags is skipped with a problem. Keep that rule when extending, and
  change `classify_frame` rather than one walker.
- **Problem strings are user-visible** (`srdump --verify`, `dump`'s `problem:` tail) and compared by the
  fixture tests; word changes are fixture changes. Add a problem through `add_problem` (or `walker::problem`),
  never with `push_back`, so the cap holds.
- **Two walkers, one behaviour.** `for_each_record` (span) and `for_each_record_in_file` (windows) must report
  the same problems for the same bytes, and `for_each_frame_in_file` must deliver the same frames whatever the
  window; "2.1.16 A6 every window size gives the whole-file output, byte for byte" is the guard. A new limit
  goes into `decode_limits` and is read once per walk, not per frame.
- **Allocation:** the walker allocates for the dictionary, problem strings and `std::function` callbacks; it
  is a tools-layer component, not the serialisation hot path. Do not include it from `serialize.hpp`.
- **Threading:** `for_each_record` is a pure function of its inputs; a `walker` is used by one thread. The
  callbacks are the caller's.
- **Fuzz** under the ASan/UBSan preset (`fuzz_capture`) for a few minutes after any change to the cutters
  or the header detection.
