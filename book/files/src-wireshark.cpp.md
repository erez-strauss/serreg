# `src/wireshark.cpp`

`tools::wireshark_dissector()` from `include/es/serreg/tools.hpp` (`srdump --wireshark`, user documentation
in `docs/wireshark.md`): a Wireshark dissector, written in Lua, generated from one `dyn::dictionary`. The output is
one Lua file in two parts. The first part is data generated here from the dictionary: the facts of the source
(`SERREG`), one value table per enum (`E_<hash>`), one `ProtoField` per value a display filter can name (`PF`,
`serreg.<type>.<member>`), the types with their fields (`TYPES`), the call signatures (`CALLS`), the literal texts
(`LITERALS`) and the scalar-argument hashes of full records (`SCALARS`). The second part is a fixed Lua runtime
(`lua_runtime_text`), the same for every dictionary of one library version: a file reader for FILE captures, the
frame and record walk, and a descriptor-driven value walker that applies the slot rules of `src/decoder.cpp`
(`fmt::image_budget`). No C++ type is compiled into the dissector and the capture's own dictionary is never used to
decode: every frame is matched to the generated tables by the XXH3-64 ids on the wire, so a frame the dissector
does not know is shown as bytes and flagged, never decoded with a wrong layout. The runtime has a print renderer
(std::format replacement fields in Lua), the per-call `serreg.call.name` field, and heuristic UDP/TCP dissectors
that recognise frames by call hash without a port setting; for format 1.6 the `K.MAP`/`K.SET` kinds —
a map member as `[i]` subtrees with `key`/`value` sub-fields (`<member>.key`, `<member>.value`), text `{k: v, …}`
/ `{…}` — and transparent types, rendered as their value under the argument's name; and for format 1.7 the
container and frames: the FILE reader skips the 16-byte CHAIN, a frame's CRC32C trailer is shown
and excluded from the records, checkpoint and writer-info frames are dissected as such, dictionary deltas are
compared like any dictionary frame, log frames are rendered with a `LEVEL file:line [tid] #seq` prefix, and the
heuristics accept the 1.7 meta-frame sentinels. This is the **tools layer**; the file has no reflection dependency.

## Depends on / used by

Includes: `internal.hpp` (`dec`, `hex64`), `es/serreg/hash.hpp` (`fingerprint64`), `es/serreg/tools.hpp`
(`wireshark_options`, `library_version`), `<map>`, `<set>`. From `dictionary.hpp`: `dyn::dictionary` (`types`,
`calls`, `literals`, `find_type`, `find_literal` (`call_name`), `program_hash`, `computed_program_hash()`,
`abi`), `dyn::type`, `dyn::field`, `dyn::call` (`is_print()`, `newline()`, `is_log()`, `args`, `flags`). From `descriptor.hpp`: `kind`, `is_scalar_kind`, `to_string(kind)`. From `format.hpp`:
`format_major`/`format_minor`, `hash_algo_xxh3_64`, `arg_scalar`, `abi_tag`, `unpack_abi`.

Library: `es::serreg_tools` (`libserreg_tools`, decision G).

Used by: `src/cli.cpp` (`write_wireshark()`, the `--wireshark <file.lua|->` option), the ctests
`wireshark_generate` and `wireshark_dissector_parity` (`tests/wireshark/`, label `wireshark`; with the
`wireshark_print_capture` program), and (through `cli.cpp`) `example_embedded_cli_wireshark`. The generated
file is loaded by Wireshark or `tshark -X lua_script:<file>`.

## Structure

- **Header comment** (`wireshark.cpp:1-17`): the two parts, the snapshot rule.
- **`hexkey()`** (`wireshark.cpp:27-32`): 16 lower-case hex digits without `0x`, the form of every 64-bit key in
  the generated tables (what the runtime's `UInt64:tohex()` gives after `string.lower`).
- **`lua_str()`** (`wireshark.cpp:34-56`): a Lua string literal; printable ASCII as is, `"` and `\` escaped,
  every other byte as a three-digit decimal escape.
- **`filter_segment()`** (`wireshark.cpp:58-81`): a display-filter name segment: letters and digits kept, every
  other run of characters one `_`, trailing `_` dropped, `t_` prefixed when empty or starting with a digit.
- **`is_signed_kind()`**, **`scalar_ctor()`** (`wireshark.cpp:83-128`): the `ProtoField` constructor of a scalar
  leaf (`int8`…`uint64` with `base.DEC`, `float`, `double`, `bool`, character kinds and `byte` as hex integers).
- **`struct generator`** (`wireshark.cpp:130-409`): the dictionary, the `ProtoField` list `fields`, the type
  segments `tsegs`, and the output string.
  - `add_field()` (`wireshark.cpp:137-143`): appends a constructor, returns its 1-based Lua index.
  - `enum_table()`, `enum_ctor()` (`wireshark.cpp:145-171`): the `E_<hash>` table name; the integer `ProtoField` of
    an enum, with its value table for sizes 1, 2 and 4, without one for 8.
  - `field_table()` (`wireshark.cpp:173-220`): the Lua table of one field (`n`, `k`, `o`, `s`, `a`, `th`, `x`, `pf`,
    `str`, `e`), recursive over element and alternative descriptors, assigning `ProtoField`s on the way.
  - `call_name()` (`wireshark.cpp:222-254`): the text of a call signature for `serreg.call.name`; a
    constant argument as `const <type>=0x<value>`; a log call is spelled `log(…)`.
  - `assign_segments()` (`wireshark.cpp:256-269`): one unique filter segment per type.
  - `run()` (`wireshark.cpp:271-408`): builds `enums`, `types`, `calls` (each with its `name` and, for a
    constant argument, its value bytes `v`), `lits`,
    `scalars`, then writes the header comment, `SERREG`, the `Proto`, the enum tables, `PF`, `TYPES`, `CALLS`,
    `LITERALS`, `SCALARS` and the runtime.
- **`tools::wireshark_dissector()`** (`wireshark.cpp:413-417`): `generator{d}.run(o)`.
- **`lua_runtime_text`** (`wireshark.cpp:424-1337`), returned by `lua_runtime()` (`wireshark.cpp:1371-1374`):
  - constants `K` (with `MAP = 53`, `SET = 54` for 1.6), `KNAME`, `KSIZE`, the `AK_*` argument categories, `MAX_DEPTH = 64` (`wireshark.cpp:426-434`);
  - header and record fields `H` (`serreg.file.*`, `serreg.dict.*`, `serreg.frame.*`, `serreg.call.name`,
    `serreg.record.*`, `serreg.type`, `serreg.literal`, `serreg.arg.string`, `serreg.message`;
    `wireshark.cpp:436-461`);
  - scalar-argument fields `ARGF` (`serreg.arg.i8` … `serreg.arg.byte`) and the field registration
    (`wireshark.cpp:462-479`);
  - expert infos `X` (`wireshark.cpp:481-492`) and the `tcp_port`/`udp_port` preferences (`wireshark.cpp:494-495`);
  - helpers `align8`, `bit`, `hex`, `u32`, `u16`, `fail`, `need`, `stride`, `enum_value`, `enum_text`, `alt_text`
    (`wireshark.cpp:497-525`);
  - the slot rules `take()` and `slot()` (`wireshark.cpp:553-561`);
  - srdump-style inline text: `quote`, `number_text`, `float_text`, `scalar_inline`, `enum_inline`
    (`wireshark.cpp:538-599`);
  - `walk_value()` (`wireshark.cpp:604-709`; 1.6: the `K.MAP`/`K.SET` branch) and `walk_struct()`
    (`wireshark.cpp:712-737`; 1.6: the transparent branch);
  - `type_record()` (`wireshark.cpp:767-780`), `scalar_text()` (`wireshark.cpp:784-788`);
  - the print renderer (`wireshark.cpp:764-916`): `INT_KINDS`, `int_parts` (sign, `UInt64` magnitude and
    decimal text; 64-bit through `UInt64`/`Int64`), `digits` (bases 2, 8, 16), `parse_spec`, `pad`, `sign_of`,
    `format_int`, `format_float`, `format_string`, `format_arg`, `LOG_LEVELS`, `render(fmt, args)`;
  - `literal_item()` (`wireshark.cpp:944-948`);
  - `dissect_frame()` (`wireshark.cpp:925-1107`; the CRC trailer, checkpoint and writer-info branches at
    `wireshark.cpp:936-973`, the log prefix at `wireshark.cpp:1085-1093`), `dissect_rri()` (`wireshark.cpp:1110-1149`),
    `dissect_file_header()` (`wireshark.cpp:1184-1199`), `dissect_serreg()` (`wireshark.cpp:1201-1224`);
  - the `wtap_encap` USER0 registration (`wireshark.cpp:1232`);
  - the FILE reader: `le`, `rri_length`, `FileHandler` `reader` with `read_open`, `packet_at`, `fill`, `read`,
    `seek_read`, the close callbacks, `register_filehandler` (`wireshark.cpp:1234-1324`);
  - TCP (`serreg_tcp`, the local `dissect_serreg_tcp` with `dissect_tcp_pdus`) and UDP through the preferences
    (`wireshark.cpp:1294-1312`);
  - the heuristic dissectors (`wireshark.cpp:1314-1336`): `looks_like_frame`,
    `serreg:register_heuristic("udp", …)`, `serreg_tcp:register_heuristic("tcp", …)`.

## How it works

### Generation: one pass over the dictionary

```cpp
std::string wireshark_dissector(const dyn::dictionary& d, const wireshark_options& o)
{
    generator g{d, {}, {}, {}};
    return g.run(o);
}
```

(`wireshark.cpp:413-417`.) `run()` walks `d.types`, `d.calls` and `d.literals` (all `std::map`s, so the output is
deterministic for one dictionary) and collects the `ProtoField` constructors in `fields` as it goes; `PF` is written
after the tables that reference it by index, because it is complete only then. The header comment names
`o.generator` and every entry of `o.sources` (`srdump` passes `capture <file>`, `--dict <file>`, `--elf <file>`,
`--rri <file>`), and the counts of types, calls and literals. `SERREG` records the generator and library version,
`format_major`/`format_minor`, `hash_algo`, the program hash (the dictionary's, or `computed_program_hash()` when
the source had no header, e.g. an ELF section), the ABI tag, `wchar_size` (4 when the tag has none) and the hash of
`"string"` (the record hash of a full-mode string argument).

The generator name and each source go into Lua `--` comments. They pass
through `lua_comment` first (T-03):

```cpp
const auto lua_comment = [](const std::string_view t) {
    std::string c(t);
    for (char& ch : c)
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e)
            ch = '?';
    return c;
};
```
(`wireshark.cpp:368-374`.) Every byte outside 0x20..0x7e becomes `?`. A capture file name containing a newline
would otherwise end the comment, and the rest of the name would become Lua code that Wireshark runs when it loads
the dissector
(`runtime_selftest.cpp` writes a capture named `….x\nprint("INJECTED")\n--.ser` and requires `?print("INJECTED")?`
in the output).

### Filter names

```cpp
void assign_segments()
{
    std::set<std::string> used;
    for (const auto& [h, t] : d.types)
    {
        std::string seg = filter_segment(t.name);
        if (!used.insert(seg).second)
        {
            seg += "_" + hexkey(h).substr(0, 8);
            used.insert(seg);
        }
        tsegs[h] = seg;
    }
}
```

(`wireshark.cpp:250-263`.) A struct member's field is `serreg.<type segment>.<member segment>`
(`serreg.corpus_Order.qty`); an enum type used as a top-level argument gets `serreg.<type segment>`. `filter_segment`
turns `corpus::Order` into `corpus_Order` and `wide::tick<7>` into `wide_tick_7`. Two types whose names map to one
segment (two versions of a type, which have different schema hashes, or names that differ only in punctuation) are
kept apart by appending the first 8 hex digits of the schema hash to the second one in hash order.

Members of one type get the same treatment (T-13), in `run()`:

```cpp
std::set<std::string> fsegs;
for (std::size_t i = 0; i < t.fields.size(); ++i)
{
    const auto& f = t.fields[i];
    std::string seg = filter_segment(f.name);
    if (!fsegs.insert(seg).second)
    {
        seg += "_" + dec(i);
        fsegs.insert(seg);
    }
    types += "    " + field_table(f, base + "." + seg, f.name, 0) + ",\n";
}
```
(`wireshark.cpp:301-312`.) `filter_segment` maps `y` and `y_` (or `a__b` and `a_b`) to one segment; the later
member now gets `_<index>` appended (`serreg.t_Y.y`, `serreg.t_Y.y_1`), so each member has its own field. Before,
both members registered the same filter name and a filter on one matched the other.

### Field tables and `ProtoField`s

```cpp
const bool str = f.k == kind::array_ && f.elem.size() == 1 &&
                 (f.elem[0].k == kind::char_ || f.elem[0].k == kind::char8);
if (!assign)
    ; // the elements of a char array: shown through the array's string field
else if (is_scalar_kind(f.k))
    s += ", pf = " + dec(add_field(scalar_ctor(f.k, abbr, label)));
```

(`wireshark.cpp:184-189`.) `field_table` emits the descriptor the runtime walks: name, kind, offset, size,
alignment, the nested type hash `th`, the array extent or variant alternative count `x`, and `e`, the element
descriptors (one for array, vector and optional; `x` alternatives for a variant; (1.6) the key and the value of
a map, the key of a set, with `x` the entry stride). A scalar, enum or string leaf gets
a `pf` index; a `char`/`char8` array becomes one `ProtoField.string` (`str = true`) and its elements get none, which
is how `char sym[8]` is shown as `AAPL`. Vector, array, optional and set elements reuse the member's field name, so
`serreg.x.xs == 3` matches any element; variant alternative `i` gets `<member>.alt<i>`; a map's key and value get
`<member>.key` and `<member>.value` (labels `<member> key` / `<member> value`, `wireshark.cpp:204-210`), so
`serreg.corpus_DynMap.by_name.key == "b"` finds an entry. A type's `TYPES` entry carries `transparent = true` for
a container argument's type (`wireshark.cpp:304`), and the unnamed field of such a type takes the segment
`value` (`wireshark.cpp:314`). A descriptor nested deeper
than 32 levels is emitted without children (a corrupt dictionary; the runtime reports the missing element
descriptor).

### Enumerators and 53 bits

```cpp
for (const auto& [name, v] : t.enumerators)
{
    constexpr std::int64_t exact = std::int64_t{1}
                                   << 53; // Lua 5.2 numbers are doubles
    if (v > exact || v < -exact)
        continue;
```

(`wireshark.cpp:276-281`.) Lua 5.2 has only doubles, so an enumerator outside ±2^53 is left out of the `E_<hash>`
table and its value is shown as a number. Wireshark's Lua value strings are 32-bit: an 8-byte enum's `ProtoField`
has no value table, and the runtime appends the enumerator name to the item text instead (`walk_value`,
`walk_struct`).

The number itself is exact (T-02):

```cpp
local function enum_text(r, t)
  local signed = t and t.u >= K.I8 and t.u <= K.I64
  if r:len() == 8 then return tostring(signed and r:le_int64() or r:le_uint64()) end
  return tostring(int(signed and r:le_int() or r:le_uint()))
end
```
(`wireshark.cpp:543-547`.) An 8-byte value is turned into decimal text by `tostring` of Wireshark's `Int64`/`UInt64`,
whose digits are exact; a Lua number is not above 2^53, so a value above it would be shown rounded.
`enum_inline` and `type_record` (the text of an enum argument, which the print renderer formats) use
`enum_text`; `enum_value` (a Lua number) is still what looks up the
enumerator name. srdump's JSON gives such a value as a string (`json_int64`), which `parity.py` accepts;
`print_capture` has a 64-bit enum line with a value above 2^53.

### The slot rules, in Lua

```cpp
local function take(ctx, off, n, bytes)
  if n == 0 then return off == 0 end
  if off < ctx.fixed or off > ctx.len or bytes > ctx.len - off or bytes > ctx.left then return false end
  ctx.left = ctx.left - bytes
  return true
end
```

(`wireshark.cpp:555-560`.) This is `fmt::image_budget::take` (`format.hpp`) line for line: `n == 0` needs
`off == 0`; a block starts at or after the root's fixed part (`ctx.fixed`), lies inside the image (`ctx.len`), and
all blocks of one image together fit in its size (`ctx.left`). `walk_value` calls it for every string, vector,
optional and variant slot, with the same byte counts as `src/decoder.cpp` (`n` for a string, `n × stride` for a
vector, the element size for an optional, the alternative's size for a variant). A broken rule raises
`fail(...)`, a Lua `error({ serreg = msg })` caught by `pcall` per record; the record is marked
`serreg.malformed` and the dissector goes on with the next frame. `need()` checks every read against the buffer
before it is made, and `MAX_DEPTH` bounds recursion.

### One typed record

```cpp
local function type_record(tvb, tree, p, len, t, label)
  local ctx = { tvb = tvb, base = p, len = len, fixed = t.enum and len or t.size, left = len }
  if not t.flat and not t.enum then
    if len < 8 then fail("dynamic record prefix") end
    local ilen = u32(tvb, p)
    if u32(tvb, p + 4) ~= 0 or ilen + 8 ~= len then fail("dynamic record prefix") end
    ctx.base, ctx.len, ctx.left = p + 8, ilen, ilen
  end
```

(`wireshark.cpp:767-774`.) A flat record is the object's bytes; a dynamic record is `u32 len, u32 0` and the
canonical image, whose offsets are relative to the image start (`ctx.base`). `walk_struct` adds one tree item per
member (through `PF[f.pf]`, so every value is filterable) and returns srdump's one-line text of the value
(`corpus::Order{id: 42, …}`, enumerators as `Type::name (value)`, strings quoted with `\0`, `\n`, `\xNN` escapes).
For an enum record `type_record` returns the bare enumerator name, which is what a print message shows.

### Maps, sets and transparent types (1.6)

```cpp
local sub = tree:add(tvb(abs, 8), label .. (f.k == K.MAP and ": map[" or ": set[") .. n .. "]")
for i = 0, n - 1 do
  local at = ctx.base + off + i * st
  if f.k == K.MAP then
    local ent = sub:add(tvb(at, st), "[" .. i .. "]")
    local kt = walk_value(ctx, ent, at, kf, "key", depth + 1)
    local vt = walk_value(ctx, ent, at + vf.o, vf, "value", depth + 1)
    items[#items + 1] = kt .. ": " .. vt
  else
    items[#items + 1] = walk_value(ctx, sub, at, kf, name, depth + 1)
  end
end
return "{" .. table.concat(items, ", ") .. "}"
```

(`wireshark.cpp:674-686`.) The `K.MAP`/`K.SET` branch of `walk_value` takes the block through `take` with `n × x`
bytes (`x` is the FIELD's entry stride, refused when missing or 0) and adds one subtree `member: map[n]` /
`member: set[n]`; a map entry is an `[i]` item holding the key at the entry's start and the value at `vf.o`, each a
filterable field of its own, and the text is `{k: v, …}` or `{a, b}` as srdump prints it (a nested dynamic value
inside an entry goes through the same walker and renders in the text, `docs/deviations.md`). `walk_struct`
(`wireshark.cpp:724-729`) takes a `transparent` type's single field straight into the parent tree under the
record's own label with no struct item around it, so a top-level `std::vector<i32>` record shows as `ids: [3, 1,
2]` and renders in a print message as its value. The parity test runs the dissector over the golden
`dynamic.ser` (frames 5–9) against srdump's output.

### Frames: the call hash decides

```cpp
local sig = CALLS[call]
if sig then ft:add(H.call_name, tvb(off + 8, 8), sig.name) end
local texts, scal, summary = {}, {}, {}
local p = off + 16
local full = bit(flags, 1)
```

(`wireshark.cpp:1007-1010`.) `dissect_frame` reads the 16-byte header (`frame_len` must be ≥ 16, a multiple of 8
and inside the buffer), adds the header fields and — when the call is in `CALLS` — `serreg.call.name`, and
branches:

- **CRC trailer** (flag value 4, 1.7): the last 8 bytes are shown as `crc32c 0x…` and the records end before them;
  the dissector does **not** recompute the checksum (the C++ and Python readers do and skip a frame that fails);
- **dictionary frame** (flag value 2): the payload is an RRI — a full image or a 1.7 delta, followed in 1.7 by the
  8-byte forward link — handed to `dissect_rri`;
- **checkpoint frame** (flag value 8, 1.7): a `checkpoint` subtree with the previous and next checkpoint offsets,
  the data frames and bytes before it, the wall clock and the latest dictionary's offset; Info `checkpoint`;
- **writer-info frame** (flag value 16, 1.7): a `writer info (N items)` subtree, one line `key K: text` per item;
- **compact frame**: without an entry in `CALLS` the records cannot be delimited, so they are shown as one block
  with `serreg.unknown_call`; with one, each argument is read in signature order, aligned to 8 — a literal consumes
  nothing and shows `LITERALS[id]`, a constant consumes nothing and is shown from the call's entry (below),
  a scalar `KSIZE[k]` bytes through `ARGF[k]`, a string `u32 len` + bytes, a type
  its fixed size or `8 + u32 len` for a dynamic type. A record count that differs from the signature raises
  `serreg.record_count`;
- **full-records frame**: each record carries `u64 hash, u32 len`; the hash is looked up as a literal, the string
  hash, a scalar name hash (`SCALARS`) or a type, and an unknown hash is shown as bytes with `serreg.unknown_type`.

Per record the frame keeps `texts[i]` (the argument as srdump's text shows it: a literal's text, a string, a
scalar's `scalar_text`, a type record's inline text or enumerator name) and, for a top-level scalar, `scal[i] = { r
= TvbRange, k = kind }`, so the print renderer can format the value's bytes rather than its text. A call
whose flags mark it as `print`/`println` renders the message with `render(texts[1], args)`, where `args[i-1]` is
`{ text, r, k }` for a scalar and `{ text }` otherwise (below). The message goes to `serreg.message` and the Info
column; otherwise the Info column lists the arguments (`"new order", corpus::Order`).

**Log frames.** A call whose flags carry `log` (value 4) is a print call whose first arguments are the
envelope:

```cpp
local first, prefix = 1, ""
if sig and bit(sig.flags, 4) and #sig.args >= 6 then
  local mask = tonumber(texts[4]) or 0
  first = 6 + (bit(mask, 1) and 1 or 0) + (bit(mask, 2) and 1 or 0)
  local lvl = LOG_LEVELS[tonumber(texts[1]) or 0] or tostring(texts[1])
  prefix = lvl .. " " .. tostring(texts[2]) .. ":" .. tostring(texts[3]) .. " "
```
(`wireshark.cpp:1119-1124`.) The level, file and line are constants of the call, shown from its entry; the format
literal is argument `first` (6, 7 or 8 by the mask), and the message becomes `LEVEL file:line [tid] #seq message`.
Unlike srdump the dissector shows no timestamp in the prefix (the `u64` time is one of the records in the tree)
and the file literal as written, not its basename. The frame kind reads `compact, log`.

### Constants of a call (format 1.5)

A constant argument has no bytes in a compact frame, so the dissector cannot point at a range of the packet. The
generator therefore writes the value into the `CALLS` table:

```cpp
if (a.ak == fmt::arg_const)
{ // 1.5: the 8 value bytes in wire order, for ByteArray.new()
    std::string v;
    for (unsigned b = 0; b < 8; ++b)
    {
        static constexpr char digits[] = "0123456789abcdef";
        const unsigned byte = static_cast<unsigned>((a.value >> (8 * b)) & 0xff);
        v += digits[byte >> 4];
        v += digits[byte & 15];
    }
    calls += ", v = \"" + v + "\"";
}
```
(`wireshark.cpp:328-339`.) An argument entry of a constant is `{ ak = 4, h = "<hash>", k = <scalar kind>, v =
"<16 hex digits>" }`; `k` is left out for an enum constant, as for a type. `v` is the `u64 value` of the call's
ENTRY byte by byte, lowest byte first, so `42` is `"2a00000000000000"`.

```cpp
elseif a.ak == AK_CONST then
  -- 1.5: a compile-time constant of the call: no bytes in the frame; the value comes from the call's
  -- entry and is shown (as a generated item, from its own 8-byte data source) as the scalar or enum it is
  local t = (not a.k) and TYPES[a.h] or nil
  local sz = a.k and KSIZE[a.k] or (t and (t.flat or t.enum) and t.size)
  if not sz or sz > 8 then fail("constant of unknown type 0x" .. a.h) end
  local ctvb = ByteArray.new(a.v):tvb("serreg constant " .. label)
```
(`wireshark.cpp:1035-1041`.) The runtime turns `v` into a `Tvb` of its own (`ByteArray.new(a.v):tvb(...)`), which
Wireshark shows as a second data source next to the packet bytes. A scalar constant is added through the same
`ARGF[k]` field as a run-time scalar argument, marked generated (`set_generated()`), and keeps `scal[i]`, so a
print message formats it like any scalar. An enum constant goes through `type_record` on that `Tvb` and shows its
enumerator name. `p` does not advance. An enum that is not in `TYPES`, or a type larger than 8 bytes, raises the
frame's error ("constant of unknown type").

In a full-records frame a constant is an ordinary scalar or enum record on the wire, and the existing full-record
branch shows it from the packet bytes; nothing constant-specific is needed there. `serreg.call.name` lists a
constant as `const i32=0x000000000000002a` or `const corpus::Side=0x0000000000000002` (`call_name`,
`wireshark.cpp:229-237`): the type it stands for and the stored value.

The Lua stub used by the tests without Wireshark gained `ByteArray.new(hex):tvb(name)` for this
(`tests/wireshark/wslua_stub.lua`, lines 130-136).

### Print messages: std::format replacement fields in Lua

```cpp
local function render(fmt, args)
  local auto, out = 0, {}
  local pos = 1
  while pos <= #fmt do
    local c = fmt:sub(pos, pos)
    if c == "{" and fmt:sub(pos + 1, pos + 1) == "{" then out[#out + 1] = "{"; pos = pos + 2
    elseif c == "}" and fmt:sub(pos + 1, pos + 1) == "}" then out[#out + 1] = "}"; pos = pos + 2
    elseif c == "{" then
      local close = fmt:find("}", pos, true) or #fmt
      local field = fmt:sub(pos + 1, close - 1)
      local id, spec = field:match("^(%d*):?(.*)$")
      local idx
      if id ~= "" then idx = tonumber(id) + 1 else auto = auto + 1; idx = auto end
      out[#out + 1] = format_arg(args[idx], spec or "") or ("{" .. field .. "}")
      pos = close + 1
    else out[#out + 1] = c; pos = pos + 1 end
  end
  return table.concat(out)
end
```

(`wireshark.cpp:924-942`.) The runtime renders a replacement field as `std::format` does, so Wireshark's message equals
srdump's. `{{`/`}}` are literal braces; a field is `{[arg-id][:spec]}`, numbered or automatic. `format_arg` turns a
field that it cannot render into `nil`, and the field is then shown as written (`{…}`), never as a wrong value.

- **`parse_spec`** (`wireshark.cpp:823-844`) reads `[[fill]align][sign][#][0][width][.precision][L][type]` into
  `{ fill, align, sign, alt, zero, width, prec, type }`. A nested replacement field (`{:{}}`), a `.` without
  digits or a type of more than one character returns `nil`. `L` is accepted and ignored (the C locale).
- **`pad`** applies width, fill and alignment; numbers default to right alignment, text to left; `0` pads between
  the sign/base prefix and the digits, and only when no alignment was given, as in `std::format`. **`sign_of`**
  gives `-`, `+` or a space.
- **Integers** (`format_int`): `int_parts` returns sign, magnitude and decimal text. An 8-byte value goes through
  Wireshark's `UInt64`/`Int64` (Lua 5.2 numbers are doubles and would lose digits above 2^53): the magnitude of a
  negative `i64` is `UInt64(0) - u`, and `digits` converts a `UInt64` to base 2 or 8 by repeated `%`/`/` and to
  base 16 through `tohex()` (each digit through `int()`: under Lua 5.4 `tonumber()` is a float). Types `d x X o b B`; `#` adds `0x`/`0X`/`0b`/`0B`/`0` (none for an octal zero). The
  character kinds `char16`/`char32`/`wchar` and `byte` format as integers.
- **Floating point** (`format_float`): `{}` without precision is `float_text` (the shortest round trip, which
  srdump's text uses too); otherwise `string.format` with the type `a A e E f F g G` and precision 6 by default
  (none for `a`/`A`), and the `0x` that C's `%a` prints removed, as `std::format` omits it. NaN and infinities are
  `nan`/`inf` (upper case for the upper-case types) and never zero-padded; `-0.0` keeps its sign.
- **`bool`** is `true`/`false` for `{}`/`{:s}` and `1`/`0` through `format_int` for an integer type; **`char`** is
  the character for `{}`/`{:c}` and its code otherwise. **Everything else** (literals, strings, enumerator names,
  structs as srdump's inline text) is `format_string`: type `s` or none, precision truncates, width pads.

`tests/wireshark/print_capture.cpp` (target `wireshark_print_capture`) writes 15 print lines covering these specs,
and the parity check `prints:` requires every rendered message to equal srdump's.

### `serreg.call.name`

```cpp
std::string call_name(const dyn::call& c) const
{
    std::string s = c.is_print() ? (c.newline() ? "println(" : "print(") : "(";
    for (std::size_t i = 0; i < c.args.size(); ++i)
    {
        const auto& a = c.args[i];
        if (i)
            s += ", ";
```

(`wireshark.cpp:216-223`.) Each `CALLS` entry carries `name`, the signature as text: literals quoted with their text
(`lit#<hash>` when the dictionary lacks it), scalars by kind name (`f64`, `u32`), strings as `string`, types by
name (`0x<hash>` when unknown): `("new order", corpus::Order)`, `println("px={:.2f} qty={}", f64, u32)`.
`dissect_frame` adds it as the string field `serreg.call.name` on the call-hash bytes, so a display filter can
select frames by signature (`serreg.call.name contains "Order"`). A frame whose call is not in `CALLS` has no
`serreg.call.name`.

### The capture's dictionary: compared, not used

```cpp
local h = hex(tvb, p + 8)
local have = (tag <= 2 and TYPES[h]) or (tag == 3 and LITERALS[h]) or (tag == 4 and CALLS[h])
if have then
  known = known + 1
  if tag <= 2 and TYPES[h].lh ~= hex(tvb, p + 16) then
```

(`wireshark.cpp:1159-1163`.) `dissect_rri` walks the ENTRY chain of an RRI — the FILE's dictionary, a dictionary
frame, or a back-patched file's relocated dictionary — and counts entries known and unknown to the generated
tables (`serreg.dict.known`, `serreg.dict.unknown`, with up to five unknown names and `serreg.dict.unknown_entries`).
A type known under its schema hash but with another layout hash raises `serreg.dict.layout_mismatch` (review
FR-R4); a different program hash is the note `serreg.dict.program_differs`, a different ABI tag the warning
`serreg.abi_differs` (also checked on the FILE header). The frames are still decoded from the generated tables.

### Where frames come from

```cpp
if bit(flags, 2) and dict_len > 0 then
  if minor >= 7 then st.first = align8(48 + dict_len) + 16 -- 1.7: the CHAIN follows the opening dictionary
  elseif dict_off == 48 then st.first = align8(48 + dict_len)
  else
    local old = rri_length(file, 48)
    if not old then return false end
    st.first, st.stop, st.reloc, st.reloc_len = align8(48 + old), dict_off, dict_off, dict_len
  end
end
```

(`wireshark.cpp:1262-1270`.) The `FileHandler` reader (extension `ser`) opens FILE captures: `read_open` accepts the
`ESSR` magic, and packet 1 is the 48-byte header plus the leading dictionary (`dissect_file_header` →
`dissect_rri`; for a 1.7 file the packet also covers the CHAIN, and the writer-info frame is packet 2), then one
packet per frame — dictionary deltas, checkpoints and data frames in file order (`packet_at`: a frame length that is not ≥ 16, a multiple of 8 and
inside the frame area makes the rest one malformed packet). The reader does not follow the 1.7 chains: it walks
sequentially, like srdump. For a back-patched 1.6 file (`dict_offset ≠ 48`) the
original RRI at 48 is measured from its own entry chain (`rri_length`), frames stop at `dict_offset`, and the
relocated dictionary is the last packet — the same decision `src/reader.cpp` makes (the offset, not the flag
bit). Frames have no timestamps. The encapsulation is USER0, which is also registered in `wtap_encap`, so a
pcap/pcapng with link type 147 carrying one or more frames per packet (dictionary frames too) is dissected the same
way. `dissect_serreg` handles a buffer that starts with `ESSR` (file header), `ESRG` (a bare RRI) or frames, and
catches any error into one `serreg.malformed` item.

```cpp
local serreg_tcp = Proto("serreg_tcp", "es::serreg over TCP")
local function dissect_serreg_tcp(tvb, pinfo, tree)
  dissect_tcp_pdus(tvb, tree, 16, function(t, _, off) return t(off, 4):le_uint() end, dissect_serreg)
  return tvb:len()
end
serreg_tcp.dissector = dissect_serreg_tcp
```

(`wireshark.cpp:1327-1332`.) TCP streams are reassembled with `dissect_tcp_pdus`: 16 header bytes are needed, the
PDU length is the frame's `u32 frame_len`. UDP datagrams (one or more frames each) go straight to
`dissect_serreg`. Both ports are preferences (`serreg.tcp_port`, `serreg.udp_port`, 0 = off); `prefs_changed`
moves the registrations in `tcp.port`/`udp.port` when they change. The TCP dissector is a named local function
so that the TCP heuristic can call it directly.

### Heuristics: frames recognised without a port

```cpp
local function looks_like_frame(tvb)
  if tvb:len() < 16 then return false end
  local flen, flags = tvb(0, 4):le_uint(), tvb(4, 2):le_uint()
  if flen < 16 or flen % 8 ~= 0 or flags > 31 then return false end
  local call = string.lower(tvb(8, 8):le_uint64():tohex())
  return CALLS[call] ~= nil or (bit(flags, 2) and call == "4553535244494354")
      or (bit(flags, 8) and call == "45535352434b5054") or (bit(flags, 16) and call == "45535352494e464f") -- 1.7 meta
end
```

(`wireshark.cpp:1351-1358`.) A buffer is taken as es::serreg when its first 16 bytes are a frame header whose call
hash is in this dissector's `CALLS` table, or a meta frame with its sentinel: a dictionary frame (flag 2,
`"ESSRDICT"` = `0x4553535244494354`) or a checkpoint (flag 8, `"ESSRCKPT"`) or writer-info frame
(flag 16, `"ESSRINFO"`). The flag test admits the five 1.7 bits (`flags > 31` rejects). With 64-bit call hashes a random match is not a practical concern, and a buffer is never
claimed for a call the dissector could not decode. `serreg:register_heuristic("udp", …)` also requires the first
frame to fit in the datagram and hands the datagram to `dissect_serreg`. `serreg_tcp:register_heuristic("tcp", …)`
sets `pinfo.conversation = serreg_tcp`, so the rest of the TCP stream (including frames that start in the middle
of a segment, which the header test could not recognise) goes to `dissect_serreg_tcp` and its reassembly, and
dissects the first segment with `dissect_serreg_tcp` itself. By default Wireshark tries heuristic dissectors after
the port tables, so a port preference still takes precedence; users disable
them in Analyze → Enabled Protocols (`serreg_udp_heur`, `serreg_tcp_heur`).

## Maintaining it

- **Tests:** `wireshark_generate` (always: the generated file for `orders.ser` contains
  `ProtoField.uint32("serreg.corpus_Order.qty", "qty", base.DEC)`); `wireshark_dissector_parity`
  (`tests/wireshark/parity.py`; needs tshark and Python 3, disabled otherwise, `SKIP_RETURN_CODE 77` when tshark
  cannot run Lua — as root it runs tshark through `runuser -u nobody`). Its checks: `fixture` (orders.ser and
  dynamic.ser through the FILE reader — format 1.7 files with a writer-info frame), `backpatched` (built
  from `corpus/capture/orders-1.6.ser`, since a 1.7 file is never back-patched), `user0`, `udp` and `tcp` (with `text2pcap`; one frame split
  across two segments), `mismatch` (a dissector from orders.ser reading dynamic.ser: 2 known, 10 unknown entries,
  nothing decoded, expert infos), `fuzz` (3000 corrupted frames in one USER0 pcap: no Lua error), `elf` (dissectors
  generated with `--elf` from `make_fixtures` and `make_dynamic_fixtures`), and `prints:<writer>` (the
  capture `wireshark_print_capture` writes: 15 print lines of format specs, among them a line of `float`
arguments and a 64-bit enum value above 2^53), `udp-heuristic` and `tcp-heuristic`
  (the UDP and TCP captures on other ports, with no port preference set). Every value tshark shows, in order, must
  equal what `srdump --json` shows, and print messages must be equal. `example_embedded_cli_wireshark`
  checks a dissector generated from an application's own registry. `wireshark_dissector_lua52` and
  `wireshark_dissector_lua54` run the `fixture`, `backpatched`, `prints` and `elf` checks with the dissector
  under plain Lua 5.2 and 5.4 through `tests/wireshark/wslua_stub.lua` (no Wireshark needed). The
  golden `orders.ser` has three frames with constants (compact, full records, `println`), so the `fixture` check of
  the parity test and of both stub tests covers the `AK_CONST` branch. Run the parity test after any change here, to
  `src/decoder.cpp`, `src/formatters.cpp` or the wire format.
- **Keep the runtime Lua 5.2 and 5.4 compatible.** Wireshark up to 4.4 uses Lua 5.2, 4.6 and later 5.4. No
  `bit32` (gone in 5.4), no integer operators (`//`, `&`, `>>`: not in 5.2); flag bits are tested with `bit(v, b)`
  = `math.floor(v / b) % 2 == 1`. Numbers are only used below 2^53. 64-bit ids are compared as lower-case hex
  strings (`hex()` = `UInt64:tohex()` lowered; `hexkey()` on the C++ side), never as numbers. Generated string
  literals use decimal escapes only. **Lua 5.4 keeps integers and floats apart**: `UInt64`/`Int64:tonumber()`
  returns a float there and `tostring(7.0)` is `"7.0"`, so a whole number that becomes text or a table key goes
  through the runtime's `int()` (`math.tointeger` when it exists, a no-op on 5.2) — including the
  octal/binary digits of `digits()` (without it Wireshark 4.6.8 shows octal 137357 as `0.10.30.70.30.50.7`) and
  the value of an 8-byte enum (`enum_value()`, otherwise `7.0`). The parity test has run with Wireshark 4.2.2
  (Lua 5.2) here and 4.6.8 (Lua 5.4) on the reference box; `wireshark_dissector_lua52` and `_lua54` run the FILE-capture
  checks under both interpreters without Wireshark (`tests/wireshark/wslua_stub.lua`). Run both after any change to
  the runtime.
- **The print renderer must match `std::format` as srdump uses it.** srdump renders print frames with
  `std::vformat` (`src/formatters.cpp`); the Lua renderer reimplements the format-spec grammar for the argument
  kinds a print call can carry. A spec the renderer does not support must return `nil` (shown as written), never
  an approximation. Extend `tests/wireshark/print_capture.cpp` with a line for every newly supported spec; the
  `prints:` check compares the messages. 64-bit integers go through `UInt64`/`Int64`, never through Lua numbers.
- **The heuristics claim only frames the dissector knows.** `looks_like_frame` accepts a call hash from `CALLS` or
  the dictionary-frame sentinel, nothing looser; widening it (e.g. to any header that looks plausible) would make
  the dissector claim unrelated UDP/TCP traffic.
- **The slot rules must match `format.hpp` and `src/decoder.cpp`.** `take()` is `fmt::image_budget::take`; the
  byte counts per kind, the variant/optional rules and the dynamic record prefix check are those of
  `decoder.cpp`. A change to either is a change here, in the same commit.
- **When the wire format changes, the runtime changes with `format_minor`.** Header offsets (FILE 48 bytes, RRI
  40 bytes, frame 16 bytes, record header 16 bytes), flag values, the ARG categories, the `kind` numbers in `K` and
  the ENTRY tags in `dissect_rri` are copies of `format.hpp`/`descriptor.hpp`; `SERREG.format_minor` records which
  format a file was generated for. A dissector is a snapshot: regenerate it when the program's types change.
- **Generated names are user-visible.** Filter names (`serreg.<type>.<member>`, `.alt<i>`, the hash suffix on a
  type clash, the `_<index>` suffix on a member clash), the `H` field names (including `serreg.call.name` and the text `call_name` produces) and the
  expert-info names are documented in `docs/wireshark.md`; users keep filters
  and coloring rules with them. Renaming one is a documented change.
- **Nothing from the input reaches the Lua code unescaped.** Names and texts go into string literals through
  `lua_str`, and text in comments through `lua_comment` (T-03). A new place that writes dictionary or
  command-line text into the generated file must use one of the two.
- **Never decode with the capture's dictionary.** Frames are matched by call hash (compact) or record hash (full)
  against the generated tables only; that is what makes a wrong layout impossible.
- **Allocation and threading:** tools layer; `wireshark_dissector` builds one `std::string` (proportional to the
  dictionary plus the fixed runtime of about 38 KB), allocates freely, keeps no state, and is reentrant.
