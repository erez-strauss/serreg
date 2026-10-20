# `include/es/serreg/limits.hpp`

*(Maintainer answer O2, spec FR-D5.)* The reader limits: one struct, `decode_limits`, and the
two functions that hold the process-wide copy, `get_decode_limits()` / `set_decode_limits()`. It is the code
form of the **never-crash contract** stated in `docs/SECURITY.md` and spec FR-D5: for any sequence of input bytes — a
capture, a stream, a frame, an RRI image, an ELF file, a config file, a ring header in shared memory — every
public reading API returns a value or an error, and never crashes, aborts or asserts, executes undefined
behaviour, or allocates or loops beyond these limits. Errors are reported; decoding moves on to the next item
where the format allows it. "Wrong data" means corrupt or truncated files, buggy writers, mismatched versions, a
peer that wrote garbage into a ring; it does not mean system out-of-memory within the limits, or a policy the
user asked for (`ES_SERREG_CONFLICT=abort`). The header has no reflection dependency and includes nothing from the
library; it is C++ a reader of any layer can take.

## Depends on / used by

Includes: `<cstddef>`, `<cstdint>`.

Included by: `serreg.hpp` (the umbrella), `tools.hpp` (`block_options::limits`, `file_options::limits`, the
comments on `verify_report::problems_omitted` and `frame_walk::problems_omitted`), `src/internal.hpp`
(`input_limit()`). Through those: `src/reader.cpp` (defines the two functions; the windowed reader and the span
walker take one copy per walk), `src/formatters.cpp` (`dump`'s problem cap), `src/decoder.cpp`
(`max_depth()`), `src/dictionary.cpp` (`add_rri`), `src/file_io.cpp` (`slurp`), `src/cli.cpp`
(`--frame-limit`), `tests/unit/test_limits.cpp`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `decode_limits` | struct, `constexpr`-comparable (`operator==` defaulted) | the seven limits below |
| `get_decode_limits()` | function → `decode_limits`, `noexcept` | the process-wide limits (a copy) |
| `set_decode_limits(const decode_limits&)` | function, `noexcept` | replaces them; thread-safe (a mutex in `src/reader.cpp`); the next walk sees the new values |

The fields, with their defaults and what each one bounds:

| field | default | bounds |
|---|---|---|
| `frame_bytes` (`u64`) | 64 MiB | the largest frame a capture reader accepts, header included (the writer limit agreed under O2); a `frame_len` above it is a `bad frame_len` problem and the walk stops (`classify_frame`'s `room`) — or, with `--resync` (format 1.7), continues at the next checkpoint frame |
| `dictionary_bytes` (`u64`) | 256 MiB | the largest dictionary a reader loads: a FILE's embedded dictionary (`unsupported`), a dictionary frame (skipped with a problem) |
| `dictionary_entries` (`u32`) | 1 000 000 | the most entries an RRI header may declare (`n_entries`); more is `corrupt_dictionary` (`dyn::dictionary::add_rri`) |
| `image_depth` (`u32`) | 64 | the deepest nesting of struct levels the dictionary-driven decoder follows (`src/decoder.cpp`: `2 × min(image_depth, fmt::max_nesting)` units); `fmt::max_nesting` is the ceiling and bounds the typed decoder at compile time |
| `input_bytes` (`u64`) | 1 GiB | the largest input read whole: an RRI, ELF or config file, a `--diff` operand, or a capture handed to the span-based readers through `tools::read_file`; `internal::max_input_bytes` (1 GiB) is its ceiling, so the field can lower the cap but not raise it |
| `window_bytes` (`size_t`) | 10 MiB | the window of the windowed readers (`for_each_frame(path)`, `walk_values(path)`, `verify(path)`, `dump(path)`, `srdump`): the file is read this many bytes at a time; a frame larger than the window grows it to that frame, within `frame_bytes`; the reader's floor is 4 KiB |
| `problems` (`size_t`) | 1000 | problem lines a walk keeps; after that only a count (`problems_omitted`; `… and N more` in the text report, `"problems_omitted"` in JSON), so a corrupt multi-terabyte capture does not accumulate unbounded text |

## How it works

### Process-wide, read once per walk

```cpp
// The process-wide limits: what every reader uses unless the call carries its own. Thread-safe.
decode_limits get_decode_limits() noexcept;
void set_decode_limits(const decode_limits& l) noexcept;
```

(`limits.hpp:45-47`.) The storage is a `constinit decode_limits` behind a `std::mutex` in `src/reader.cpp`
(`reader.cpp:23-41`): a mutex rather than seven atomics because a reader takes **one copy per walk**, never per
frame — `walker::limits` for the span walker, a local `limits` in `for_each_frame_in_file`, `verify_source` and
`dump_source` — so a `set_decode_limits` during a walk affects the next walk, not the running one. The defaults
are chosen so that no honest data reaches them; `srdump --frame-limit <size>` (config key `framelimit`) writes
`frame_bytes` for the run (`cli::execute`), and `--window <size>` chooses the window per call.

### Per call

```cpp
struct block_options
{
    std::size_t block_bytes = 0;           // 0: decode_limits::window_bytes (10 MiB by default)
    const decode_limits* limits = nullptr; // nullptr: the process-wide limits (set_decode_limits)
};
```

(`tools.hpp:184-188`.) Where a reader takes `tools::block_options` or `tools::file_options` — `for_each_frame`
over a path, `for_each_frame_in_file`, `verify(path, …)`, `dump(path, …)` — a caller can narrow or widen the
limits for that call by pointing `limits` at its own `decode_limits`; the object must outlive the call.
`walk_values(path, …)` forwards `block_bytes` but not `limits`, so the typed path walk uses the
process-wide set.

### What is outside the contract

System out-of-memory within the limits; policies the user asked for (`ES_SERREG_CONFLICT=abort`); the writer
side (a program serialising its own objects is trusted with its own memory); the generated Wireshark dissector
and C++ header, which are text the user reviews before running or compiling; and `es::ipc`, whose rings validate
their headers by their own rules (`docs/ipc/ipc-design.md` §5) and have no notion of these limits
(`docs/deviations.md`).

### How it is checked

- `tests/unit/test_limits.cpp`: "2.1.16 A3 decode_limits: defaults, process-wide set/get, and what each one
  bounds", "2.1.16 A3 a frame above decode_limits::frame_bytes is a problem, not an allocation" (a 200 MiB
  `frame_len` after two honest frames, over a span and over a file), "2.1.16 A3 the problem list is capped at
  decode_limits::problems; the rest are counted", "2.1.16 A3 decode_limits::dictionary_entries bounds what a
  dictionary may declare", the two "2.1.16 A6 …" window cases and "2.1.16 decision D: an out-of-range value of
  an enum without a fixed underlying type reads as 0 and is reported as invalid_value".
- The fuzz driver (`tests/fuzz/fuzz_driver.cpp`) gates every input on time and memory as well as on crashes: an
  input that takes longer than 2 s (20 s under a sanitizer) or raises the RSS high-water mark by more than
  512 MiB fails the run and is saved as `<target>-slow-N.bin` / `<target>-big-N.bin`, because amplification is a
  bug under the contract even when nothing crashes (`files/tests.md`).
- `docs/SECURITY.md` says how to report a crash, a hang, an amplification or undefined behaviour reached from input
  bytes.

## Maintaining it

- **A new limit goes here**, with a default no honest data reaches, a one-line comment saying what it bounds,
  a row in `docs/SECURITY.md`'s list and spec FR-D5, and a case in `test_limits.cpp`. Read it once per walk
  (`get_decode_limits()` or the call's `limits`), never per frame or per record.
- **Changing a default is a behaviour change** for every tool: a CHANGELOG line, and the `test_limits.cpp`
  defaults case.
- **`operator==` is defaulted** so a test can compare the whole set; keep the struct an aggregate with
  default member initialisers (`decode_limits l = get_decode_limits(); l.frame_bytes = …;` is the intended use).
- **Not `constexpr` storage**: the process-wide copy lives in `libserreg` (`src/reader.cpp`), so a static
  `libserreg` in a host and in a `dlopen`'ed DSO gives two sets, as it gives two registries (chapter 9).
- **Threading:** `get_decode_limits`/`set_decode_limits` take a mutex; a walk never does after its first copy.
  Nothing on the serialisation hot path reads the limits.
