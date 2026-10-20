# `python/serreg/` — the pure-Python reader

*(DECISIONS T-*.)* A stand-alone reader of `es::serreg` captures in Python: standard library only,
Python ≥ 3.10, no C++ build, no compiled extension. It reads what `srdump` reads — FILE captures of format 1.6
(back-patched or not) and 1.7 (CHAIN, dictionary deltas, CRC32C trailers, checkpoints, writer info), bare frame
streams with a side dictionary, RRI images — decodes compact and full records, flat structs, enums, arrays, the
canonical images of dynamic types (strings, vectors, optionals, variants, maps, sets, nested), transparent
container types, and renders print and log frames with a `std::format` subset. Like the C++ readers it never
raises on corrupt input: problems are collected, as `verify` collects them. Its JSON output is compared with
`srdump --json` by the `python_reader_parity` ctest on every golden and corpus capture, so it doubles as an
executable second specification of the wire format (chapter 7).

It lives outside `src/` and is not part of the CMake install; it ships in the release tarball and the public
tree as `python/` (with `python/README.md` and a `python/pyproject.toml`, so `pip install ./python` installs it).
(This page is kept at the top of `book/` rather than in `files/` because `scripts/check-book.py` maps every
`files/` page to a C++ source or one of its three topic pages.)

## Layout

| file | lines | contents |
|---|---|---|
| `python/serreg/__init__.py` | 26 | the package: re-exports `Reader`, `Dictionary`, `load_rri`, `capture_info`, the value classes and the renderers; `__version__`, the library version |
| `python/serreg/format.py` | 316 | the wire format as constants and `struct` layouts (its docstring is the byte-level layout of ENTRY, RRI, FILE, FRAME and the canonical image, mirroring `format.hpp`), kinds and their sizes, `AbiTag`, the bounded `Cursor`, `scalar_value`, `crc32c` (table-driven, checked against `0xE3069283` at import), the limits (`FRAME_BYTES_LIMIT` 64 MiB, `DICTIONARY_ENTRIES_LIMIT`, `PROBLEMS_LIMIT` 1000, `MAX_NESTING` 64) and `LEVEL_NAMES` |
| `python/serreg/dictionary.py` | 630 | ENTRY/RRI parsing: `Field`, `Type`, `Literal`, `CallArg`, `Call` (`is_print`, `newline`, `is_log`, `form`), `parse_field`/`parse_type`/`parse_literal`/`parse_call`, `Dictionary` (`add_rri`, `add_entries`, `merge`, the `find_*` lookups, conflicts, `to_json_obj()` and `to_text()` in srdump's forms), `load_rri(path)` |
| `python/serreg/reader.py` | 1585 | the capture walker and the value decoder: `parse_file_header`, `rri_length`, `classify_frame`, `check_crc`, `Reader` (`frames()`, `verify()`), `Decoder` (the slot rules and byte budget of `fmt::image_budget`), the value classes (`Struct`, `Enum`, `Array`, `Str`, `Variant`, `Map`, `Set`, `Lit`, `Transparent`, `Null`), `value_json`/`record_json`/`json_lines` and `value_text`/`record_text`/`text_lines` (srdump's JSON and text), `cxx_format` (a `std::format` spec subset), `decode_log`/`LogView`/`render_message`/`log_envelope`, `CaptureInfo`/`capture_info` (`tools::info`, with the same chain-link `warnings`) |
| `python/serreg/__main__.py` | 110 | `python3 -m serreg`: the command line |

## The command line

```
python3 -m serreg <file.ser> [--json | --text] [--dict x.rri ...]   dump: the dictionary, then the frames
python3 -m serreg <file.ser> --verify [--json]                        counts and problems; exit 1 on problems
python3 -m serreg <file.ser> --info [--json]                          1.7: header, dictionary chain, checkpoints
python3 -m serreg <file.ser> --level LVL [--json]                     log frames at or above LVL, one line each
python3 -m serreg --rri <image.rri> [--json]                          a dictionary on its own
```
(`__main__.py:1-9`.) Further options: `--dict-only` (only the capture's dictionary), `--hex` (with `--json`: each
record's payload bytes), `-` as the capture reads standard input. A file is `mmap`ed when possible. Exit codes: 0
ok, 1 problems or a capture that cannot be read, 2 usage. Without `--level` the dictionary is printed first "as of
the end of the capture" — a first pass with `Reader(data, extra, decode=False)` collects every dictionary frame —
exactly as `srdump` does.

## How it works

### The walk

`Reader(data, extra=[...])` (`reader.py:471-545`) mirrors `for_each_record` of `src/reader.cpp`: the
`"ESSR"`-is-also-a-frame-length tie-break, `parse_file_header` (major 1, minor 6 or 7, little-endian, XXH3-64), the
opening dictionary, then where the frames begin — after the CHAIN for 1.7, after the dictionary for a plain 1.6
file, between the original area and `dict_offset` for a back-patched one. `frames()` (`reader.py:557-603`) is a
generator over the data frames:

```python
whole = memoryview(data)[off:off + f.frame_len]
if f.has_crc and not check_crc(whole):
    self._problem(f, "crc mismatch (frame skipped)")
    off += f.frame_len
    continue
f.body = whole[F.FRAME_HEADER_SIZE:f.frame_len - (F.CRC_TRAILER_SIZE if f.has_crc else 0)]
if f.is_dict:
    self._dict_frame(f)
if not f.is_meta:
```

A bad `frame_len` stops the walk (there is no `--resync`), an unknown flag skips the frame, a CRC mismatch skips
it, a dictionary frame (full or delta) is merged, checkpoint and writer-info frames are consumed. `frame.index`
counts every frame, `frame_count`/`byte_count` data frames only — srdump's counting. Compact frames are cut by the
call signature, full frames by their record headers, with the same bounds checks as the C++ cutters; problems use
srdump's wording (`frame N @off: …`) so the parity test can compare them.

### Values

`Decoder` (`reader.py:202-335`) decodes a record against its `Type`: flat structs member by member at their
offsets, enums (by name when known), arrays, and the canonical image of a dynamic type with the slot rules of
`format.hpp` (`n == 0 ⇔ off == 0`, tails after the fixed part, inside the image, a running byte budget) and a
depth limit of `2 × MAX_NESTING`. Maps and sets are entry blocks of `extent` bytes; a transparent type renders as
its one field. The JSON form (`value_json`) follows srdump's rules: enums as `{"name", "value"}`, 64-bit integers
as strings, floats as the shortest round-trip text (single precision for `f32`), maps as
`[{"key","value"}]`.

### Print and log frames

`render_message` (`reader.py:1272-1295`) formats a print, println or log frame: the format literal from the
dictionary, `{}`/`{n}`/`{:spec}`/`{{`/`}}`, each argument through `cxx_format` (fill, align, sign, `#`, `0`,
width, precision, the integer, float, char and string presentation types — the subset the library's readers
support; nested replacement fields are refused, as `log.hpp` refuses them at compile time). For a log frame
`decode_log` (`reader.py:1205-1231`) reads the envelope at `log_layout`'s positions — level, file, line, mask,
time, then the thread id and sequence the mask announces — and `log_envelope` prefixes
`<time> <LEVEL> <file>:<line> [tid] #seq `; the time is `YYYY-MM-DD HH:MM:SS.nnnnnnnnn` (UTC) for the system clock
and the raw number for any other clock (`LogView.time_text`, `reader.py:1197-1202`).

### `--info`

`capture_info(data)` (`reader.py:1510-1585`) is `tools::info` over the bytes: the header, the opening dictionary,
the CHAIN, the writer-info items, every delta the dictionary chain reaches and every checkpoint, each link required
to point forward. Unlike the C++ version it also checks the CRC of each chain frame it reads.

## What it does not do

No XXH3 (it never recomputes a hash: ids, schema and call hashes are taken from the dictionary), no `--elf`
(reading the `es_serreg_dict` section of a program), no `--diff`, no `--resync`, no windowed reading of pipes
(`-` reads standard input whole), no TOML output, no `--wireshark`/`--cxx` generation.

## Maintaining it

- **A wire-format change changes this package too.** A new flag, field kind, frame kind or envelope position is
  added to `format.py`'s constants and docstring and to the walker or decoder, in the same change as the C++
  readers; `python_reader_parity` fails otherwise.
- **The parity test is the contract.** `tests/python/parity.py` runs `srdump --json` and
  `python3 -m serreg --json` on `orders.ser`, `dynamic.ser`, `features.ser`, the 1.6 corpus captures,
  `selftest.ser`, two bare streams with `selftest.rri`, and a log capture written by `python_log_capture`
  (`tests/python/log_capture.cpp`), and compares the dictionary objects, every record, message and log line
  (floats numerically), the `--verify` counts and verdict, `--info` for 1.7 files and `--level trace`. It reports
  SKIP (77) without a Python 3 interpreter. Label `python`.
- Standard library only; no third-party imports, no type-checker-only syntax beyond 3.10.
- `__version__` and `pyproject.toml`'s `version` are bumped by `scripts/bump-version.sh` together with
  `library_version`.
- `__pycache__/` directories are by-products of running the tests in place; `scripts/make-release.sh` deletes
  them from the working tree before it packs.
