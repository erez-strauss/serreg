# serreg — a pure-Python reader for es::serreg captures

A stand-alone reader for the binary files `es::serreg` writes (library 2.2.1, wire format 1.6 and 1.7): capture
FILEs, bare frame streams, dictionary images (`.rri`). Standard library only, Python 3.10 or later, no build step.
It is written to be read next to the C++ sources: `format.py` is the byte layout, `dictionary.py` mirrors
`src/dictionary.cpp`, `reader.py` mirrors `src/reader.cpp`, `src/decoder.cpp` and `src/formatters.cpp`.
`tests/python/parity.py` keeps it honest against the reference tool, `srdump`.

## Use

From this directory (or after `pip install .`, which installs the package and a `serreg-dump` command):

```sh
python3 -m serreg orders.ser                   # text dump: the dictionary, then every frame and record
python3 -m serreg orders.ser --json            # srdump's JSON: a dictionary object, then one object per line
python3 -m serreg features.ser --verify        # "ok: 11 frames, 34 records, 664 bytes"; exit 1 on problems
python3 -m serreg features.ser --info          # 1.7: header, dictionary chain, checkpoints, writer info
python3 -m serreg app.ser --level warn         # log frames at or above warn, one rendered line each
python3 -m serreg frames.bin --dict app.rri    # a bare frame stream: the dictionary comes from an RRI
python3 -m serreg --rri app.rri --json         # a dictionary on its own
```

`--json` prints what `srdump --json` prints, semantically: the same dictionary object, then
`{"frame":N,"record":M,"type":...,"value":...}` per record, `{"frame":N,"message":"..."}` per print frame,
`{"frame":N,"log":{"level":..,"time":..,"file":..,"line":..,["thread":..,]["sequence":..,]"message":".."}}` per
log frame and `{"problem":"..."}` lines at the end. Integers of 64 bits are strings (JSON numbers are doubles in
most parsers), enums are `{"name","value"}`, maps are `[{"key","value"}]`, arrays of `char` are strings.

As a library:

```python
import serreg

with open("orders.ser", "rb") as fh:
    r = serreg.Reader(fh.read())                 # bytes, bytearray or an mmap
for frame in r.frames():                         # data frames; meta frames are consumed on the way
    print(frame.index, frame.form, hex(frame.call_hash))
    for rec in frame.records:
        print("  ", rec.index, rec.type_label, serreg.value_text(rec.value))
    if frame.call and frame.call.is_print:       # print / println / log frames: the rendered message
        print("  ", serreg.render_message(r.dictionary, frame.call, frame.records))
print(r.frame_count, r.record_count, r.byte_count, r.problems)

d, err = serreg.load_rri("app.rri")              # an external dictionary for a bare frame stream
r = serreg.Reader(stream_bytes, extra=[d])
info = serreg.capture_info(file_bytes)           # 1.7 chains: info.dictionary_offsets, info.checkpoints, info.writer
```

Decoded values are plain Python where they can be (`int`, `float`, `bool`, `Null`) and small dataclasses where
structure matters: `Struct(type_name, fields)`, `Array(items, chars)`, `Str(raw)`, `Enum(type_name, name, value)`,
`Variant(index, alt_text, value)`, `Map(entries)`, `Set(items)`, `Lit(id, text)`, `Transparent(value)`.
`serreg.value_json(v)` turns any of them into srdump's JSON shape, `serreg.value_text(v)` into its one-line text
form (`corpus::Order{id: 42, px: 1050000, ...}`).

## What is covered

- **FILE** 1.6 and 1.7: the header, the opening dictionary, the 1.7 CHAIN, back-patched 1.6 files (the
  dictionary rewritten at the end), bare frame streams with or without an inline dictionary frame, extra
  dictionaries from `.rri` files (merged first-definition-wins, conflicts reported).
- **Frames**: compact and full-record frames, dictionary frames (merged as they are met, 1.7 forward link
  skipped), checkpoint and writer-info frames (consumed; `--info` follows both chains by their offsets), the
  CRC32C trailer (a mismatching frame is reported and skipped, never decoded), unknown frame flags (skipped with a
  problem), a bad `frame_len` (the walk stops, as in C++). Frame numbering counts every frame, meta frames
  included; `frames`/`bytes` count data frames only — exactly srdump's counts.
- **Records**: flat structs (base classes as `<base:Name>` pseudo-fields), enums (sign-extended, unknown values
  without a name), arrays (arrays of `char` as strings), top-level scalars and strings, literals, 1.5 constants
  (zero bytes in a compact frame, the value from the call's entry), and the v2 canonical image: `std::string`,
  vectors, optionals, variants (`variant<i:type>`), maps, sets, nested dynamic structs, transparent container
  arguments — with the same slot rules and byte budget as both C++ decoders, so `--verify` accepts and refuses the
  same images.
- **Messages**: print/println frames rendered like `std::format`, log frames with their envelope (level, file,
  line, time, thread, sequence). The `std::format` subset: `{}`, `{n}`, `{{`/`}}`, fill and alignment, sign, `#`,
  `0`, width, precision, the integer types `d x X o b B c`, the floating-point types `f F e E g G a A` and the
  default shortest form, strings with precision, `bool` and `char`. Struct arguments render in the one-line text
  form, enums as their enumerator name — what the C++ writer's compile-time check assumed.
- **Robustness**: nothing raises on corrupt input. Every length is checked against the buffer, structural
  problems are collected in `Reader.problems` (the same texts as srdump's where practical), a record that does not
  decode carries `error` instead of `value`, and the C++ depth and step limits bound the work a hostile dictionary
  can cause.

## What is not covered

- Nested replacement fields in a format string (`{:{}}`) and the `L` locale option: `srdump` rejects them too;
  both tools report a render problem and print no message for that frame.
- `srdump`'s text output is not reproduced byte for byte: records print in their one-line form (no block
  layout, no array eliding), and the dictionary listing is a close relative, not a copy. `--json` is the
  interface to rely on.
- Hashes are not recomputed: the reader trusts the schema hashes, literal ids and call hashes in the file (there
  is no XXH3 here), so it cannot tell a forged hash from a real one — only that it is unknown.
- No ELF reading (`srdump --elf`), no `--diff`, no Wireshark or C++ generation, no `--resync` scan after a bad
  frame, no `--window` (the capture is memory-mapped whole, so files of any size open, but nothing is streamed from
  a pipe except by reading it whole).
- Decode limits are the C++ defaults (64 MiB frames, 256 MiB dictionaries, 1000 problem lines) and are not
  configurable from the command line.

## Testing

`tests/python/parity.py` (ctest `python_reader_parity`, label `python`) runs `srdump --json` and this reader on
the golden and corpus captures (1.6 and 1.7 FILEs, bare frame streams with an external RRI, a log capture written
by `tests/python/log_capture.cpp`) and compares them frame by frame and record by record, plus `--verify`'s counts
and verdict, `--info`'s dictionary blocks and checkpoints, and `--level trace`. Run it by hand with

```sh
python3 tests/python/parity.py --srdump build/gcc16-debug/srdump --package python \
    tests/fixtures/golden/orders.ser tests/fixtures/corpus/capture/bare_frames.bin:tests/fixtures/corpus/rri/selftest.rri
```
