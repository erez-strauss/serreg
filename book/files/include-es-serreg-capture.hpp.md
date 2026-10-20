# `include/es/serreg/capture.hpp`

The FILE container and stream dictionary frames (spec §6.5, FR-W1, FR-W7; format 1.7):
`basic_capture_file` writes a seekable `.ser` file — the 48-byte header, the opening dictionary (an RRI), the
16-byte CHAIN, an optional writer-info frame, then frames — and keeps it **self-describing up to its last complete
frame at every instant**: when the registry moves (a `dlopen`), the next frame is preceded by a dictionary frame
holding the *delta*, linked forward from the previous dictionary by one 8-byte `pwrite`. Nothing is rewritten at
`close()` (1.6 back-patched the header; 1.7 writers never do). Optional per file, zero bytes per frame when off: a
CRC32C trailer on every frame, checkpoint frames (their own forward chain, for seeking into a file of any size), the
writer-info frame and a sync policy. `write_dictionary_frame` and `dictionary_tracker` do the same for
non-seekable sinks (pipes, sockets). It belongs to the **serialisation / wire-format layer**, is header-only, and
depends on reflection only through `serialize.hpp`/`registry.hpp`.

## Depends on / used by

Includes: `es/serreg/format.hpp` (layout constants, `info_key`), `es/serreg/hash.hpp`, `es/serreg/registry.hpp`
(`registry::instance()`, `export_image_since()`, `export_image_scope()`, `version()`, `program_hash()`),
`es/serreg/serialize.hpp` (`detail::sink_failure`), `es/serreg/sink.hpp` (`detail::report_line`, `scoped_sink`),
`es/serreg/version.hpp` (`library_version` for the writer-info frame), `<chrono>`, `<cstdio>` (`FILE*`),
`<unistd.h>` (`pwrite`, `fdatasync`, `gethostname`, `getpid`, `gettid`) and the usual containers.

Included by: `serreg.hpp`. Users: `tests/unit/test_capture.cpp`, `tests/unit/test_format17.cpp`,
`tests/unit/test_sinks.cpp`, `tests/unit/test_log.cpp`, `tests/golden/make_fixtures.cpp` /
`make_dynamic_fixtures.cpp` (they write the golden `.ser` fixtures, `features.ser` among them),
`tests/python/log_capture.cpp`, `tests/wireshark/print_capture.cpp`, `examples/file_store.cpp`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `sync_policy` | `enum class : u8` | `none`, `dictionary` (default), `checkpoint`, `frame` — when the writer calls `fdatasync` |
| `capture_options` | struct | per-file run-time options (below); every default writes what 1.6 wrote, except the writer-info frame |
| `capture_policy` | struct (NTTP) | `dictionary_updates = true`: false removes the per-frame registry-version compare and the delta machinery |
| `detail::file_header(program_hash, dict_offset, dict_len, extra_flags)` | inline function | the 48-byte FILE header; flags = `file_flag_le \| file_flag_dict \| extra` |
| `detail::dictionary_frame_bytes(rri)` | inline function | a dictionary frame: header \| RRI \| pad \| `u64 next` (0) |
| `detail::meta_frame_bytes(flags, sentinel, payload)` | inline function | a checkpoint or writer-info frame |
| `detail::info_builder`, `abi_long_text`, `hostname_text`, `system_wall_clock_ns` | helpers | the writer-info payload |
| `write_dictionary_frame(S& out, since_version = 0)` | function template (`sink S`) | a dictionary frame on any sink: the full image, or the delta since a registry version |
| `dictionary_tracker` | struct | `last_version`; `update(S& out) -> result<bool>`: the full image the first time, a delta whenever `registry::version()` moved |
| `basic_capture_file<Scope = void, capture_policy P = {}>` | class template | the seekable file container and `claiming_sink`: `open`, `write`, `claim`, `commit`, `frame_crc`, `checkpoint`, `close`, `state`, `bytes_written`, `data_frames`, `is_open`, `options` |
| `capture_file` | alias | `basic_capture_file<>` — the whole registry is the file's dictionary |
| `scoped_capture_file<Scope>` | alias template | `basic_capture_file<Scope>` — a `scoped_sink`; its dictionary holds only the scope's calls |

A capture file is not thread-safe (one writer) and is non-copyable. Its destructor closes and prints a one-line
diagnostic if that fails.

## How it works

### The options

```cpp
struct capture_options
{
    bool crc = false;                   // every data frame carries a CRC32C trailer (frame flag crc)
    std::uint64_t checkpoint_bytes = 0; // a checkpoint frame every N bytes of frames (0: none by size)
    std::chrono::nanoseconds checkpoint_every{0}; // … or every T (0: none by time; checked every 64 frames)
    bool checkpoint_dictionary = false; // each checkpoint carries the full dictionary (seek without the chain)
    bool writer_info = true;            // the writer-info frame after the opening dictionary
    sync_policy sync = sync_policy::dictionary;
    std::string_view program{}; // recorded in the writer-info frame (argv[0], a service name)
    bool info_host = true;      // hostname, pid and start time in the writer-info frame (off: reproducible files)
    bool info_thread = false;   // the opening thread's id in the writer-info frame (a logger's thread_policy::once)
    std::string_view log_clock{}; // the loggers' clock, recorded for readers: "system" | "steady" | "tsc" | "user"
    std::uint64_t (*clock)() noexcept = nullptr; // wall clock for checkpoints and the info frame; nullptr: system
};
```
(`capture.hpp:58-71`.) Run-time options go to `open(path, options)`; the compile-time choices are template
arguments: the scope tag (whose dictionary) and `capture_policy` (`capture.hpp:83-86`). Both are part of the sink
*type*, so their branches are `if constexpr` and cost nothing per message.

### File layout at `open()`

```cpp
const registry& reg = registry::instance();
dict_version_ = reg.version();
const auto rri = export_dictionary(0);
std::uint32_t flags = 0;
if (opt_.crc)
    flags |= fmt::file_flag_crc_all;
if (checkpoints_enabled())
    flags |= fmt::file_flag_checkpoints;
if (opt_.writer_info)
    flags |= fmt::file_flag_info;
const auto hdr =
    detail::file_header(reg.program_hash(), fmt::file_header_size, rri.size(), flags);
if (!fwrite_all(hdr.data(), hdr.size()) || !fwrite_all(rri.data(), rri.size()) ||
    !pad8())
    return fail(error::io_error);
// the CHAIN: u64 next_dictionary, u64 first_checkpoint — zero until patched
chain_offset_ = pos_;
```
(`capture.hpp:276-292`.) The header says `dict_offset = 48`, `dict_len = rri.size()` and is never touched again.
The RRI is padded so that everything after it is 8-aligned (every frame is 8-aligned relative to the file start,
which is what makes `view<T>` zero-copy possible on an mmap'd capture). The 16 zero bytes of the CHAIN follow
(`u64 next_dictionary`, `u64 first_checkpoint`), then — unless `writer_info` is false — the writer-info frame
(`write_info`, `capture.hpp:501-533`), then the first `fdatasync` if the sync policy is `dictionary` or stronger.
`export_dictionary(since)` (`capture.hpp:440-446`) is `export_image_since(since)` for `capture_file` and
`export_image_scope(scope_hash_v<Scope>, since)` for a scoped file.

### A claiming sink with a per-frame hook

```cpp
std::byte* claim(const std::size_t n)
{ // may allocate (first frame / larger frame): honestly not noexcept
    if (!f_ || !st_)
    {
        if (st_)
            st_ = std::unexpected(error::io_error);
        return nullptr;
    }
    if (!before_frame())
        return nullptr;
    if (buf_.size() < n)
    {
        buf_.resize(n);
    } // grows once to the largest frame seen; not on the hot path afterwards
    return buf_.data();
}
// 2.1.12 (review C-09): the result reaches serialize(), so a frame the disk refused is not reported as
// written (it used to surface only at the next call or at close()).
bool commit(const std::size_t n) noexcept
{
    return fwrite_all(buf_.data(), n) && after_frame(n);
}
```
(`capture.hpp:320-341`.) `serialize` fills the frame in `buf_` and `commit` writes it with one `fwrite`.
`commit` returns that result (C-09): `serialize` commits through `detail::commit_claimed`
(`sink.hpp`), so a frame the file system refuses makes that very `serialize` call fail with `io_error`. `claim` may
allocate, so it is not `noexcept` and `sink_noexcept<capture_file>` is false. `write()` (the plain protocol) runs the
same two hooks. Failures are sticky in `st_`; `state()` exposes them and `serialize` reports `io_error` instead of
`sink_full` through `sink_with_state`. A capture file that failed to `open` has `is_open() == false`, `state()`
says why, and every later `write`/`claim` refuses (gk 1.8).

`frame_crc()` (`capture.hpp:352-355`) returns `options().crc`; `serialize` asks every sink that has it
(`detail::sink_wants_crc`, `serialize.hpp`) and then appends the 8-byte trailer to each frame — so a
`{.crc = true}` file needs no per-call option.

### Before a frame: the dictionary delta

```cpp
bool before_frame()
{
    if constexpr (P.dictionary_updates)
    {
        const std::uint64_t v = registry::instance().version();
        if (v != dict_version_) [[unlikely]]
            return write_delta(v);
    }
    return true;
}
```
(`capture.hpp:408-417`.) One relaxed load per frame. When the registry moved since the last dictionary written,
`write_delta` (`capture.hpp:447-465`) exports the delta since `dict_version_` — only the entries whose
`registry_entry::added` is newer — writes it as a dictionary frame, and links it:

```cpp
const auto buf = detail::dictionary_frame_bytes(rri);
const std::uint64_t at = pos_;
if (!fwrite_all(buf.data(), buf.size()))
    return false;
if (!sync_if(sync_policy::dictionary) || !patch(next_link_offset_, at))
    return false;
next_link_offset_ = at + buf.size() - fmt::dict_frame_link_size;
```
(`capture.hpp:455-461`.) `patch` (`capture.hpp:535-548`) flushes the `FILE*` buffer and writes the frame's offset
with one aligned 8-byte `pwrite` into the previous link — the CHAIN's first word for the first delta, the last 8
bytes of the previous dictionary frame afterwards. Each link is written exactly once, after the frame it points at
exists (and, with the default sync policy, is on disk), so a crash leaves either a zero link (a reader that follows
the chain stops; a sequential reader still meets the delta inline) or a valid one. A registry move that adds
nothing to this file's dictionary (another scope's types) writes no frame (`capture.hpp:450-454`).

With `capture_policy{.dictionary_updates = false}` the whole branch disappears: a program that never `dlopen`s
saves the compare.

### After a frame: checkpoints and sync

`after_frame` (`capture.hpp:418-437`) counts data frames and bytes; with checkpoints enabled a checkpoint is due
after `checkpoint_bytes` bytes or — the clock is read only every 64 frames — after `checkpoint_every`.
`write_checkpoint` (`capture.hpp:466-500`) writes the 64-byte fixed part (version 1, flags, `rri_len`, `prev`,
`next` = 0, data frames and bytes before it, wall clock, the latest dictionary's offset, the registry version) and,
with `checkpoint_dictionary`, the full dictionary, then links it:

```cpp
// link it: the previous checkpoint's `next` (payload offset 16 → frame offset 32), or the CHAIN's second word
const std::uint64_t link = last_ckpt_offset_ ? last_ckpt_offset_ + fmt::frame_header_size + 16
                                             : chain_offset_ + 8;
```
(`capture.hpp:491-493`.) `checkpoint()` writes one on demand; `close()` writes a final one when checkpoints are on
and at least one data frame was written, so the chain ends at the last frame. `sync_if(level)`
(`capture.hpp:549-559`) flushes and `fdatasync`s when the policy is at least `level`: `dictionary` syncs at open
and before linking each delta, `checkpoint` also at each checkpoint, `frame` after every frame.

### The writer-info frame

`write_info` (`capture.hpp:501-533`) records `library_version`, `format` ("1.7"), `program` (if given), hostname,
pid and start time (`info_host`), the thread id (`info_thread`), the loggers' clock (`log_clock`), the long ABI
text, `crc`, `checkpoint_bytes`, `checkpoint_ns`, `dictionary` (`"full"` or `"scoped:<scope name>"`) and `sync`, as
`u16 key, u16 len, bytes, pad to 2` items (`detail::info_builder`, `capture.hpp:145-175`). `srdump --info` prints
them.

### `close()`: nothing is rewritten

```cpp
[[nodiscard]] result<void> close()
{
    if (!f_)
        return st_;
    if (st_ && checkpoints_enabled() && data_frames_ != 0)
        static_cast<void>(write_checkpoint());
    if (st_ && std::fflush(f_) != 0)
        st_ = std::unexpected(error::io_error);
    if (st_ && opt_.sync != sync_policy::none && ::fdatasync(::fileno(f_)) != 0)
        st_ = std::unexpected(error::io_error);
```
(`capture.hpp:354-363`.) A 1.6 writer's `close()` appended a complete RRI and rewrote the header with
`file_flag_backpatched` when the registry had grown, so a 1.6 file had to be closed to show every type. Here the
deltas are already written inline, so a file cut anywhere — a crash, a full disk, a
copy taken while it grows — is readable up to the last complete frame. Readers still accept back-patched 1.6
files.

The destructor calls `close()` and can only print an error; call `close()` explicitly when the result matters
(it is `[[nodiscard]]`):

```cpp
~basic_capture_file()
{
    if (f_ && !close())
        detail::report_line("es::serreg: capture_file: close failed ({})",
                            to_string(st_.error()));
}
```
(`capture.hpp:257-262`.) The line is written with `detail::report_line` (`sink.hpp`): formatted into a stack
buffer and written to descriptor 2, so a destructor never allocates and never throws, also when stderr is closed.

### Scoped capture files

```cpp
template<class Scope = void, capture_policy P = capture_policy{}>
class basic_capture_file : public detail::scope_decl<Scope>
```
(`capture.hpp:246-247`.) `detail::scope_decl<Scope>` (`capture.hpp:232-238`) declares `using dictionary_scope =
Scope;` for a tag type, which makes `scoped_capture_file<Tag>` a `scoped_sink` (`sink.hpp`). Every
`serialize`/`print` into such a file odr-uses `detail::registrar_scoped<Tag, Sig...>::g`
(`registry.hpp`), a `node_kind::scope` registration that adds no ENTRY but records "this call belongs to that
scope". The file's opening dictionary and its deltas are `export_image_scope`: the scope's calls, closed over
their argument types (recursively) and literals. A process with ten thousand message types can thus write a log
file whose dictionary holds the dozen calls actually logged. The writer-info item `dictionary` says
`scoped:<scope name>`.

### Streams: dictionary frames

A pipe cannot be patched, so the dictionary travels *inside* the stream:

```cpp
template<sink S>
result<std::size_t> write_dictionary_frame(S& out, const std::uint64_t since_version = 0)
{
    const auto rri = registry::instance().export_image_since(since_version);
    const auto buf = detail::dictionary_frame_bytes(rri);
```
(`capture.hpp:201-205`.) `dictionary_tracker::update(out)` (`capture.hpp:207-216`) does one atomic load of
`registry::version()` and writes a frame only the first time (the full image) or when the version moved (since
1.7 the delta since `last_version`); it is meant to be called from the writer's idle/heartbeat path, not from
`serialize()`. Readers treat a dictionary frame (`frame_header::dictionary()`: the flag, or the sentinel as call
hash) as a dictionary update and merge it; a 1.7 dictionary frame ends with the 8-byte `next` link (0 in a stream,
nobody patches it). `deserialize` on a dictionary frame is `hash_mismatch` (it is not a data frame).

```cpp
es::serreg::fd_sink pipe{fd};
es::serreg::dictionary_tracker dt;
for (;;)
{
    if (const auto r = dt.update(pipe); !r) break;          // first call: the full dictionary frame
    if (const auto r = es::serreg::serialize(pipe, "tick"_, tick); !r) break;
    ...                                                     // after a dlopen: the next update() writes a delta
}
```

### Anatomy of a finished `.ser` (1.7)

For a program that wrote two frames with the default options and loaded no plugin:

| offset | content | written by |
|---|---|---|
| 0 | 48-byte FILE header: `"ESSR"`, 1, 7, flags `LE\|dict\|info`, algo 2, program hash, `dict_offset = 48`, `dict_len = L`, ABI tag | `open()` |
| 48 | RRI: 40-byte header + every ENTRY of the registry (or of the scope) | `open()` |
| 48 + L | zero pad to a multiple of 8 | `open()` |
| `align8(48 + L)` | CHAIN: `u64 next_dictionary = 0`, `u64 first_checkpoint = 0` | `open()` |
| +16 | writer-info frame (flag bit 4, `"ESSRINFO"`) | `open()` |
| … | frame 1, frame 2 (16-byte header + records [+ 8-byte CRC trailer]) | `serialize` → `claim`/`commit` |

After a `dlopen` a dictionary frame with the delta sits before the first frame that needed it and the CHAIN's
first word points at it; with checkpoints, checkpoint frames are interleaved and the CHAIN's second word points at
the first. Chapter 7 dumps such a file byte by byte (`tests/fixtures/golden/features.ser`).

## Maintaining it

- **Header and frame bytes are format.** `detail::file_header`, `dictionary_frame_bytes`, `meta_frame_bytes` and
  `write_checkpoint` write the layouts of `format.hpp`; `tests/fixtures/golden/*.ser` are byte-exact fixtures
  (`golden_fixture_ser_byte_exact`, `golden_fixture_dynamic_ser_byte_exact`, the `features.ser` checks)
  regenerated by `make_fixtures` with `info_host = false` so they are reproducible. A new flag bit or field is a
  `format_minor` bump and a refreeze.
- **Chain invariants:** a link is patched only after the frame it points at is fully written (and synced under
  the default policy); each link is written once; `pos_` counts every byte written so offsets are exact; the
  opening RRI stays 8-padded. Never rewrite the header or the opening dictionary — crash safety depends on it
  (DECISIONS P-*).
- **Defaults write what 1.6 wrote** (DECISIONS A-11) except the header minor, the CHAIN and the writer-info frame;
  a new feature is opt-in through `capture_options` or `capture_policy`.
- A capture file uses `std::FILE` buffering; `bytes_written()` is the logical position, not what reached the disk.
  The sync points are `sync_policy`'s and `close()`.
- Concurrency: one writer per file. `dictionary_tracker` is per stream.
- `write_dictionary_frame`, `write_delta` and `write_checkpoint` allocate; none of them is on the steady-state
  path (a delta follows a `dlopen`, a checkpoint every N bytes).
- **The destructor's diagnostic** uses `detail::report_line`, never `printf`/`fprintf` or `std::println`.
- Tests: `tests/unit/test_capture.cpp` ("FR-W1 capture_file: compact frames read back, verify and dump",
  "FR-W2 capture_file with full records", "FR-W7 bare frame stream with a leading dictionary frame verifies"),
  `tests/unit/test_format17.cpp` (the 1.7 container: "P 1.7 a capture file is written as 1.7: header, CHAIN,
  writer-info frame, no rewrite at close", the delta chain, CRC trailers, checkpoints, `--resync`, 1.6
  compatibility), `tests/unit/test_sinks.cpp` ("gk 1.3 8 MiB struct serialised to capture_file under a 1 MiB
  stack", "gk 1.8 capture_file: open failure is sticky, write fails with it, close reports it",
  "FR-W7 dictionary_tracker writes a dictionary frame once and not again while the version …",
  "2.1.12 C-09 capture_file: a frame the file system refuses is reported by that serialize()"),
  `tests/unit/test_log.cpp` (scoped capture files under a logger).
