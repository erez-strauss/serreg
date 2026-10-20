# Security policy

## Threat model

ser-reg is designed for **one trust domain**: the programs that write captures, the programs and tools that read
them, and the processes that share a ring are operated by the same people. Input is not assumed hostile — but it
is assumed to be *wrong* at times: a truncated file, a buggy writer, a mismatched version, a peer that wrote
garbage into shared memory, a disk that flipped a bit.

What the library promises for such input is the **never-crash contract** (spec FR-D5, decision R-2 in
`docs/DECISIONS.md`):

> For any sequence of input bytes — a capture, a stream, a frame, an RRI image, an ELF file, a config file, a ring
> header in shared memory — every public reading API returns a value or an error, and never crashes, aborts or
> asserts, executes undefined behaviour, or allocates or loops beyond the configured limits. Errors are reported;
> decoding moves on to the next item where the format allows it.

The limits are `es::serreg::decode_limits` (`include/es/serreg/limits.hpp`): the largest frame (64 MiB), the
largest dictionary (256 MiB, 1 000 000 entries), the deepest nesting (64), the largest whole-file read (1 GiB), the
reader window (10 MiB) and the number of problem lines kept (1000). They are process-wide (`set_decode_limits`)
and per call where a reader takes them. Every bug that breaks the contract within the limits is fixed, whatever
the input that found it.

Outside the contract: system out-of-memory within the limits; policies the user asked for
(`ES_SERREG_CONFLICT=abort`); the writer side (a program serialising its own objects is trusted with its own
memory); and the generated Wireshark dissector and C++ header, which are text the user reviews before running or
compiling — a dictionary is a schema, not a program, but a dictionary from an untrusted source should be read
with `srdump` first.

**Hashes and CRCs are integrity checks, not authentication.** The XXH3-64 identities (schema, layout, call,
literal, program) and the optional CRC32C frame trailer detect bugs and bit rot: a writer and a reader that
disagree about a type, a frame cut short, a disk or a ring that flipped a bit. Neither is a MAC: anyone who can
write the bytes can write matching hashes and CRCs, and the library has no keys, signatures or encryption. A
capture that arrives from outside the trust domain is therefore **out of contract** — the decoder still returns
an error instead of crashing on it (that is the never-crash contract, and a crash on such a file is still a bug to
report), but nothing in the file proves who wrote it or that it was not altered on the way. Keyed integrity and
encryption are roadmap items behind an optional dependency (`docs/roadmap.md` §5.6), not properties of today's
format.

**Generated Lua and C++ are code.** `srdump --wireshark` emits a Lua dissector that Wireshark executes and
`srdump --cxx` emits a header that a compiler and then your program execute. The emitters already escape and
restrict what the dictionary contributes: every name or literal text that reaches the Lua file is a Lua string
literal with non-printable bytes as decimal escapes (`lua_str`, `src/wireshark.cpp`); every identifier the C++
generator declares is sanitised to letters, digits and `_` (`sanitize`, `src/cxxgen.cpp`), the recorded name
travels in an escaped string literal (`[[=es::serreg::name("…")]]`), and what cannot be reproduced is listed in
a banner comment rather than emitted — so a dictionary cannot inject statements through a name. Even so,
a dictionary from an untrusted source is fed to `srdump`'s decoder (`srdump --dict-only`, `--json`, `--verify`),
which is under the never-crash contract, and **not** to Wireshark or a compiler until a person has read the
generated file.

## How the contract is checked

- Four fuzz targets (`tests/fuzz`: RRI, capture, ELF, config) run in the full verification protocol under
  ASan/UBSan at 200 000 mutations each, and are gated on **time and memory** as well as on crashes: one input that
  takes longer than 2 s or raises the process's RSS high-water mark by more than 512 MiB fails the run and is saved
  next to the corpus (`<target>-slow-N.bin`, `<target>-big-N.bin`). The seed corpora are in `tests/fixtures/corpus`;
  the crafted negative fixtures (`tests/fixtures/negative`) are replayed byte for byte.
- Every reader limit has a unit test (`tests/unit/test_limits.cpp`): a frame above `frame_bytes`, a dictionary above
  `dictionary_entries`, the problem cap, the windowed readers against the whole-file output, an enum value outside
  its range.
- The release protocol (`scripts/run-all-tests.sh`) runs the whole suite under ASan/UBSan and TSan.

## Reporting

Report a crash, a hang, an amplification (time or memory far beyond the input's size) or undefined behaviour
reached from input bytes to the maintainer, Erez Strauss (erstrauss@gmail.com), with the input file (or the fuzz
driver's seed and iteration), the library version (`srdump --version`), the compiler and the platform. Reports are
answered as bugs, with a fix in the next release and the input added to the negative fixtures; there is no bounty
and no embargo process, because the users and the operators of the data are the same people.

## Supported versions

The current release only. The wire format minor moves when a change needs it (`format.hpp`); readers accept the
minors from `format_minor_oldest` (1.6) to `format_minor` (1.7) and refuse anything older (`bad_version`); older
captures are read with the release that wrote them.
