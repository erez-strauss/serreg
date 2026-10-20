# `include/es/serreg/format.hpp`

The wire format, byte for byte: the ENTRY, RRI, FILE and FRAME layouts as a normative comment, their constants
(`format_major`/`format_minor`, magics, sizes, flags, tags), the little-endian `cursor`/`writer` used by every
decoder and encoder, and the constexpr byte builder `detail::cbuf` plus `detail::to_blob_array` that turn a
`consteval` byte vector into a `.rodata` array. This is the **wire-format layer**; nothing here depends on
reflection. The library writes format **1.7** and reads **1.6 and 1.7** — two minors (`format_minor_oldest` = 6,
`format_minor` = 7). The minors so far: 1.1 scalar and string
records and call flags; 1.2 dynamic types; 1.3 XXH3-64 ids, `hash_algo` 2 and the writer's ABI tag
in the FILE and RRI headers; 1.4 the canonical-name rules, which moved some schema hashes and no bytes;
1.5 compile-time constants as call arguments (`arg_const`); 1.6 the FIELD kinds `map_` (53) and
`set_` (54), the struct ENTRY flag `transparent` (a container passed as an argument) and the enum ENTRY's reserved
byte as `flags` with bit 0 = `fixed`; 1.7 integrity and crash safety for captures of any size: the FILE
flags `crc_all`, `checkpoints`, `info` (bits 3–5), the frame flags CRC32C trailer, checkpoint and writer info
(bits 2–4), the 16-byte CHAIN after a FILE's opening dictionary, dictionary deltas (`rri_flag_delta`) linked forward
by an 8-byte pointer at the end of each dictionary frame, the checkpoint and writer-info frames with their
sentinels, and the call flag `log` (bit 2). Every 1.7 feature is optional per sink; with the defaults a 1.7 file
differs from a 1.6 one only in the minor, the CHAIN and the writer-info frame, and nothing is rewritten at close.
A file of any other minor is refused with `bad_version`: formats 1.0–1.2 carry FNV-1a-64 ids, 1.3 may carry the old
names, a 1.4 reader would misparse a call ENTRY that has a constant, and 1.5 predates the container kinds.

## Depends on / used by

Includes: `<array>`, `<bit>`, `<cstddef>`, `<cstdint>`, `<cstring>`, `<span>`, `<vector>`.

Included by: `arg.hpp`, `blob.hpp`, `capture.hpp`, `deserialize.hpp`, `dictionary.hpp`, `literal.hpp`,
`serialize.hpp`, `src/internal.hpp`, `src/registry.cpp`, `tests/unit/runtime_selftest.cpp`. The canary
(`tests/canary/rri_canary.cpp`) intentionally does *not* include it — it re-types the layout comment as literal
offsets so that a coordinated encoder+decoder drift is still caught.

## What it defines

Namespace `es::serreg::fmt`:

| name | value / kind | meaning |
|---|---|---|
| `file_magic`, `rri_magic` | `array<char,4>` `"ESSR"`, `"ESRG"` | FILE and RRI magics |
| `format_major`, `format_minor` | `1`, `7` | written by this library; readers accept `format_minor_oldest` (6) to `format_minor` (7) (`format.hpp:102`) |
| `hash_algo_xxh3_64` | `2` | the only hash algorithm id (XXH3-64; 1 was FNV-1a-64, before format 1.3) |
| `format_minor_oldest` | `6` | the oldest minor readers accept |
| `dict_frame_sentinel` | `0x4553535244494354` (`"ESSRDICT"`) | `call_hash` of a dictionary frame |
| `checkpoint_sentinel`, `info_sentinel` | `0x45535352434b5054` (`"ESSRCKPT"`), `0x45535352494e464f` (`"ESSRINFO"`) (1.7) | the frame-header hash of a checkpoint and of a writer-info frame (`format.hpp:105-107`) |
| `file_header_size`, `rri_header_size`, `frame_header_size`, `entry_header_size`, `full_record_header_size` | 48, 40, 16, 8, 16 | fixed header sizes |
| `max_nesting` | `64` | deepest nesting of struct levels a reader follows; the typed decoder (`detail::max_image_depth`) and the dictionary walker in `src/decoder.cpp` share it (D-10) |
| `literal_len_offset`, `literal_text_offset` | 16, 24 | where a literal ENTRY's `u32 len` and text sit (used by `lit<S>::text()` and `registry::push`) |
| `string_len_prefix` | 4 | `u32 len` before a string record's bytes |
| `image_prefix` | 8 | `u32 len, u32 0` before a dynamic type's image (1.2) |
| `entry_tag` | enum `tag_struct=1, tag_enum=2, tag_literal=3, tag_call=4` | ENTRY tag byte |
| `arg_kind` | enum `arg_type=0, arg_literal=1, arg_scalar=2, arg_string=3, arg_const=4` | ARG category byte (2/3 since 1.1, 4 since 1.5) |
| `const_value_size` | 8 (1.5) | the `u64` value that follows a constant's ARG in the call ENTRY |
| `call_flag_print`, `call_flag_newline` | bit 0, bit 1 | call ENTRY flags (1.1) |
| `call_flag_log` | bit 2 (1.7) | a logger's call: the log envelope precedes the format literal (`log.hpp`); set together with print and newline |
| `file_flag_le`, `file_flag_dict` | bits 0, 1 | FILE header flags |
| `file_flag_backpatched` | bit 2 | 1.6 files only: the dictionary was rewritten at the end; never written since 1.7 |
| `file_flag_crc_all`, `file_flag_checkpoints`, `file_flag_info` | bits 3, 4, 5 (1.7) | every data frame carries a CRC trailer; the writer emits checkpoints; a writer-info frame follows the CHAIN (`format.hpp:147-152`) |
| `frame_flag_full`, `frame_flag_dict` | bits 0, 1 | FRAME flags |
| `frame_flag_crc`, `frame_flag_checkpoint`, `frame_flag_info` | bits 2, 3, 4 (1.7) | 8-byte CRC32C trailer; checkpoint frame; writer-info frame (`format.hpp:161-165`). Any other bit → `error::bad_version` (the typed reader), a skipped frame with a problem (the walkers) |
| `crc_trailer_size` | 8 (1.7) | `u32 crc32c, u32 0` at the end of a frame with `frame_flag_crc` |
| `chain_size`, `dict_frame_link_size` | 16, 8 (1.7) | the CHAIN after a FILE's opening dictionary (`u64 next_dictionary, u64 first_checkpoint`); the `u64 next_dictionary` at the end of a dictionary frame |
| `checkpoint_fixed_size`, `checkpoint_flag_full_dictionary` | 64, bit 0 (1.7) | a checkpoint payload before its optional RRI; its flag "a full RRI follows" |
| `info_key` | enum (1.7) | writer-info keys: `info_library_version = 1`, `info_format`, `info_program`, `info_hostname`, `info_pid`, `info_start_wall_ns`, `info_scrub_padding`, `info_full_records`, `info_canonical_unordered`, `info_crc`, `info_checkpoint_bytes`, `info_checkpoint_ns`, `info_dictionary`, `info_sync`, `info_clock`, `info_thread`, `info_abi = 17` (`format.hpp:166-185`) |
| `rri_flag_delta` | bit 0 | the RRI is a delta (1.7: written in dictionary frames after a registry change; reserved until 1.6) |
| `struct_flag_flat` | bit 0 | struct ENTRY flags byte: memcpy-able native image |
| `struct_flag_transparent` | bit 1 (1.6) | struct ENTRY flags byte: the synthetic type of a container argument — exactly one FIELD, unnamed, at 0, of a dynamic kind; renderers show the value, not a struct around it |
| `enum_flag_fixed` | bit 0 (1.6) | enum ENTRY flags byte (the byte after `underlying_kind`, reserved until 1.5): the enum has a fixed underlying type, so every value of that type is valid (decision D's range check applies to enums without it) |
| `align8(n)` | `constexpr` | round up to a multiple of 8 |
| `abi_tag` | 8-byte struct | 1.3 (FR-W4): `version` (1), `endian`, `ptr_size`, `long_size`, `wchar_size`, `char_signed`, `max_align`, `long_double_size` |
| `host_abi()`, `host_abi_tag` | `constexpr` | this build's tag, and the same packed into the header `u64` |
| `pack(abi_tag)`, `unpack_abi(u64)` | `constexpr` | `std::bit_cast` between the struct and the header `u64` (byte 0 = version) |
| `slot`, `slot_size` (8), `read_slot(image, at, slot&)` | struct, constant, `inline` function | a dynamic member's reference `{u32 off, u32 n}`, read with a bounds check |
| `slot_stride(size, align)` | `constexpr` | an element's stride in a block, in 64 bits |
| `image_budget` | struct | the slot rules both image decoders apply (review G2-6): `image`, `fixed` (the root's fixed size), `left` (bytes the remaining blocks may reference, starting at the image size), `take(slot, bytes)`, `take_n(slot, stride)` |
| `cursor` | struct | bounded LE reader: `remaining()`, `can(n)`, `read(T&)`, `skip(n)`, `bytes(n, span&)` |
| `writer` | struct | unbounded LE writer over caller-sized memory: `put(T)`, `put_bytes`, `zero(n)` |

Namespace `es::serreg::detail`:

| name | kind | meaning |
|---|---|---|
| `cbuf` | struct | constexpr byte builder: `put(T)` via `bit_cast`, `str16(sv)` (u16 length + bytes), `pad8()`, `begin_entry(tag)` / `end_entry()` (back-patches `total_len`) |
| `to_blob_array<P>()` | `consteval` function template | freezes `P::bytes()` (a `std::vector<std::byte>`) into a `std::array` — two passes: size, then contents |

A `static_assert(std::endian::native == std::endian::little)` (`format.hpp:202-203`) makes the library refuse to
compile on big-endian hosts (FR-W4).

## How it works

### Byte layouts

All integers little-endian, fixed width, read and written with `memcpy`. "u16 name_len, name bytes" is a length
prefix immediately followed by the bytes, no terminator. Reproduced from the header comment (`format.hpp:6-61`);
chapter 7 dumps every structure from the golden fixtures.

**ENTRY** — the unit stored in the ELF section `es_serreg_dict`, in an RRI and in capture dictionaries.

| offset | field | notes |
|---|---|---|
| 0 | `u32 total_len` | whole entry incl. header, multiple of 8, zero padded |
| 4 | `u8 tag` | 1 struct, 2 enum, 3 literal, 4 call |
| 5 | `u8 version` | 1 |
| 6 | `u16 reserved` | 0 |
| 8 | body | by tag, below |

| tag | body |
|---|---|
| 1 struct | `u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 flags(bit0 = is_flat, bit1 = transparent — 1.6), u8 rsv, u16 since, u16 deprecated, u16 name_len, u16 n_fields, name bytes, FIELD × n_fields` |
| 2 enum | `u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 underlying_kind, u8 flags(bit0 = fixed — 1.6; `rsv` until 1.5), u16 since, u16 deprecated, u16 name_len, u16 n_enumerators, name bytes, ENUMERATOR × n` where ENUMERATOR = `i64 value, u16 name_len, name bytes` |
| 3 literal | `u64 id, u32 len, u32 rsv, bytes` (so `id` @+8, `len` @+16, text @+24 — the two `literal_*_offset` constants) |
| 4 call | `u64 call_hash, u16 n_args, u16 flags(bit0 print, bit1 newline — 1.1, bit2 log — 1.7), u32 rsv, ARG × n` where ARG = `u8 arg_kind, u8 kind, u8 rsv[6], u64 hash` (kind: the scalar kind of a scalar or of a scalar constant, else 0; hash: schema hash / literal id / `fingerprint64(scalar name)` / `fingerprint64("string")`). 1.5: the ARG of a constant (`arg_kind` 4) is followed by `u64 value`, so it is 24 bytes |

**ARG of a constant** (1.5, `arg_const`) — 24 bytes where every other ARG is 16:

| offset | field | notes |
|---|---|---|
| 0 | `u8 arg_kind` | 4 |
| 1 | `u8 kind` | the scalar kind of the value (`i32` = 3, `f64` = 10, …), or 0 for an enum |
| 2 | `u8 rsv[6]` | 0 |
| 8 | `u64 hash` | `fingerprint64(scalar name)`, or the enum's schema hash |
| 16 | `u64 value` | the object's bytes, zero-extended to 8 (`detail::const_bits`, `arg.hpp`) |

A call ENTRY is therefore `8 + 16 + 16 × n_args + 8 × (number of constants)` bytes, padded to 8 — no longer a
function of `n_args` alone, which is why a 1.4 reader cannot walk it and the minor moved. The enumerator comment in
the header (`format.hpp:135-138`) says the same.

**FIELD** (recursive):

| field | notes |
|---|---|
| `u16 name_len` | |
| `u8 kind` | `descriptor.hpp` kind value |
| `u8 rsv` | |
| `u32 offset` | native (flat) or canonical image offset (dynamic) |
| `u32 size` | |
| `u16 align` | |
| `u16 since`, `u16 deprecated` | |
| `u16 n_elem` | 1 for array/vector/optional/set, 2 for map (1.6: key, value), `extent` for variant, else 0 |
| `u64 type_hash` | struct/enum only |
| `u32 extent` | array count / variant alternative count / (1.6) the entry stride of a map or set block |
| name bytes, then `FIELD × n_elem` | element descriptors; a map's or set's element `offset` is the offset inside one entry (key 0, value at its canonical entry offset) |

**RRI** (Raw Registry Image) — 40-byte header then `ENTRY × n_entries`:

| offset | field |
|---|---|
| 0 | `char[4] "ESRG"` |
| 4 | `u16 major` (1) |
| 6 | `u16 minor` (7; 6 in a 1.6 file) |
| 8 | `u32 flags` (bit0 = delta — 1.7) |
| 12 | `u32 hash_algo` (2 = XXH3-64) |
| 16 | `u32 n_entries` |
| 20 | `u64 program_hash` |
| 28 | `u32 reserved` |
| 32 | `u64 abi_tag` (1.3) |

**FILE** — 48-byte header, then the opening dictionary at `dict_offset` = 48 (an RRI of `dict_len` bytes, padded
to 8), then — 1.7 — the 16-byte CHAIN, then `FRAME × N`:

| offset | field |
|---|---|
| 0 | `char[4] "ESSR"` |
| 4 | `u16 major`, `u16 minor` |
| 8 | `u32 flags` (bit0 LE, bit1 dict present, bit2 back-patched — 1.6 files only, bit3 crc_all, bit4 checkpoints, bit5 writer info — 1.7) |
| 12 | `u32 hash_algo` |
| 16 | `u64 program_hash` |
| 24 | `u64 dict_offset` |
| 32 | `u64 dict_len` |
| 40 | `u64 abi_tag` (1.3; was reserved) |

**CHAIN** (1.7) — right after the padded opening dictionary, at `align8(48 + dict_len)`:

| offset | field |
|---|---|
| 0 | `u64 next_dictionary` — the first dictionary frame (a delta) after the opening block; 0 until one is written |
| 8 | `u64 first_checkpoint` — the first checkpoint frame; 0 until one is written |

Each word is written as zero and patched once, with one aligned 8-byte `pwrite`, after the frame it points at
exists. A 1.6 file has no CHAIN; its frames start right after the padded dictionary (or, back-patched, after the
original dictionary area, ending at `dict_offset`).

**FRAME** — 16-byte header:

| offset | field |
|---|---|
| 0 | `u32 frame_len` (whole frame incl. header, multiple of 8) |
| 4 | `u16 flags` (bit0 full records, bit1 dictionary frame; 1.7: bit2 CRC32C trailer, bit3 checkpoint frame, bit4 writer-info frame) |
| 6 | `u16 n_records` |
| 8 | `u64 call_hash` |

Records follow, each starting 8-aligned:

| mode | record |
|---|---|
| compact, flat type | raw bytes (size from the dictionary) |
| compact, literal | **nothing** (1.1, FR-L6); `u64 id` in 1.0 streams |
| compact, constant | **nothing** (1.5): the value is in the call ENTRY |
| compact, scalar | raw bytes (1.1) |
| compact, string | `u32 len, bytes` (1.1) |
| compact, dynamic type | `u32 len, u32 0, canonical image` (1.2); a container argument (1.6) is this record, its image the slot plus its tail |
| full | `u64 hash, u32 payload_len, u16 layout(0 = native), u16 rsv, payload, pad to 8`; payload is the object / `u64 id` / scalar bytes / `u32 len + bytes` / `u32 len, u32 0, image` — identical to the compact record for strings and images. A constant (1.5) is written as the scalar or enum record of its value |
| dictionary frame | `call_hash = dict_frame_sentinel`, `n_records = 0`, payload = one RRI (a delta since 1.7), pad to 8, and when the RRI's minor is ≥ 7 the `u64 next_dictionary` link (0 = none yet) |
| CRC32C trailer (1.7) | with `frame_flag_crc` the last 8 bytes of the frame are `u32 crc32c(frame[0, frame_len − 8))`, `u32 0`; the records end before them; a frame whose CRC does not match is reported and skipped, never decoded |
| checkpoint frame (1.7) | `call_hash = checkpoint_sentinel`, `n_records = 0`, payload = `u16 version (1), u16 flags (bit0 a full RRI follows), u32 rri_len, u64 prev_checkpoint, u64 next_checkpoint (patched), u64 data_frames_before, u64 bytes_before, u64 wall_clock_ns, u64 latest_dictionary_offset, u64 registry_version` (64 bytes), then the RRI if any |
| writer-info frame (1.7) | `call_hash = info_sentinel`, `n_records = 0`, payload = `u16 version (1), u16 n_items`, then `ITEM × n` = `u16 key (info_key), u16 len, bytes, pad to 2`; unknown keys are skipped by readers |

### `cursor` and `writer`

`cursor` is the FR-D5 guarantee in one place: every read checks `can(sizeof(T))` first and reads with `memcpy`,
so corrupt input can only produce `false`, never UB:

```cpp
template<class T> bool read(T& out) noexcept
{
    if (!can(sizeof(T)))
        return false;
    std::memcpy(&out, p, sizeof(T));
    p += sizeof(T);
    return true;
}
```
(`format.hpp:266`). `writer` is unbounded on purpose — its callers (`put_frame_header`, `file_header`,
`write_dictionary_frame`, `to_rri`) size the buffer first from `frame_size`/`align8`; the invariant is "measure,
then write exactly that many bytes".

### Reference slots and the byte budget

A canonical image (format 1.2+) has two decoders: the typed one in `reflect.hpp` (`decode_value`, which splices
private members and so has to live there) and the dictionary-driven walker in `src/decoder.cpp` (`srdump`,
`verify`, `walk_values`). Two copies of the slot checks would disagree (review G2-6), so both call
`image_budget::take`, which has no reflection dependency:

```cpp
constexpr bool take(const slot s, const std::uint64_t bytes) noexcept
{
    if (s.n == 0)
        return s.off == 0;
    if (s.off < fixed || s.off > image.size() || bytes > image.size() - s.off || bytes > left)
        return false;
    left -= bytes;
    return true;
}
```
(`format.hpp:329-337`). An empty slot must be `{0, 0}`, which is what every writer emits. A non-empty block
starts at or after the root's fixed part and lies inside the image. The block's bytes are charged to `left`, which
starts at the image size. Writers bump-allocate disjoint tails, so an honest image can never run out. Slots that
share a tail can: without the budget, 64 string slots over one 3 000-byte tail would decode into 192 000 bytes,
and a 1 MiB image could ask for gigabytes. Both decoders turn a `false` into `error::length_mismatch`.

A vector's block is `s.n` elements of one stride. Both decoders charge it (D-01)
through `take_n`, which computes the product with `__builtin_mul_overflow` and refuses one that wraps 64 bits:

```cpp
constexpr bool take_n(const slot s, const std::uint64_t stride) noexcept
{
    std::uint64_t bytes{};
    if (__builtin_mul_overflow(std::uint64_t{s.n}, stride, &bytes))
        return false;
    return take(s, bytes);
}
```
(`format.hpp:341-347`). With sizes taken from a dictionary, `n` is below 2^32 but the stride can reach about
2^33, so `n × stride` can wrap; unchecked, a wrapped product would be charged to the budget as a small block. These rules only
tighten the reader: they reject nothing a writer of this library has produced, so they are not a format change.

### `cbuf` and the two-pass blob freeze

`std::memcpy` is not usable in constant evaluation; `std::bit_cast` to `std::array<std::byte, sizeof(T)>` is.
`cbuf::put` uses that, `begin_entry` writes a zero `total_len` placeholder plus tag/version/reserved, and
`end_entry` pads to 8 and back-patches the length:

```cpp
constexpr void end_entry()
{
    pad8();
    const auto len = std::bit_cast<std::array<std::byte, 4>>(static_cast<std::uint32_t>(v.size()));
    for (int i = 0; i < 4; ++i)
        v[static_cast<std::size_t>(i)] = len[static_cast<std::size_t>(i)];
}
```
(`format.hpp:410`). A `std::vector` cannot outlive constant evaluation, so `to_blob_array<P>()` evaluates
`P::bytes()` twice — once inside the array's size expression, once to copy the contents:

```cpp
template<class P> consteval auto to_blob_array()
{
    std::array<std::byte, P::bytes().size()> out{};
    const auto v = P::bytes();
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = v[i];
    return out;
}
```
(`format.hpp:421`). `P` is one of `literal_blob_p<S>`, `type_blob_p<T>`, `call_blob_p<Ts...>` (small structs
with a static `consteval bytes()`), so a blob's construction cost is paid twice per instantiation; this is why
`blob.hpp` keeps the builders cheap and why the memoised hashes in `reflect.hpp` matter.

## Maintaining it

- **The layout comment is normative.** `docs/ser-reg-architecture.md` (the byte layouts) and
  `docs/plans-for-v2.0.md` §2 restate it; `tests/canary/rri_canary.cpp` copies its offsets as literals. When you change bytes, update the
  comment, the canary, `tests/golden/blobs.cpp` (blob hashes and sizes, plus the byte-position asserts such as
  `blob_of<T>()[32] & struct_flag_flat` and `pb[16]`, `pb[18]`, `pb[24]`…), the fixtures in
  `tests/fixtures/golden/` (regenerated by `make_fixtures`/`make_dynamic_fixtures`), and bump `format_minor` (or
  major) with a `CHANGELOG.md` entry — never silently.
- **Minor-version discipline.** Readers accept the minors `format_minor_oldest`…`format_minor` (1.6 and 1.7)
  and reject anything else, and unknown frame flags, with `bad_version` (`read_frame_header`,
  `dictionary::add_rri`, `internal::parse_file_header`), so a new flag bit is a minor bump by construction. 1.3
  dropped the reading of 1.0–1.2 together with FNV-1a (the library was not yet in wide use); 1.4 (new
  canonical names), 1.5 (constants) and 1.6 (containers) each refused the minor before them; 1.7
  is the first to keep reading its predecessor — its features are recognised by their flags, and a 1.6 file
  has none of them. DECISIONS G-2: until 3.0 the minor is the compatibility unit, after 3.0 the wire format is
  additive only. The canary (`format_canary_rri`), the golden fixtures and the 1.6 corpus seeds are the regression
  net.
- **The ABI tag** is written from `host_abi_tag` by the capture file (FILE +40) and `registry::export_image`
  (RRI +32), and from the source's tag by `dyn::to_rri`. Adding a field means a new tag `version`; readers
  print an unknown version as `unknown(version N)` and still compare the whole `u64`.
- **Reserved slots already spent:** the call ENTRY's `u16 rsv` after `n_args` became `flags` (1.1); ARG's first
  `rsv` byte became `kind` (1.1). A constant's value (1.5) did not spend a reserved slot: it is 8 new bytes after
  the ARG. 1.6 spent the enum ENTRY's `u8 rsv` (now `flags`, bit 0 `fixed`) and struct flag bit 1
  (`transparent`). 1.7 spent FRAME flag bits 2–4 (CRC trailer, checkpoint, writer info), FILE flag bits 3–5, RRI
  flag bit 0 (`delta`, written by 1.7 writers) and call flag bit 2 (`log`); the checkpoint and writer-info
  data are frames, not ENTRY tags. Remaining: ENTRY `u16 reserved` and tags 5+, struct `u8 rsv`, literal `u32 rsv`,
  call `u32 rsv` and flags bits 3+, ARG `rsv[6]`, FIELD `u8 rsv`, RRI `u32 reserved` and `flags` bits 1+, FILE flags
  bits 6+, FRAME flags bits 5+, checkpoint flags bits 1+, full-record `u16 layout` values ≠ 0 and `u16 rsv`. (The
  FILE `u64 reserved` became the ABI tag in 1.3.)
- `dict_frame_sentinel` is a §11 default; a real call hash colliding with it is treated as a dictionary frame
  (`frame_header::dictionary()` also checks the flag). Do not change the value. The 1.7 sentinels are recognised
  only together with their flag bit (a checkpoint or writer-info frame needs both), which is also what
  `--resync` and the Wireshark heuristic look for.
- `cursor::read` is not `constexpr` (memcpy); `skip`/`can`/`remaining` are. Keep it that way — `cbuf` exists for
  the constexpr side.
- Fuzzing: `fuzz_rri` (`add_rri`), `fuzz_capture` (`verify`+`dump`), `fuzz_elf` (`extract_section_from_image`)
  in `tests/fuzz/` exercise every `cursor` consumer; run them under the ASan/UBSan preset after touching a parser.
- **Slot rules live in `image_budget` only.** A new rule for dynamic images goes there, so the typed decoder and
  the tools walker stay identical; the `[g2-6]` tests in `test_dynamic.cpp` check that both reject the same bytes.
- **One nesting limit.** `max_nesting` is the only depth constant; the typed decoder uses it as is and the walker
  as `2 * max_nesting` (it spends two depth units per struct level), so a type the typed decoder accepts is never too deep for
  `srdump` (D-10).
- Tests: `tests/unit/runtime_selftest.cpp` and `tests/unit/test_capture.cpp` (FILE/RRI/dictionary frames),
  `tests/unit/test_print.cpp` "FR-L6 a compact literal occupies no bytes; full records keep the id",
  `tests/unit/test_dynamic.cpp` "2.0 §2 byte-exact canonical image of the plan example" (the `image_prefix`),
  `tests/golden/hashes.cpp` "FR-S3 frame sizes"; 1.5: the constant call ENTRY in `tests/golden/blobs.cpp`,
  `tests/unit/test_roundtrip.cpp` "FR-A5 constants: no bytes in a compact frame, the value's record in a full one
  (1.5)", and `tests/canary/rri_canary.cpp`, which reads a constant's ARG from literal offsets; 1.6: the flag-byte
  and transparent-flag checks in `tests/golden/blobs.cpp` and `tests/unit/test_containers.cpp`; 1.7:
  `tests/unit/test_format17.cpp`, the golden `features.ser` and the `python_reader_parity` test, whose reader
  (`python/serreg/format.py`) restates every layout above.
