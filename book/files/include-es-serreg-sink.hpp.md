# `include/es/serreg/sink.hpp`

The output side of `serialize()`: the `sink` and `claiming_sink` concepts, three ready-made sinks (`span_sink`,
`vector_sink`, `fd_sink`), the one `write(2)` loop of the header-only part (`detail::write_fd`) and
the library's diagnostic line on stderr (`detail::report_line`). A claiming
sink hands out an exact-size slot so a frame is built in place with no intermediate copy; a plain sink only has
`write()`. This header is part of the **serialisation layer**, has **no reflection dependency**, and is the
only header besides `capture.hpp` that touches the OS (`<unistd.h>`).

## Depends on / used by

Includes: `<cerrno>`, `<concepts>`, `<cstddef>`, `<cstdint>`, `<cstring>`, `<format>` (`std::format_to_n`,
`std::format_string`), `<span>`, `<utility>`, `<vector>`, `<unistd.h>`, `es/serreg/error.hpp` (`status`,
`error::io_error`).

Included by: `serialize.hpp` (the concepts constrain every `serialize*`; `sink_failure` reads `state()`),
`capture.hpp` (`capture_file` models `claiming_sink`; `write_dictionary_frame<S>` takes any `sink`),
`tests/support/test_support.hpp`, and via `serreg.hpp` the examples. `detail::report_line` is used by
`capture.hpp` (the destructor's close failure) and `src/registry.cpp` (conflict line, second-copy line, the
`ES_SERREG_ALLOW_DLCLOSE` notice, dump-signal lines). `es::ipc::sink`
(`include/es/ipc/sink.hpp`; es::ipc_old's `producer` likewise) models `claiming_sink` without including this header: the concept is duck-typed, so `es::ipc` stays independent of
es::serreg.

## What it defines

| name | kind | meaning |
|---|---|---|
| `sink<S>` | concept | `s.write(const std::byte*, size_t) -> bool`; `false` ⇒ `error::sink_full` or `state().error()` |
| `scoped_sink<S>` | concept | `sink<S>` plus a member type `S::dictionary_scope`: the calls serialised into it register under that scope (`sink.hpp:28-33`) |
| `claiming_sink<S>` | concept | `sink<S>` plus `s.claim(size_t) -> std::byte*` (nullptr ⇒ full) and `s.commit(size_t)` (returning `void`, or `bool`: false ⇒ not written) |
| `detail::commit_claimed(out, n)` | function template | calls `out.commit(n)`; the result of a `bool` `commit`, `true` for a `void` one |
| `span_sink` | struct | writes into a caller-provided `std::span<std::byte>`; `used`, `bytes()`; claiming; `noexcept`; no wrap |
| `vector_sink` | struct | appends to a `std::vector<std::byte>&`; claiming; allocates (not `noexcept`) |
| `fd_sink` | struct | writes to a blocking descriptor; plain (write-only); sticky `status state()`, `reset()` |
| `detail::write_fd(int, const std::byte*, size_t) -> status` | inline function | the write loop: retries `EINTR`, zero-length write is `io_error` |
| `detail::report_line(std::format_string<Args...>, Args&&...)` | function template, `noexcept` | one diagnostic line on stderr, formatted like `std::println`, with no allocation and no exception; for code that runs in destructors, library constructors and inside `dlopen()`/`dlclose()` |

Not defined here but part of the sink protocol: `detail::sink_with_state<S>` and `detail::sink_failure(s)` in
`serialize.hpp` (a sink *may* expose `status state() const`), and `detail::sink_noexcept<S>` (whether
`serialize` into `S` is `noexcept`).

## How it works

### Two protocols

```cpp
template<class S>
concept sink = requires(S& s, const std::byte* p, std::size_t n) {
    { s.write(p, n) } -> std::same_as<bool>; // false ⇒ error::sink_full, or state().error()
};
template<class S>
concept claiming_sink = sink<S> && requires(S& s, std::size_t n) {
    { s.claim(n) } -> std::same_as<std::byte*>; // nullptr ⇒ full
    { s.commit(n) }; // void, or (2.1.12, review C-09) bool: false ⇒ the frame was not written
};
```
(`sink.hpp:24`). `serialize_frame` (`serialize.hpp`) branches on `claiming_sink<S>` at compile time:

- **claiming**: `p = out.claim(total)`; if null → failure; fill header and records straight into `p`;
  `detail::commit_claimed(out, total)`; if that is false → failure. Nothing is copied twice; this is the hot path for ring buffers, disruptor slots, `capture_file`'s internal buffer.
  A claiming sink must give back memory that stays valid until `commit`; `claim` may be called with a size larger
  than any previous frame.
- **plain**: the frame is assembled in a stack buffer (≤ 64 KiB, or ≤ 4 KiB when the size is only known at run
  time) and written with one `write`, or streamed record by record through a 16 KiB chunk. Writing stops at the
  first `false`.

Every sink here provides `write()`, so a claiming sink is also usable by `write_dictionary_frame` and by the
streaming path of `serialize_raw`.

### A commit that can fail

A sink may learn only at commit time that the frame did not reach its destination (`capture_file`, whose `fwrite`
may fail); with a `void` commit `serialize` would return success and the error would surface later, at `close()`.
So `commit` may return `bool` (C-09), and both claiming
paths (`serialize_frame`, `serialize_raw`) go through one helper:

```cpp
template<claiming_sink S>
bool commit_claimed(S& out, const std::size_t n) noexcept(noexcept(out.commit(n)))
{
    if constexpr (std::is_same_v<decltype(out.commit(n)), bool>)
        return out.commit(n);
    else
    {
        out.commit(n);
        return true;
    }
}
```
(`sink.hpp:38-48`.) `false` makes that `serialize` return `sink_failure(out)` — the sink's `state().error()` when it
has `state()`, else `sink_full`. A `void` `commit` keeps its meaning (the frame is written), so existing sinks
compile and behave as before. The concept accepts any return type of `commit`; only `bool` is interpreted.

### Failure reporting: `state()`

A plain `false` from `write` means `error::sink_full` — unless the sink also has `status state() const`, in which
case `serialize` returns *that* error (`sink_failure` in `serialize.hpp:114`). `fd_sink` uses this so a broken pipe
is `io_error` rather than a misleading `sink_full`:

```cpp
bool write(const std::byte* const p, const std::size_t n) noexcept
{
    if (!st_)
        return false;
    st_ = detail::write_fd(fd, p, n);
    return st_.has_value();
}
status state() const noexcept { return st_; }
void reset() noexcept { st_ = {}; }
```
(`sink.hpp:152`). The state is *sticky*: after one failure every later `write` refuses without touching the
descriptor until `reset()`. `capture_file` follows the same pattern. `status` is `std::expected<void, error>`
(`error.hpp`): `st_` true = ok.

### `detail::write_fd`

```cpp
inline status write_fd(const int fd, const std::byte* p, std::size_t n) noexcept
{
    while (n)
    {
        const ssize_t r = ::write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return std::unexpected(error::io_error); }
        if (r == 0) return std::unexpected(error::io_error);
        p += r; n -= static_cast<std::size_t>(r);
    }
    return {};
}
```
(`sink.hpp:108`). Blocking descriptors only: `EAGAIN` is not retried. `tools::write_all` in `libserreg` is the same
loop for the compiled side (`write_file`, `export_image(fd)`, the env dump).

### `detail::report_line`

```cpp
template<class... Args>
void report_line(const std::format_string<Args...> fmt, Args&&... args) noexcept
{
    char buf[1024];
    const auto r =
        std::format_to_n(buf, std::ptrdiff_t{sizeof buf - 1}, fmt, std::forward<Args>(args)...);
    const auto n = static_cast<std::size_t>(r.out - buf); // format_to_n stops at the limit
    buf[n] = '\n';
    static_cast<void>(write_fd(2, reinterpret_cast<const std::byte*>(buf), n + 1));
}
```

(`sink.hpp:132-141`.) The maintainer's rule is no `printf`/`puts` family anywhere: applications, tools,
tests and examples use `std::print`/`std::println`. The library's own diagnostics cannot use `std::println` either:
they are written from destructors (`capture_file`), from libserreg's load-time constructor (`serreg_init`) and
from `push()` inside `dlopen()` under the loader lock (the conflict line), in processes that may have closed
stderr. `std::println` formats into a `std::string` (allocation) and throws `std::system_error` when the
write fails. `report_line` takes the same compile-time-checked format string, formats with `std::format_to_n` into
a 1024-byte stack buffer — at most 1023 characters, the rest is cut — appends `'\n'` and writes the line with one
`write_fd(2, …)`, ignoring the result. Formatting a `std::string_view`, an integer or a `const char*` does not
allocate. The whole line, newline included, is handed to one `write_fd` call, which retries a short write.

### `span_sink` and `vector_sink`

`span_sink` refuses instead of wrapping (`n > buf.size() - used` → false, gk 1.16), so a fixed buffer that fills
up yields `sink_full` and writes nothing for that frame (`claim` returns null before anything is touched).
`vector_sink::claim` does `resize(at + n)` and returns `data() + at`; `commit` is a no-op because the resize
already accounted for the bytes. Both are cheap value types over a reference; neither owns storage.

### `noexcept` propagation

`serialize` is `noexcept(detail::sink_noexcept<S>)`, computed from `noexcept(write)` and, for claiming sinks,
`noexcept(claim) && noexcept(commit)` — see `serialize.hpp:113` for the specialisation trick that avoids naming
`claim` on a plain sink. `span_sink` and `fd_sink` are fully `noexcept`; `vector_sink` is not (allocation), so
`serialize` into it may propagate `std::bad_alloc` (cc F6). `tests/unit/test_sinks.cpp` "cc F6 serialize is
noexcept exactly when the sink is" checks the three built-ins plus a `throwing_sink` whose `write` is not
`noexcept` (`capture_file` is not `noexcept` either — its `claim` may allocate).

### Writing a claiming sink

A ring buffer or disruptor slot needs three members; `es::ipc::sink` (over a `es::ipc::producer`) is the reference implementation. The contract, in
the order `serialize_frame` uses it:

```cpp
struct my_ring   // user code, modelled on span_sink
{
    std::byte* claim(std::size_t n) noexcept;          // n = frame_size(...): return n contiguous bytes or nullptr
    void commit(std::size_t n) noexcept;                // n is the same value; the frame is complete (or: bool, false ⇒ not written)
    bool write(const std::byte* p, std::size_t n) noexcept; // used only by write_dictionary_frame / serialize_raw streaming
    status state() const noexcept;                      // optional: makes serialize() report your error, not sink_full
};
static_assert(es::serreg::claiming_sink<my_ring>);
```

Rules a claiming sink must honour: the slot from `claim(n)` stays writable until `commit(n)`; nothing else touches
it in between; a `nullptr` means "no room for `n` bytes right now" and leaves the sink unchanged (so `serialize`
returns `sink_full` with nothing written); `claim` may be called with any `n` up to `u32` — a fixed-slot ring should
return `nullptr` for a frame larger than a slot rather than truncating. A sink that can fail after the bytes were
written into the slot returns `bool` from `commit` and gives `false` in that case; it should also offer `state()` so
the caller learns why. If the consumer reads with `view<T>`, hand out 8-aligned slots.

### Optional members a sink may add

Two members change what `serialize` writes; both are detected with `requires`, so a sink without them is
unaffected:

- **`using dictionary_scope = Tag;`** makes the sink a `scoped_sink`:

  ```cpp
  template<class S>
  concept scoped_sink = sink<S> && requires { typename S::dictionary_scope; };
  ```
  (`sink.hpp:32-33`.) `serialize_frame` then also odr-uses `detail::registrar_scoped<Tag, Sig...>::g`, a
  compile-time registration of "this call signature is written into sinks of scope `Tag`".
  `registry::export_image_scope(scope_hash_v<Tag>)` exports exactly those calls, closed over their argument types
  and literals — the dictionary of a log file that holds a dozen calls in a program with thousands of types.
  `scoped_capture_file<Tag>` is the built-in scoped sink; a user ring or socket sink adds the one `using` line.
  A `logger` with `dictionary_policy::scoped` (the default) requires one (`log.hpp`).
- **`bool frame_crc() const`** returning true makes every frame written into the sink carry the 1.7 CRC32C trailer
  (`detail::sink_wants_crc`, `serialize.hpp`), whatever the caller's `serialize_options`. `capture_file` returns
  its `capture_options::crc`.

### The three built-ins side by side

| | `span_sink` | `vector_sink` | `fd_sink` |
|---|---|---|---|
| protocol | claiming | claiming | plain |
| storage | caller's span | caller's `std::vector` (grows) | descriptor |
| full | `write` false / `claim` null, nothing written | never (allocates) | never; `io_error` instead |
| `state()` | no | no | yes, sticky, `reset()` |
| `noexcept` | yes | no | yes |
| `serialize` error on failure | `sink_full` | `std::bad_alloc` propagates | `io_error` |
| typical use | fixed message buffers, tests | tests, building a dictionary/RRI in memory | pipes, sockets, files opened elsewhere |

`capture_file` (`capture.hpp`) is the fourth: claiming, with `state()`, not `noexcept` (its `claim` may grow a
buffer), and its `commit` returns `bool`. `es::ipc::sink::commit` (`es/ipc/sink.hpp`) and es::ipc_old's `producer::commit` return
`bool` too.

## Maintaining it

- **Concept stability is API.** A custom sink written against the original concepts still compiles: `last_error()`
  became `status state()`, and `commit` may return `bool` instead of `void` without requiring anything new. Do not add required members to `sink`/`claiming_sink`; add
  optional protocols the way `state()` was added (a separate concept in `serialize.hpp`, detected with
  `if constexpr`).
- A claiming sink's `claim(n)` must not assume `n` fits any previous allocation and must return 8-aligned memory
  only if the *consumer* needs it — `serialize` itself uses `memcpy` throughout and does not require alignment;
  `view<T>` on the reader side does, which is why `capture_file` and `span_sink` over an aligned buffer give
  zero-copy reads and a misaligned span gives `error::misaligned`.
- `write()` must not partially consume and return false in a way the caller cannot detect; the contract is
  "all or nothing per call" for plain sinks (the streaming path writes header, then records, then pad, and stops
  at the first `false` — a partial frame in the sink is the *sink's* state to report).
- `fd_sink` is deliberately not seekable and does not close the descriptor. Dictionary updates on a pipe are the
  application's job via `dictionary_tracker` (`capture.hpp`).
- **`report_line` is for the library's own diagnostics only**. It must stay `noexcept`, allocation-free
  and a single `write_fd(2, …)`; do not pass it arguments whose formatter allocates. Everything else prints with
  `std::print`/`std::println`; no `printf`/`puts`/`snprintf` anywhere.
- Keep `write_fd` and `tools::write_all` behaviourally identical (EINTR retry, zero-length → error, no EAGAIN
  retry); `tests/unit/test_sinks.cpp` "gk 1.9 fd_sink: io_error is sticky until reset(), never sink_full" pins the
  header-side loop.
- Tests: `tests/unit/test_sinks.cpp` (all of it: 8 MiB struct to `capture_file` and to `fd_sink(pipe)` under a
  1 MiB stack, failing plain sink stops after the first refused write, bool scrub, noexcept table, `capture_file`
  sticky open failure, `dictionary_tracker`), `tests/unit/test_roundtrip.cpp` "FR-S2 span_sink exhaustion reports
  sink_full and writes nothing", `tests/unit/test_print.cpp` "FR-A2 strings on plain sinks: small, and past the 4 KiB
  stack buffer (streamed)", `tests/unit/test_dynamic.cpp` "2.0 large vectors: plain sink beyond the chunk (heap
  path) and claiming sink (no heap)", `tests/unit/test_ipc.cpp` "ipc: basic_sink — serreg frames are built in the ring and read in place"
  (`es::ipc::sink` as a claiming sink; `serialize` into a full ring reports `sink_full`).
