# 7. The wire format, byte by byte

Every dump in this chapter is taken verbatim from the checked-in golden fixtures of this release
(`tests/fixtures/golden/orders.ser`, `dynamic.ser`, `features.ser`), so you can reproduce it with `srdump --hex` or
a hex editor. All integers are little-endian. `include/es/serreg/format.hpp` is normative; this chapter is the
guided tour. Wire format **1.7**. The minors that led here: 1.3 brought XXH3-64 fingerprints (`hash_algo` 2) and
the writer's ABI tag in the FILE and RRI headers; 1.4 changed canonical names, and with them some schema hashes, but
no byte layout; 1.5 added compile-time constants as call arguments; 1.6 added the FIELD kinds `map` (53) and `set`
(54), the struct ENTRY flag `transparent` (a container passed as an argument) and the enum ENTRY's reserved byte as
`flags` with bit 0 = `fixed`; 1.7 added integrity and crash safety for captures of any size (§7.9): the CHAIN after a file's opening dictionary,
dictionary **deltas** written inline and linked forward, an optional CRC32C trailer per frame, checkpoint frames,
the writer-info frame, and the call flag `log` (chapter 3 §3.9). Readers accept **1.6 and 1.7**; any other minor is
`bad_version` (§7.7). Every 1.7 feature is opt-in: with the default options a 1.7 file differs from the 1.6 file
of the same program only in the headers' minor, the 16-byte CHAIN and the writer-info frame — and in that nothing
is ever rewritten at close.

The golden `orders.ser` is 2296 bytes: the 48-byte FILE header, a 1504-byte dictionary of 20 entries (3 types,
8 literals, 9 calls), the 16-byte CHAIN, a 152-byte writer-info frame and 11 data frames holding 34 records.
`orders.rri` is that dictionary alone, 1504 bytes. The golden `dynamic.ser` is 3960 bytes: the header, a
2616-byte dictionary of 20 entries (9 types, 4 literals, 7 calls), the CHAIN, a writer-info frame and 9 data frames
holding 18 records; the last five are a map/set message, a top-level vector compact, empty and with full records,
and a `println` with a vector and a map (§7.5b). The golden `features.ser` is 7144 bytes: the same 11 data frames
as `orders.ser`, each with a CRC32C trailer, and three checkpoint frames, each carrying the full dictionary (§7.9).

## 7.1 The three carriers of one dictionary

The same ENTRY blobs appear in three places: the ELF section `es_serreg_dict` of every binary that instantiates
`serialize()` for a type, the **RRI** (Raw Registry Image = 40-byte header + ENTRY stream) exported by a process
or written into a capture, and the dictionary frames of a stream or (1.7) of a file. The bytes of an ENTRY are
identical in all three — that is what makes `srdump --elf ./app` and `srdump capture.ser` agree. A 1.7 checkpoint
frame may carry a fourth copy (a full RRI, §7.9).

## 7.2 FILE header (48 bytes)

```
0000  45 53 53 52 01 00 07 00 23 00 00 00 02 00 00 00   ESSR....#.......
0010  b7 ec bc 65 9e b3 bd d6 30 00 00 00 00 00 00 00   ...e....0.......
0020  e0 05 00 00 00 00 00 00 01 00 08 08 04 01 10 10   ................
```

| offset | field | value in the dump |
|---|---|---|
| 0 | `char[4]` magic | `"ESSR"` |
| 4 | `u16 major` | 1 |
| 6 | `u16 minor` | 7 (format 1.7) |
| 8 | `u32 flags` | `0x23` = bit0 little-endian, bit1 dictionary present, bit5 writer-info frame (1.7). The other bits: bit2 back-patched (1.6 files only), bit3 every data frame has a CRC trailer, bit4 the writer emits checkpoints — `features.ser` has `0x3b` = bits 0, 1, 3, 4, 5 |
| 12 | `u32 hash_algo` | 2 = XXH3-64 (seed 0, default secret; `hash.hpp`) |
| 16 | `u64 program_hash` | `0xd6bdb39e65bcecb7` (FR-H3: XXH3-64 of the sorted type schema hashes) |
| 24 | `u64 dict_offset` | 48 — the RRI follows the header immediately |
| 32 | `u64 dict_len` | 1504 (`e0 05`) |
| 40 | `u64 abi_tag` | `01 00 08 08 04 01 10 10`: tag version 1, little-endian, pointer 8, `long` 8, `wchar_t` 4, `char` signed, `max_align_t` 16, `long double` 16 (x86-64 Linux) |

After the RRI, padded to 8 (here at 48 + 1504 = 1552, already aligned), come the 16 bytes of the CHAIN (§7.2b), then
the writer-info frame at 1568 and the first data frame at 1720. The header is written once, at `open()`, and never
again: `dict_offset` is always 48 in a 1.7 file. If types are registered after `open()` (a `dlopen` during
capture), the writer puts a **dictionary delta** frame before the first frame that needs it and links it from the
CHAIN (§7.9).

A 1.6 writer's `close()` instead appended a fresh, complete RRI at the end, rewrote `dict_offset`/`dict_len` and set
flag bit 2 ("back-patched"); a reader then measures the original dictionary area at 48 from its own entry chain and
stops the frames at `dict_offset`. The readers still do that for a 1.6 file; no 1.7 writer sets bit 2.

## 7.2b The CHAIN and the writer-info frame (1.7)

```
0610  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   CHAIN: next_dictionary 0 | first_checkpoint 0
0620  98 00 00 00 10 00 00 00 4f 46 4e 49 52 53 53 45   frame_len 152 | flags 0x10 (writer info) | 0 records | "ESSRINFO"
0630  01 00 09 00 01 00 05 00 32 2e 32 2e 32 00 02 00   version 1 | 9 items | key 1 (library_version) len 5 "2.2.2" pad | key 2
0640  03 00 31 2e 37 00 03 00 0d 00 6d 61 6b 65 5f 66   len 3 "1.7" pad | key 3 (program) len 13 "make_f
0650  69 78 74 75 72 65 73 00 11 00 33 00 70 74 72 38   ixtures" pad | key 17 (abi) len 51 "ptr8
0660  20 6c 6f 6e 67 38 20 77 63 68 61 72 34 20 63 68    long8 wchar4 ch
0670  61 72 2d 73 69 67 6e 65 64 20 6d 61 78 2d 61 6c   ar-signed max-al
0680  69 67 6e 31 36 20 6c 64 62 6c 31 36 20 6c 65 00   ign16 ldbl16 le" pad
0690  0a 00 01 00 30 00 0b 00 01 00 30 00 0c 00 01 00   key 10 (crc) "0" pad | key 11 (checkpoint_bytes) "0" pad | key 12
06a0  30 00 0d 00 04 00 66 75 6c 6c 0e 00 0a 00 64 69   (checkpoint_ns) "0" pad | key 13 (dictionary) "full" | key 14 (sync) len 10 "di
06b0  63 74 69 6f 6e 61 72 79                           ctionary"
```

The **CHAIN** (`fmt::chain_size` = 16) sits at `align8(48 + dict_len)`: `u64 next_dictionary` (the first dictionary
delta frame) and `u64 first_checkpoint`. Both are written as zero and patched at most once, each with one aligned
8-byte `pwrite`, after the frame they point at has been written (and, under the default sync policy, synced). Here
both are 0: `orders.ser` was written without a `dlopen` and without checkpoints. In `features.ser` the second word
is `d8 07 00 00 00 00 00 00` = 2008, the first checkpoint.

The **writer-info frame** (frame flag bit 4, call hash `fmt::info_sentinel` = `"ESSRINFO"`, no records) follows
the CHAIN when the FILE flag bit 5 says so: `u16 version` (1), `u16 n_items`, then per item `u16 key`
(`fmt::info_key`), `u16 len`, the text, padded to 2; the frame is padded to 8. Keys 1–17: `library_version`,
`format`, `program`, `hostname`, `pid`, `start_wall_ns`, `scrub_padding`, `full_records`, `canonical_unordered`,
`crc`, `checkpoint_bytes`, `checkpoint_ns`, `dictionary` (`full` or `scoped:<scope name>`), `sync`, `clock` (a
logger's), `thread`, `abi` (the long ABI text); readers skip keys they do not know. The golden files are written
with `capture_options{.info_host = false}` and a fixed clock, so they carry no hostname, pid or start time and stay
byte-for-byte reproducible. `srdump --info` lists the items.

## 7.3 RRI header (40 bytes)

```
0030  45 53 52 47 01 00 07 00 00 00 00 00 02 00 00 00   ESRG............
0040  14 00 00 00 b7 ec bc 65 9e b3 bd d6 00 00 00 00   .......e........
0050  01 00 08 08 04 01 10 10                           ........
```

`"ESRG"`, `u16 major` 1, `u16 minor` 7, `u32 flags` 0 (bit0 = delta image: set on the RRI of a 1.7 dictionary
delta, §7.9), `u32 hash_algo` 2,
`u32 n_entries` 20, `u64 program_hash`, `u32 reserved`, `u64 abi_tag` (the same 8 bytes as in the FILE header).
The reader uses `n_entries` to bound the walk. The ABI tag travels with every RRI — in a capture, in a
dictionary frame on a pipe, and in `registry::export_image()` — so a reader always knows the writer's ABI.
A dictionary re-encoded by the tools (`dyn::to_rri`) keeps the tag of its source, not the tool's.

What the tag is for (FR-W4): member sizes and offsets are already in every FIELD and `long` is spelled by width
(`i64`/`i32`) in canonical strings, so a typed read across ABIs fails safely with `hash_mismatch`. The tag covers
what the dictionary does not say: `wchar` has no width in a call string (a top-level `wchar_t` argument is 4 bytes
on Linux and 2 on Windows — the dictionary walk sizes it from the tag), `char` signedness for rendering, and the
plain fact that the file came from another ABI (`srdump` prints an `abi … (differs from this host: …)` line). Two
dictionaries with different tags in one capture are an `abi_mismatch` problem for `--verify`.

## 7.4 ENTRY — one blob per type, literal or call

Every ENTRY starts with the same 8-byte header: `u32 total_len` (multiple of 8, zero padded), `u8 tag`
(1 struct, 2 enum, 3 literal, 4 call), `u8 version` (1), `u16 reserved`.

### struct `corpus::Cancel { u64 id; u32 reason; }` — 128 bytes

```
0000  80 00 00 00 01 01 00 00 19 c5 70 b7 43 68 af 35   total 128, tag 1, ver 1 | schema hash
0010  3c 67 e8 4f fd 25 c1 99 10 00 00 00 08 00 00 00   layout hash | size 16 | align 8
0020  01 00 00 00 00 00 0e 00 02 00 63 6f 72 70 75 73   flags 1 (flat) rsv | since 0 | deprecated 0 | name_len 14 | n_fields 2 | "corpus
0030  3a 3a 43 61 6e 63 65 6c 02 00 08 00 00 00 00 00   ::Cancel" | FIELD: name_len 2, kind 8 (u64), rsv, offset 0
0040  08 00 00 00 08 00 00 00 00 00 00 00 00 00 00 00   size 8 | align 8 | since 0 | deprecated 0 | n_elem 0 | type_hash 0
0050  00 00 00 00 00 00 00 00 69 64 06 00 07 00 08 00   … | extent 0 | "id" | FIELD: name_len 6, kind 7 (u32), rsv, offset 8
0060  00 00 04 00 00 00 04 00 00 00 00 00 00 00 00 00   size 4 | align 4 | since/deprecated | n_elem 0 | type_hash 0
0070  00 00 00 00 00 00 00 00 00 00 72 65 61 73 6f 6e   extent 0 | "reason" | pad
```

FIELD = `u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated, u16 n_elem,
u64 type_hash, u32 extent, name bytes, FIELD × n_elem`. Nested FIELDs describe array elements (`n_elem` 1),
vector/optional elements (1, format 1.2), variant alternatives (`extent`, format 1.2), and (format 1.6) a map's key
and value (`n_elem` 2) or a set's key (1), with `extent` the entry stride (§7.4b). Kind numbers are in
`descriptor.hpp`: scalars 1–17, `enum_` 32, `array_` 33, `struct_` 34, `string_` 48, `vector_` 49,
`optional_` 50, `variant_` 51, `literal_` 52 (reserved), `map_` 53, `set_` 54 (1.6).

### enum `corpus::Side : u8 { buy = 1, sell = 2 }` — 88 bytes

```
0000  58 00 00 00 02 01 00 00 e1 88 51 ca 7c 26 4e 07   total 88, tag 2 | schema hash
0010  7b 27 09 56 0b df cc fd 01 00 00 00 01 00 00 00   layout hash | size 1 | align 1
0020  05 01 00 00 00 00 0c 00 02 00 63 6f 72 70 75 73   underlying kind 5 (u8), flags 1 (fixed) | since | deprecated | name_len 12 | n_enumerators 2 | name
0030  3a 3a 53 69 64 65 01 00 00 00 00 00 00 00 03 00   … | ENUMERATOR: i64 value 1, u16 name_len 3
0040  62 75 79 02 00 00 00 00 00 00 00 04 00 73 65 6c   "buy" | value 2, name_len 4, "sel
0050  6c 00 00 00 00 00 00 00                           l" | pad
```

Byte 33 — `01` — is the enum's **flags** byte (1.6): bit 0, `fmt::enum_flag_fixed`, says the enum has a fixed
underlying type (`enum class Side : std::uint8_t`), so every value of that type is a value of the enum and the
typed readers do not range-check it (decision D, chapter 4 §4.1). In format 1.5 the byte was reserved and 0; an
enum without a fixed type (`enum Plain { a, b }`) has 0 there. The flag is in the blob only: the schema hash (a
function of the canonical string, not of the blob) does not depend on it, and neither does the program hash, which
hashes schema hashes. Other bits are refused by the dictionary parser.

### struct `std::vector<i32>` — the transparent type of a container argument (1.6) — 128 bytes

```
0000  80 00 00 00 01 01 00 00 45 eb 15 a0 f3 7e c8 02   total 128, tag 1 | schema hash 0x02c87ef3a015eb45
0010  eb 67 e1 f8 af f3 d4 03 08 00 00 00 04 00 00 00   layout hash | size 8 | align 4
0020  02 00 00 00 00 00 10 00 01 00 73 74 64 3a 3a 76   flags 2 (transparent; not flat) rsv | since 0 | deprecated 0 | name_len 16 | n_fields 1 | "std::v
0030  65 63 74 6f 72 3c 69 33 32 3e 00 00 31 00 00 00   ector<i32>" | FIELD: name_len 0 (unnamed), kind 49 (vector), rsv, offset 0
0040  00 00 08 00 00 00 04 00 00 00 00 00 01 00 00 00   size 8 | align 4 | since | deprecated | n_elem 1 | type_hash 0
0050  00 00 00 00 00 00 00 00 00 00 00 00 03 00 00 00   … | extent 0 | element FIELD: name_len 0, kind 3 (i32), rsv, offset 0
0060  00 00 04 00 00 00 04 00 00 00 00 00 00 00 00 00   size 4 | align 4 | since/deprecated | n_elem 0 | type_hash 0
0070  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   extent 0 | pad
```

A container passed as an argument (`serialize(f, "ids"_, ids)`, chapter 3 §3.3c) is a type like any other on
the wire, and this is its ENTRY: a struct whose canonical name is the container's vocabulary name, with exactly one
unnamed FIELD at offset 0 — the container's 8-byte slot — size 8, align 4, and flag bit 1,
`fmt::struct_flag_transparent`, which tells a renderer to show the value and not a struct around it. The
dictionary parser refuses a `transparent` entry that is flat, has more than one field, a named field, a field not
at 0, or a size other than 8. 8 + 34 + 16 + 32 + 32 = 122 → 128.

### 7.4b FIELDs of a map and a set (1.6)

From the ENTRY of `corpus::DynMap { u32 id; std::map<std::string,i32> by_name; std::set<u16> ids;
std::unordered_map<i32,std::string> names; std::map<u8,DynMsg> msgs; }` (464 bytes, schema hash
`0x1aa730519093de87`, size 36, align 4, flags 0): the `by_name` FIELD — 32 fixed bytes, the 7-byte name, then its
two element descriptors of 32 bytes each — at ENTRY offset 90 (0x5a), right after the `id` FIELD:

```
0058  69 64 07 00 35 00 04 00 00 00 08 00 00 00 04 00   …"id" | FIELD by_name: name_len 7, kind 53 (map), rsv | offset 4 | size 8 | align 4
0068  00 00 00 00 02 00 00 00 00 00 00 00 00 00 0c 00   since 0 | deprecated 0 | n_elem 2 | type_hash 0 | extent 12 (the entry stride) …
0078  00 00 62 79 5f 6e 61 6d 65 00 00 30 00 00 00 00   … | "by_name" | key FIELD: name_len 0, kind 48 (string), rsv, offset 0 (in the entry) …
0088  00 08 00 00 00 04 00 00 00 00 00 00 00 00 00 00   … | size 8 | align 4 | since | deprecated | n_elem 0 | type_hash 0 …
0098  00 00 00 00 00 00 00 00 00 00 00 03 00 08 00 00   … | extent 0 | value FIELD: name_len 0, kind 3 (i32), rsv, offset 8 (in the entry) …
00a8  00 04 00 00 00 04 00 00 00 00 00 00 00 00 00 00   … | size 4 | align 4 | since | deprecated | n_elem 0 | type_hash 0 …
00b8  00 00 00 00 00 00 00 00 00 03 00 36 00 0c 00 00   … | extent 0 | next FIELD: name_len 3 ("ids"), kind 54 (set), rsv, offset 12 …
```

A `map` FIELD is the member's 8-byte slot like a vector's, followed by **two** element descriptors — the key, at
offset 0 of an entry, and the value, at its canonical offset inside the entry (`align_up(key size, value align)`,
here 8 after the string's slot) — and `extent` holds the **entry stride**: the entry padded to the larger of the
two alignments (12 = 8 + 4 here). A `set` FIELD has one element descriptor (the key at 0) and the key's stride in
`extent`: `ids: set<u16>` has `extent` 2. Nothing else is needed: a dictionary-driven reader lays a block of `n`
entries out as `n × extent` bytes from the slot's `off`, the key at `+0` and the value at `+value offset` of each.
The other three members of the type: `ids: set<u16> @12`, extent 2, one `u16` element; `names: map<i32,string>
@20`, extent 12, elements `i32 @0` and `string @4`; `msgs: map<u8,struct<corpus::DynMsg>> @28`, extent 24,
elements `u8 @0` and `struct @4` (a 20-byte dynamic fixed part, type hash `0xb487de0c417a37e8`). The parser checks
every map/set FIELD: `size` 8, the right element count, the key at 0, a map's value at or after the key's end,
`extent` non-zero, a multiple of the entry alignment and at least the last element's end. The element descriptors'
`offset` is the one place a FIELD's offset is relative to an *entry*, not to the owner's image.

```
0000  20 00 00 00 03 01 00 00 35 72 84 a0 6d 4c 8e 19   total 32, tag 3 | u64 id = fingerprint64("evt")
0010  03 00 00 00 00 00 00 00 65 76 74 00 00 00 00 00   u32 len 3 | u32 rsv | "evt" | pad
```

The id is the XXH3-64 fingerprint of the text (FR-H7). This blob is the *only* static copy of the text
(`lit<S>::text()` and the registry view it at offset 24).

### call `("evt"_, Order)` — 56 bytes

```
0000  38 00 00 00 04 01 00 00 2e 3b e2 68 4d 49 21 20   total 56, tag 4 | u64 call hash
0010  02 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00   u16 n_args 2 | u16 flags 0 | u32 rsv | ARG: kind 1 (literal), rsv[7]
0020  35 72 84 a0 6d 4c 8e 19 00 00 00 00 00 00 00 00   u64 hash = literal id | ARG: kind 0 (type), rsv
0030  d2 1d 44 c6 d8 8c 7f 36                           u64 hash = schema hash of corpus::Order
```

The call hash is `fingerprint64("call(2){lit:0x198e4c6da0847235,0x367f8cd8c6441dd2}")` — the canonical call
string lists argument *categories*, so `serialize(out, "abc", 1)` and `deserialize(bytes, std::string&, int&)`
are the same call (§7.8).

### call `print(4){lit, string, i32, lit}` — 88 bytes (format 1.1)

```
0000  58 00 00 00 04 01 00 00 5e 56 00 1b 91 49 7f e9   total 88, tag 4 | call hash
0010  04 00 01 00 00 00 00 00 01 00 00 00 00 00 00 00   n_args 4 | flags 1 (print; 2 = newline) | rsv | ARG literal
0020  50 83 d4 e5 0d b1 10 b6 03 00 00 00 00 00 00 00   id("hello {} {} {}") | ARG kind 3 (string)
0030  f1 74 e7 48 46 19 ad d8 02 03 00 00 00 00 00 00   fingerprint64("string") | ARG kind 2 (scalar), scalar kind 3 (i32)
0040  5f a2 97 ac fc ee 23 5c 01 00 00 00 00 00 00 00   fingerprint64("i32") | ARG literal
0050  d4 94 d2 af bd 15 4e 04                           id("worlds")
```

### call `("fill"_, 42_, Order, k<Side::sell>, -1.5_)` — 128 bytes (format 1.5)

```
0000  80 00 00 00 04 01 00 00 0e bb 66 27 0a cf f1 0e   total 128, tag 4 | call hash 0x0ef1cf0a2766bb0e
0010  05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00   n_args 5 | flags 0 | rsv | ARG 0: kind 1 (literal), rsv[7]
0020  a4 66 ff f0 19 0c c9 e1 04 03 00 00 00 00 00 00   id("fill") | ARG 1: arg_kind 4 (constant), kind 3 (i32), rsv[6]
0030  5f a2 97 ac fc ee 23 5c 2a 00 00 00 00 00 00 00   fingerprint64("i32") | u64 value 42
0040  00 00 00 00 00 00 00 00 d2 1d 44 c6 d8 8c 7f 36   ARG 2: arg_kind 0 (type), rsv | schema hash of corpus::Order
0050  04 00 00 00 00 00 00 00 e1 88 51 ca 7c 26 4e 07   ARG 3: arg_kind 4 (constant), kind 0 (an enum) | schema hash of corpus::Side
0060  02 00 00 00 00 00 00 00 04 0a 00 00 00 00 00 00   u64 value 2 (Side::sell) | ARG 4: arg_kind 4 (constant), kind 10 (f64)
0070  d1 9e 48 0c cc 07 9f c8 00 00 00 00 00 00 f8 bf   fingerprint64("f64") | u64 value 0xbff8000000000000 (the bits of -1.5)
```

A constant's ARG has `arg_kind` 4 (`fmt::arg_const`) and is 24 bytes: the usual 16 — `u8 arg_kind`, `u8 kind`,
`u8 rsv[6]`, `u64 hash` — followed by `u64 value`. `kind` is the scalar kind of the value, or 0 for an enum.
`hash` is what a run-time argument of that type would have: `fingerprint64` of the scalar's name, or the enum's
schema hash. `value` holds the bytes of the object, zero-extended to 8: 42 as an `int` is `2a 00 00 00` and four
zero bytes, `Side::sell` is one byte `02`, a `double` fills all eight. The ENTRY is
8 + 16 + 2 × 16 + 3 × 24 = 128 bytes; without the three values it would be 104. A reader of format 1.4 expects
16 bytes per ARG and would misparse this entry, which is why the minor moved to 1.5.

The call hash is `fingerprint64` of
`call(5){lit:0xe1c90c19f0ff66a4,const:i32=42,0x367f8cd8c6441dd2,const:0x074e267cca5188e1=2,const:f64=0xbff8000000000000}`:
the type and the value of every constant are in the string (§7.8).

### call `println(3){lit, const i64, const bool}` — 88 bytes (format 1.5)

```
0000  58 00 00 00 04 01 00 00 95 db 09 cf 37 b8 57 95   total 88, tag 4 | call hash 0x9557b837cf09db95
0010  03 00 03 00 00 00 00 00 01 00 00 00 00 00 00 00   n_args 3 | flags 3 (print + newline) | rsv | ARG literal
0020  f6 e1 62 01 e7 e4 d0 f3 04 04 00 00 00 00 00 00   id("limit={} on={}") | ARG constant, kind 4 (i64)
0030  94 90 7c 48 09 47 be 7f 00 f2 05 2a 01 00 00 00   fingerprint64("i64") | u64 value 5000000000 (0x12a05f200)
0040  04 0b 00 00 00 00 00 00 54 e9 88 33 1c cc 20 0a   ARG constant, kind 11 (bool) | fingerprint64("bool")
0050  01 00 00 00 00 00 00 00                           u64 value 1 (true)
```

`5'000'000'000_` does not fit an `int`, so the constant is a `long long` (`i64`). 8 + 16 + 16 + 24 + 24 = 88.

## 7.5 Frames

`u32 frame_len | u16 flags | u16 n_records | u64 call_hash` (16 bytes), then the records, each starting
8-aligned. `flags`: bit0 full records, bit1 dictionary frame, and (1.7) bit2 CRC32C trailer, bit3 checkpoint
frame, bit4 writer-info frame (§7.9). Unknown flag bits are refused (`bad_version` from the typed readers; the
walkers report the frame and skip it). Dictionary, checkpoint and writer-info frames are **meta frames**: they have
no records, every reader consumes them, they count in the frame numbering (`frame_ref::index`, `srdump --frames`)
but not in `--verify`'s frame and byte counts, and dumps do not list them. The frames below are the data frames of
`orders.ser`, unchanged since 1.6.

### Compact frame: `serialize(f, "new order"_, order)` — 48 bytes

```
0000  30 00 00 00 00 00 02 00 b2 fd 66 de 5b e2 f8 bc   frame_len 48 | flags 0 | n_records 2 | call hash
0010  2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00   record 0 = the literal: NO BYTES (FR-L6). record 1 = Order: id 42, px 1050000
0020  64 00 00 00 01 41 41 50 4c 00 00 00 00 00 00 00   qty 100 | side 1 | sym "AAPL\0\0\0\0" | 3 padding bytes zeroed by the scrub
```

Two records, one of which costs nothing: the literal is named by the call hash and the call ENTRY (FR-L6).

### Compact frame with only a literal: `serialize(f, corpus::Side::buy)` — 24 bytes, and `("heartbeat"_)` — 16 bytes

```
0000  18 00 00 00 00 00 01 00 ea ca d4 80 2c 61 34 36   n_records 1 | call hash
0010  01 00 00 00 00 00 00 00                           the enum's single byte, padded to 8
```

### Full-records frame: `serialize_with({.full_records = true}, f, "evt"_, order)` — 88 bytes

```
0000  58 00 00 00 01 00 02 00 2e 3b e2 68 4d 49 21 20   flags 1 (full) | 2 records | call hash
0010  35 72 84 a0 6d 4c 8e 19 08 00 00 00 00 00 00 00   record header: u64 hash = literal id | u32 payload_len 8 | u16 layout 0 | u16 rsv
0020  35 72 84 a0 6d 4c 8e 19 d2 1d 44 c6 d8 8c 7f 36   payload = u64 id | record header: schema hash of Order
0030  20 00 00 00 00 00 00 00 2a 00 00 00 00 00 00 00   payload_len 32 | layout 0 | rsv | Order bytes …
0040  90 05 10 00 00 00 00 00 64 00 00 00 01 41 41 50
0050  4c 00 00 00 00 00 00 00
```

Full records are self-describing (`srdump` can decode them without the call table), which is why a literal
keeps its 8-byte payload here.

### Print frame: `print(f, "hello {} {} {}"_, "new", 100, "worlds"_)` — 32 bytes (1.1)

```
0000  20 00 00 00 00 00 04 00 5e 56 00 1b 91 49 7f e9   n_records 4 | call hash of print(4){…}
0010  03 00 00 00 6e 65 77 00 64 00 00 00 00 00 00 00   string: u32 len 3, "new", pad | i32 100, pad
```

Four records, 16 bytes of payload: both literals (the format string and `"worlds"`) live in the dictionary.
`srdump` renders `hello new 100 worlds`; the `println` variant is the same bytes under a call whose flags are 3.

### Frames with constants (format 1.5)

`serialize(f, "fill"_, 42_, order, k<Side::sell>, -1.5_)` — a compact frame of 48 bytes:

```
0000  30 00 00 00 00 00 05 00 0e bb 66 27 0a cf f1 0e   frame_len 48 | flags 0 | n_records 5 | call hash
0010  2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00   records 0 and 1 (the literal, 42_): NO BYTES. record 2 = Order …
0020  64 00 00 00 01 41 41 50 4c 00 00 00 00 00 00 00   … records 3 and 4 (k<Side::sell>, -1.5_): NO BYTES
```

Five records, 32 bytes of payload: these are the bytes of the `("new order"_, order)` frame above but for
`n_records` and the call hash. The literal and the three constants are named by the call hash; a reader finds
their values in the call ENTRY of §7.4.

The same call with `serialize_with({.full_records = true}, …)` — 160 bytes:

```
0000  a0 00 00 00 01 00 05 00 0e bb 66 27 0a cf f1 0e   frame_len 160 | flags 1 (full) | 5 records | the same call hash
0010  a4 66 ff f0 19 0c c9 e1 08 00 00 00 00 00 00 00   record 0: hash = id("fill") | payload_len 8 | layout 0 | rsv
0020  a4 66 ff f0 19 0c c9 e1 5f a2 97 ac fc ee 23 5c   payload = u64 id | record 1: hash = fingerprint64("i32")
0030  04 00 00 00 00 00 00 00 2a 00 00 00 00 00 00 00   payload_len 4 | i32 42, padded to 8
0040  d2 1d 44 c6 d8 8c 7f 36 20 00 00 00 00 00 00 00   record 2: schema hash of Order | payload_len 32
0050  2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00   Order bytes …
0060  64 00 00 00 01 41 41 50 4c 00 00 00 00 00 00 00
0070  e1 88 51 ca 7c 26 4e 07 01 00 00 00 00 00 00 00   record 3: schema hash of Side | payload_len 1
0080  02 00 00 00 00 00 00 00 d1 9e 48 0c cc 07 9f c8   Side::sell, padded to 8 | record 4: hash = fingerprint64("f64")
0090  08 00 00 00 00 00 00 00 00 00 00 00 00 00 f8 bf   payload_len 8 | f64 -1.5
```

In a full-records frame a constant is the scalar or enum record a run-time value would be: records 1, 3 and 4
have the header and payload that `serialize_with({.full_records = true}, f, "fill"_, 42, order, Side::sell, -1.5)`
would write. Only the call hash in the frame header tells the two frames apart. 16 + 24 + 24 + 48 + 24 + 24 = 160.

`println(f, "limit={} on={}"_, 5'000'000'000_, k<true>)` — 16 bytes:

```
0000  10 00 00 00 00 00 03 00 95 db 09 cf 37 b8 57 95   frame_len 16 | flags 0 | n_records 3 | call hash of println(3){…}
```

A frame of a literal and constants only is its header. `srdump` renders `limit=5000000000 on=true` from the
dictionary alone.

### String / scalar frame: `serialize(f, "abc", 42, 1.5, "evt"_)` — 40 bytes (1.1)

```
0000  28 00 00 00 00 00 04 00 57 d9 d0 c0 34 51 43 99
0010  03 00 00 00 61 62 63 00 2a 00 00 00 00 00 00 00   string len 3 "abc" pad | i32 42 pad
0020  00 00 00 00 00 00 f8 3f                           f64 1.5 | literal "evt": no bytes
```

### Dynamic-type frame: `serialize(f, corpus::DynMsg{7, "ab", {1, -2, 3}})` — 56 bytes (1.2)

```
0000  38 00 00 00 00 00 01 00 8e db c9 2a 4e b1 6a 75   frame header
0010  1c 00 00 00 00 00 00 00 07 00 00 00 14 00 00 00   u32 len 28 | u32 0 | image: id 7 | name.off 20
0020  02 00 00 00 16 00 00 00 03 00 00 00 61 62 01 00   name.n 2 | xs.off 22 | xs.n 3 | "ab" | xs[0]=1
0030  fe ff 03 00 00 00 00 00                           xs[1]=-2 | xs[2]=3 | pad
```

The record is `u32 len, u32 0, image` (the 8-byte prefix keeps the image 8-aligned inside the record). The
image is a 20-byte **fixed part** (`id` at canonical offset 0, two 8-byte reference slots at 4 and 12) plus a
**tail** the slots point into (offsets relative to the image start). Chapter 8 walks the rule; the nested example
below shows references to references.

### Nested dynamic frame: `serialize(f, DynNested, DynArr)` — 248 bytes (record 0 shown)

```
0010  73 00 00 00 00 00 00 00 18 00 00 00 02 00 00 00   len 115 | 0 | msgs.off 24, n 2
0020  48 00 00 00 01 00 00 00 68 00 00 00 03 00 00 00   maybe.off 72, n 1 (engaged) | v.off 104, n 3 (alternative 2 = string)
0030  07 00 00 00 40 00 00 00 02 00 00 00 42 00 00 00   msgs[0] fixed part: id 7 | name {64,2} | xs {66,3}
0040  03 00 00 00 08 00 00 00 00 00 00 00 00 00 00 00   msgs[1] fixed part: id 8 | name {0,0} (empty) | xs {0,0}
0050  00 00 00 00 00 00 00 00 61 62 01 00 fe ff 03 00   … | tail of msgs[0]: "ab" @64, xs @66
0060  2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00   maybe: the Order image @72 (flat bytes, scrubbed)
0070  64 00 00 00 01 41 41 50 4c 00 00 00 00 00 00 00
0080  70 00 00 00 03 00 00 00 76 61 72 00 00 00 00 00   v: a string slot @104 {112,3} → "var" @112
```

Element blocks are contiguous at the element stride (a `DynMsg` element is 20 bytes; block aligned to 4), and
each element's own tails follow the block in depth-first order — exactly what `srdump` walks with only the
dictionary.

### 7.5b Map, set and container-argument frames (format 1.6)

`serialize(f, "map"_, corpus::sample_dyn_map())` — frame 5 of `dynamic.ser`, 232 bytes. The value:
`{9, by_name = {{"b",2},{"a",1},{"c",3}}, ids = {30,10,20}, names = {{5,"five"},{4,"four"},{-1,"minus one"}}
(an std::unordered_map), msgs = {{2, DynMsg{2,"two",{2}}}, {1, DynMsg{1,"one",{1,1}}}}}`. Image offsets below
are relative to the image, which starts at frame offset 0x18:

```
0000  e8 00 00 00 00 00 02 00 71 54 36 da 45 54 9f cd   frame_len 232 | flags 0 | n_records 2 | call hash; record 0 = "map": NO BYTES
0010  ca 00 00 00 00 00 00 00 09 00 00 00 24 00 00 00   record 1: u32 len 202 | u32 0 | image: id 9 @0 | by_name.off 36
0020  03 00 00 00 4c 00 00 00 03 00 00 00 54 00 00 00   by_name.n 3 | ids {76, 3} | names.off 84
0030  03 00 00 00 8c 00 00 00 02 00 00 00 48 00 00 00   names.n 3 | msgs {140, 2} — end of the 36-byte fixed part | by_name block @36: entry 0 key slot {72,
0040  01 00 00 00 01 00 00 00 49 00 00 00 01 00 00 00   1} value 1 | entry 1 @48: key {73, 1}, value
0050  02 00 00 00 4a 00 00 00 01 00 00 00 03 00 00 00   2 | entry 2 @60: key {74, 1}, value 3
0060  61 62 63 00 0a 00 14 00 1e 00 00 00 ff ff ff ff   "a" @72 "b" @73 "c" @74 | pad | ids block @76: 10, 20, 30 (u16) | pad | names block @84: entry 0 key -1
0070  78 00 00 00 09 00 00 00 04 00 00 00 81 00 00 00   value slot {120, 9} | entry 1 @96: key 4, value {129,
0080  04 00 00 00 05 00 00 00 85 00 00 00 04 00 00 00   4} | entry 2 @108: key 5, value {133, 4}
0090  6d 69 6e 75 73 20 6f 6e 65 66 6f 75 72 66 69 76   "minus one" @120 "four" @129 "fiv
00a0  65 00 00 00 01 00 00 00 01 00 00 00 bc 00 00 00   e" @133 | pad to 4 | msgs block @140: entry 0 key 1, pad | DynMsg{id 1 | name {188,
00b0  03 00 00 00 c0 00 00 00 02 00 00 00 02 00 00 00   3} | xs {192, 2}} | entry 1 @164: key 2, pad | DynMsg{id 2
00c0  02 00 00 00 c4 00 00 00 03 00 00 00 c8 00 00 00   | name {196, 3} | xs {200, 1}}
00d0  01 00 00 00 6f 6e 65 00 01 00 01 00 74 77 6f 00   … | entry 0's tails: "one" @188, pad, xs 1, 1 @192 | entry 1's: "two" @196, pad
00e0  02 00 00 00 00 00 00 00                           xs 2 @200 — image ends at 202 | record pad to 8
```

Every rule of §7.4b and chapter 8 §8.1b is visible here:

- **The slot** of each container is `{u32 off, u32 n}` in the fixed part (`by_name` at 4, `ids` at 12, `names` at
  20, `msgs` at 28; the fixed part is 36 bytes). `n` is the entry count, never a byte count.
- **The block** is `n × stride` bytes at `off`, aligned to the entry: `by_name` 3 × 12 at 36, `ids` 3 × 2 at 76
  (the block of a `set<u16>` needs alignment 2, hence the pad byte at 75), `names` 3 × 12 at 84 (aligned to 4
  after the set's 6 bytes end at 82), `msgs` 2 × 24 at 140 (aligned to 4 after `"five"` ends at 137).
- **Inside an entry** the key is at 0 and the value at its canonical offset: a string key is an 8-byte slot and
  the `i32` value follows at 8; an `i32` key is followed by the string slot at 4; a `u8` key is padded to 4, where
  the `DynMsg` fixed part begins (its own slots at +4 and +12 of the entry's value, i.e. 148/156 for entry 0).
- **Tails come after the whole block, entry by entry**: `"a"`, `"b"`, `"c"` at 72–74 after the 36-byte block;
  `"minus one"`, `"four"`, `"five"` after the `names` block; `"one"`, `xs` of entry 0, then `"two"`, `xs` of
  entry 1 after the `msgs` block — the encoder allocates the block first, then encodes each entry, which allocates
  its own tails (`reflect.hpp`, `encode_value`'s `map_`/`set_` branch).
- **Canonical order**: `by_name` was built as `{"b","a","c"}` and is on the wire `a, b, c`; `ids` as `{30,10,20}`
  and is `10, 20, 30`; `msgs` as `{2, 1}` and is `1, 2`; `names` is an `std::unordered_map`, sorted by the writer
  into `-1, 4, 5` — by `operator<` on `int`, not by the bytes (`ff ff ff ff` would sort last). The same values in
  any of the six containers give these bytes.
- **Gaps are zero** (the pad at 75, 82–83, 137–139, the three bytes after each `u8` key, 191, 199, and the record
  padding at 202–207): the encoder zeroes the whole measured image first.

Image 202 → record 8 + 202 = 210 → 216; frame 16 + 216 = 232.

A **container as an argument** is the dynamic-type record of its transparent type. `serialize(f, "ids"_, ids)`
with `std::vector<std::int32_t>{3, 1, 2}` — frame 6, 48 bytes:

```
0000  30 00 00 00 00 00 02 00 4d 89 96 a6 f1 ae d3 9e   frame_len 48 | flags 0 | n_records 2 | call hash; record 0 = "ids": NO BYTES
0010  14 00 00 00 00 00 00 00 08 00 00 00 03 00 00 00   record 1: u32 len 20 | u32 0 | image: the vector's slot {8, 3} @0
0020  03 00 00 00 01 00 00 00 02 00 00 00 00 00 00 00   block @8: 3, 1, 2 (i32) — image ends at 20 | pad
```

The image is exactly what the vector produces as the only member of a struct: an 8-byte fixed part holding the
slot, then the tail. The call hash is `fingerprint64("call(2){lit:0xfa579a03eefd9495,0x02c87ef3a015eb45}")` —
the literal and the transparent type's schema hash, as for any type argument (§7.8). The same call with an empty
vector — frame 7, 32 bytes — is `u32 8 | u32 0 | slot {0, 0}`:

```
0000  20 00 00 00 00 00 02 00 4d 89 96 a6 f1 ae d3 9e
0010  08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   len 8 | 0 | the slot {0, 0}: an empty container has no tail
```

With full records (frame 8, 88 bytes) the record header carries the transparent type's schema hash:

```
0000  58 00 00 00 01 00 02 00 4d 89 96 a6 f1 ae d3 9e   flags 1 (full) | 2 records | the same call hash
0010  95 94 fd ee 03 9a 57 fa 08 00 00 00 00 00 00 00   record 0: hash = id("ids") | payload_len 8 | layout 0 | rsv
0020  95 94 fd ee 03 9a 57 fa 45 eb 15 a0 f3 7e c8 02   payload = u64 id | record 1: hash = schema hash of std::vector<i32>
0030  1c 00 00 00 00 00 00 00 14 00 00 00 00 00 00 00   payload_len 28 | layout 0 | rsv | payload: u32 len 20 | u32 0
0040  08 00 00 00 03 00 00 00 03 00 00 00 01 00 00 00   slot {8, 3} | 3, 1
0050  02 00 00 00 00 00 00 00                           2 | pad
```

Frame 9, `println(f, "ids={} map={}"_, ids, map)` (264 bytes), is the two records above one after
the other under a `println(3){…}` call; `srdump` renders it as `ids=[3, 1, 2] map=corpus::DynMap{id: 9, by_name:
{"a": 1, "b": 2, "c": 3}, ids: {10, 20, 30}, names: {-1: "minus one", 4: "four", 5: "five"}, msgs: {1:
corpus::DynMsg{id: 1, name: "one", xs: [1, 1]}, 2: corpus::DynMsg{id: 2, name: "two", xs: [2]}}}`.

### Dictionary frame (streams and 1.7 files)

`u32 frame_len | u16 flags = 2 | u16 0 | u64 0x4553535244494354 ("ESSRDICT") | one RRI | pad to 8 | u64
next_dictionary` — the last 8 bytes when the RRI's minor is ≥ 7. On a stream, `write_dictionary_frame()`
/ `dictionary_tracker::update()` emit the full image first and, when the registry version changed, a **delta**
(the RRI header's `rri_flag_delta` set, only the entries added since the last dictionary written); the link stays
0, nobody patches a pipe. In a 1.7 file the capture file writes the same deltas inline and links them (§7.9).
Readers merge every dictionary frame they meet, full or delta, into what they already have.

## 7.6 Record encodings per argument category

| category | compact record | full record payload |
|---|---|---|
| flat type | native bytes (padding zeroed, bools 0/1), padded to 8 | the same |
| literal | **nothing** | `u64 id` |
| constant (1.5) | **nothing**: the value is in the call ENTRY (`u64` after the ARG) | the scalar or enum record of its value |
| scalar (1.1) | native bytes, padded to 8 | the same |
| runtime string (1.1) | `u32 len, bytes`, padded to 8 | the same (`payload_len` = 4 + len) |
| dynamic type (1.2) | `u32 len, u32 0, image`, padded to 8 | the same (`payload_len` = 8 + len) |
| container argument (1.6) | the dynamic-type record of its transparent type: `u32 len, u32 0, slot + tail` | the same, under the transparent type's schema hash |

The full-record header slot carries: schema hash / literal id / `fingerprint64(kind name)` /
`fingerprint64("string")`; for a constant, the hash of the value it stands for (`fingerprint64(kind name)` or the
enum's schema hash).

## 7.7 Reading older captures

**1.6 is read** (readers accept two minors: `fmt::format_minor_oldest` = 6, `format_minor` = 7). A 1.6 file has no CHAIN, no meta frames other than stream dictionary frames, and no CRC
trailers — none of the 1.7 flags is set — and may be back-patched (FILE flag bit 2, `dict_offset` ≠ 48): the
readers then take the dictionary at `dict_offset`, start the frames after the original area at 48 and stop them at
`dict_offset`, exactly as a 1.6 reader does. `tests/fixtures/corpus/capture/orders-1.6.ser` and `dynamic-1.6.ser` and
`corpus/rri/orders-1.6.rri` are kept for this ("P 1.7 the readers still read 1.6 files, and refuse 1.5 and 1.8",
`tests/unit/test_format17.cpp`; the Wireshark `backpatched` check). DECISIONS G-2 makes this the rule from now on:
until 3.0 the minor is the compatibility unit, and after 3.0 the wire format is additive only.

Older minors are not supported. Formats 1.0–1.2 identified every type, call and literal by FNV-1a-64 (`hash_algo`
1); the library computes XXH3-64 only, so no id in an older file can be matched against a compiled type. The minors
after that were not kept readable either: a 1.3 file may carry the canonical names from before format 1.4, whose
schema hashes no longer match; 1.5 exists because a 1.4 reader cannot walk a call ENTRY that has a constant; 1.6 exists
because a 1.5 reader refuses a FIELD of kind `map` or `set` and the new flag bits, and by the
rule that a reader accepts exactly its own minor a 1.6 reader refuses 1.5 files too — although a 1.5 capture of a
program that uses no container is byte-identical to its 1.6 capture apart from the headers' minor and the enum
entries' flag byte. A reader refuses a file up front: a minor other than 6 or 7 is `bad_version`, a header of
an accepted minor naming another hash algorithm is `unsupported`
(`FR-H5 captures of an older minor or hash algorithm are refused` in `tests/unit/test_print.cpp`). The frozen
1.0/1.1 corpora were removed with the FNV code; the library was not yet in wide use and no data files had to be
kept readable. The header says what it is, and a reader either knows that minor or refuses it.

## 7.8 Canonical strings (what the hashes hash)

```
schema:  struct<corpus::Order>[32,8]{id:u64@0[8,8];px:i64@8[8,8];qty:u32@16[4,4];side:enum<corpus::Side,u8>@20[1,1];sym:array<char,8>@21[8,1];}
layout:  struct[32,8]{u64@0[8,8];i64@8[8,8];u32@16[4,4];enum<u8>@20[1,1];array<char,8>@21[8,1];}
dynamic: dyn<corpus::DynMsg>[20,4]{id:u32@0[4,4];name:string@4[8,4];xs:vector<i16>@12[8,4];}
map/set: dyn<corpus::DynMap>[36,4]{id:u32@0[4,4];by_name:map<string,i32>@4[8,4];ids:set<u16>@12[8,4];names:map<i32,string>@20[8,4];msgs:map<u8,dyn<0xb487de0c417a37e8>>@28[8,4];}
transp.: dyn<std::vector<i32>>[8,4]{:vector<i32>@0[8,4];}        layout: dyn[8,4]{vector<i32>@0[8,4];}
call:    call(2){lit:0x198e4c6da0847235,0x367f8cd8c6441dd2}   print(4){lit:…,string,i32,lit:…}
         call(2){lit:0xfa579a03eefd9495,0x02c87ef3a015eb45}   println(3){lit:0xb6ed62f53c446e40,0x02c87ef3a015eb45,0x1aa730519093de87}
const:   call(5){lit:0xe1c90c19f0ff66a4,const:i32=42,0x367f8cd8c6441dd2,const:0x074e267cca5188e1=2,const:f64=0xbff8000000000000}
         println(3){lit:0xf3d0e4e70162e1f6,const:i64=5000000000,const:bool=true}
```

A constant (format 1.5) is the item `const:<scalar name>=<value>` — integers in decimal, `bool` as `true`/`false`,
floating values by their bit pattern — or `const:0x<enum schema hash>=<underlying value>` (chapter 6 §6.2).

A map or set member (1.6) is the kindspec `map<key,value>` / `set<key>` whatever the C++ container — the
`std::unordered_map` member `names` spells `map<i32,string>` like a `std::map` would, so the container is not in
the hash; the two `dyn<…>` strings above hash to `0x1aa730519093de87` and `0x02c87ef3a015eb45`, the entries of
§7.4 and §7.4b, and the last two call strings to the call hashes of frames 6–9 (chapter 6 §6.2). A container
argument's call item is its transparent type's schema hash, which does carry the container's name
(`std::vector<i32>`, `std::unordered_map<i32,std::string>`).

Schema strings carry names and `since`/`deprecated` (`[4,4;s2;d0]`); layout strings do not. Template arguments
render as values (`3`, `true`, `120`, `enum(7)`), never as compiler spellings. `[golden-print]` in
`serreg_tests` prints every frozen value; `tests/golden/hashes.cpp` locks them.

## 7.9 Format 1.7: crash safety, integrity and seeking

Format 1.7 (DECISIONS P-*) is about captures that are large, long-lived or written by processes
that may die: a file must be readable up to its last complete frame at every instant, damage must be detected and
contained, and a reader must be able to start in the middle. Every feature is optional per file
(`capture_options`, chapter 3 §3.4) and costs zero bytes per frame when off.

### The forward-linked dictionary chain

A 1.7 writer never rewrites anything. When the registry moves while a file is open (a `dlopen`), the capture file
compares one atomic version before each frame and, when it changed, writes a **dictionary frame holding the
delta** — only the entries whose `registry_entry::added` version is newer than the last dictionary written, with
`rri_flag_delta` in the RRI header — *before* the first frame that may need it. Then it links the delta from the
previous dictionary: the CHAIN's `next_dictionary` for the first delta, the last 8 bytes of the previous
dictionary frame afterwards. Schematically:

```
0       FILE header (dict_offset 48)
48      opening dictionary (full RRI)                  ─┐
1552    CHAIN: next_dictionary ────────────────────────┼─┐   first_checkpoint ──► first checkpoint (if any)
        writer-info frame                               │ │
        data frames …                                   │ │
D1      dictionary frame: RRI delta | pad | next ──┐  ◄─┘─┘
        data frames that use the new types …       │
D2      dictionary frame: RRI delta | pad | next=0 ◄┘
        data frames …
```

Each link is written once, with one aligned 8-byte `pwrite`, after the frame it points at is complete (and, with
the default `sync_policy::dictionary`, after an `fdatasync`). A crash therefore leaves the file in one of two
states: the newest link is still zero — a reader that follows the chain stops at the previous dictionary, a
sequential reader still meets the delta inline — or it is valid. A file truncated anywhere is readable up to the
last complete frame. Sequential readers (`for_each_frame`, `verify`, `dump`, the Python reader, Wireshark) merge
deltas as they meet them; `tools::info` and `srdump --info` follow the chain with seeks, so collecting the whole
dictionary of a multi-terabyte file costs one read per delta, not a scan. With `capture_policy{.dictionary_updates =
false}` a program that never loads plugins drops the per-frame compare.

### The CRC32C trailer

With `capture_options{.crc = true}` (every data frame of the file; FILE flag bit 3) or `serialize_options{.crc =
true}` (one frame, any sink), a frame carries frame flag bit 2 and 8 more bytes at its end. Frame 1 of
`features.ser`, the `"new order"` frame of §7.5 with its trailer:

```
0000  38 00 00 00 04 00 02 00 b2 fd 66 de 5b e2 f8 bc   frame_len 56 | flags 4 (CRC) | n_records 2 | call hash
0010  2a 00 00 00 00 00 00 00 90 05 10 00 00 00 00 00   the Order record, as in the 48-byte frame
0020  64 00 00 00 01 41 41 50 4c 00 00 00 00 00 00 00
0030  57 1e 7c c6 00 00 00 00                           u32 crc32c(bytes 0..47) = 0xc67c1e57 | u32 0
```

The CRC is CRC32C (Castagnoli, the iSCSI polynomial; `crc32c()` in `hash.hpp`, SSE4.2 when available) over every
byte of the frame before the trailer, header included. Records end before the trailer (`payload_end()`). A reader
checks it before believing anything in the frame: the typed readers return `error::crc_mismatch`; the walkers
report `frame N @off: crc mismatch (frame skipped)` and continue with the next frame — a damaged frame is never
decoded and never delivered. Meta frames (dictionary, checkpoint, writer info) carry no trailer: the `crc` flag
covers data frames.

### Checkpoint frames

With `checkpoint_bytes` and/or `checkpoint_every` the writer emits a checkpoint frame (frame flag bit 3, call hash
`fmt::checkpoint_sentinel` = `"ESSRCKPT"`, no records; FILE flag bit 4) after that many bytes of data frames or
that much time (the clock is read every 64 frames), and a final one at `close()`. The first checkpoint of
`features.ser` (`checkpoint_bytes = 256`, `checkpoint_dictionary = true`):

```
0000  30 06 00 00 08 00 00 00 54 50 4b 43 52 53 53 45   frame_len 1584 | flags 8 (checkpoint) | 0 records | "ESSRCKPT"
0010  01 00 01 00 e0 05 00 00 00 00 00 00 00 00 00 00   version 1 | flags 1 (a full RRI follows) | rri_len 1504 | prev 0
0020  70 0f 00 00 00 00 00 00 05 00 00 00 00 00 00 00   next 3952 (patched) | data frames before: 5
0030  18 01 00 00 00 00 00 00 00 00 2a 36 fe 9c 97 17   their bytes: 280 | wall clock ns (the fixtures' fixed clock, 1.7e18)
0040  30 00 00 00 00 00 00 00 14 00 00 00 00 00 00 00   latest dictionary @48 | registry version 20
0050  45 53 52 47 01 00 07 00 00 00 00 00 02 00 00 00   the full RRI (1504 bytes) …
```

The fixed part is 64 bytes (`fmt::checkpoint_fixed_size`): `u16 version`, `u16 flags` (bit 0: a full RRI
follows), `u32 rri_len`, `u64 prev_checkpoint`, `u64 next_checkpoint`, `u64 data_frames_before`, `u64
bytes_before`, `u64 wall_clock_ns`, `u64 latest_dictionary_offset`, `u64 registry_version`. Checkpoints form their
own forward chain: the CHAIN's `first_checkpoint` points at the first, each `next` (frame offset 32) is patched when
the following one is written, and `prev` points back. A reader can therefore seek to the checkpoint whose wall
clock or frame count it wants and start decoding there: the checkpoint names the latest dictionary at that point
(follow the dictionary chain up to that offset) or, with `checkpoint_dictionary`, carries the full dictionary
itself. With the default `checkpoint_dictionary = false` a checkpoint is 80 bytes.

Checkpoints are also the **resynchronisation points** of `srdump --resync` (`block_options::resync`): when a frame
header cannot be parsed (a `frame_len` that is not a multiple of 8, too small, or beyond the file), the windowed
reader scans forward 8 bytes at a time for a frame header with a valid length, the checkpoint flag and the
`"ESSRCKPT"` sentinel — a signature corrupt data does not produce by accident — reports the skipped range as one
problem, and continues there. Without `--resync` the walk stops at the bad frame.

### What a reader does with a 1.7 file

1. Read the 48-byte header; accept minor 6 or 7.
2. Read the opening dictionary at 48; for minor 7 skip the CHAIN (`align8(48 + dict_len) + 16`).
3. Walk the frames: a frame with an unknown flag is skipped with a problem; a frame whose CRC fails is skipped with
   a problem; a dictionary frame is merged (full or delta); checkpoint and writer-info frames are consumed; a data
   frame is decoded with the dictionary as known at that point.
4. Or, for an overview without walking: follow the CHAIN's two forward chains with seeks (`tools::info`).

`tests/unit/test_format17.cpp` checks each of these, `golden_srdump_verify_features` and
`golden_srdump_info_features` run `srdump` on `features.ser`, and `python_reader_parity` compares the pure-Python
reader (`python/serreg`, which restates this chapter in code) with `srdump` on every golden capture.
