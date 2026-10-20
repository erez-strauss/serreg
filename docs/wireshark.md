# Wireshark dissector generation (`srdump --wireshark`)

`srdump --wireshark <file.lua>` writes a Wireshark dissector, in Lua, for the types of a program. Wireshark then
opens `.ser` captures directly, shows every frame as a packet with its records decoded into a tree, and lets you
filter on any member: `serreg.corpus_Order.qty >= 100`.

The dissector is **generated from a dictionary**: the one embedded in a capture, the `es_serreg_dict` section of
a binary, or an `.rri` file. It is a snapshot of that dictionary. No C++ type is compiled into it, and nothing is
parsed from the capture's own dictionary to decode frames. Each frame is matched to the generated tables by the
same XXH3-64 ids the wire carries, so a frame the dissector does not know is shown as raw bytes and flagged. It is
never decoded with a wrong layout.

## Generate

```
srdump --wireshark serreg.lua capture.ser                        # the capture's own dictionary
srdump --wireshark serreg.lua --elf ./app --elf ./libplugin.so   # the program's embedded dictionaries
srdump --wireshark serreg.lua --dict app.rri                     # an RRI written by Registry::export_image
srdump --wireshark - capture.ser > serreg.lua                    # to standard output
```

Sources can be combined (`capture.ser --elf ./app`); all are merged. Two sources that define one schema hash with
different layouts are refused (exit 1, one `schema conflict` line per type), as for the dictionary dump. Several
versions of a program can share one dissector: two versions of a type have different hashes, so generate from all
the binaries at once.

An application that embeds the command line (`cli::run`, `examples/embedded_cli.cpp`) can describe **its own
registry**: `your-app --wireshark serreg.lua` with no other source writes a dissector for every type the
program has registered, loaded plugins included. (srdump itself has no types, so it needs a source.)

From C++: `std::string tools::wireshark_dissector(const dyn::dictionary&, const tools::wireshark_options&)`
(`include/es/serreg/tools.hpp`).

## Use

- **Wireshark**: copy the file to your personal Lua plugins folder (Help → About Wireshark → Folders → "Personal
  Lua Plugins", usually `~/.local/lib/wireshark/plugins/`) and restart. Open a `.ser` file. Wireshark cannot run
  Lua plugins as root.
- **tshark**, without installing:
  ```
  tshark -X lua_script:serreg.lua -r capture.ser                   # one line per frame
  tshark -X lua_script:serreg.lua -r capture.ser -V                # the full trees
  tshark -X lua_script:serreg.lua -r capture.ser -Y 'serreg.corpus_Order.qty >= 100'
  tshark -X lua_script:serreg.lua -r capture.ser -T fields -e serreg.corpus_Order.id -e serreg.message
  ```

Example (`tests/fixtures/golden/orders.ser`, one line per packet):

```
    1   SERREG 1264 file header, format 1.3; dictionary: 16 entries known, 0 unknown
    2   SERREG 48 "new order", corpus::Order
    4   SERREG 32 corpus::Cancel, "why"
    7   SERREG 32 hello new 100 worlds
    8   SERREG 40 px=2.50 qty=7 side=sell
```

and the tree of packet 2 (`-V`):

```
es::serreg frame
    Frame length: 48
    Records: 2
    Call hash: 0xbcf8e25bde66fdb2
    Kind: compact
    Literal: new order
    [1] corpus::Order
        Type: corpus::Order
        id: 42
        px: 1050000
        qty: 100
        side: buy (1)
        sym: AAPL
```

## Where frames come from

| Source | How |
|---|---|
| FILE captures (`.ser`) | The generated file reader. Packet 1 is the FILE header and the embedded dictionary; then one packet per frame. A back-patched file's relocated dictionary is the last packet. Frames have no timestamps. |
| pcap/pcapng, link type USER0 (147) | One or more frames per packet (dictionary frames too). Any tool that can wrap frames in a pcap works. |
| UDP | Recognised automatically: a datagram whose first 16 bytes are a frame header with a call hash from the dissector's tables. Or set Preferences → Protocols → SERREG → UDP port (`-o serreg.udp_port:N`). One or more frames per datagram. |
| TCP | Recognised automatically at the start of a stream, the same test; the rest of the conversation follows. Or set the TCP port (`-o serreg.tcp_port:N`). Frames that span segments are reassembled. |

`es::ipc` rings live in shared memory, which Wireshark cannot capture. Write the frames to a `capture_file` (or
to a socket) to look at them.

## Filter fields

| Field | Meaning |
|---|---|
| `serreg.<type>.<member>` | a member of a struct, e.g. `serreg.corpus_Order.qty`. The type name keeps letters, digits and `_`; every other run of characters becomes one `_` (`corpus::Order` → `corpus_Order`, `wide::tick<7>` → `wide_tick_7`); a clash gets `_<8 hex digits of the schema hash>` appended. Members clash the same way (`y` and `y_`): the later member gets `_<its index>` appended. Array, vector and optional elements use the member's field; variant alternatives use `<member>.alt<i>`. |
| `serreg.<enum type>` | a top-level enum argument, with its enumerator names |
| `serreg.literal`, `serreg.arg.string`, `serreg.arg.<kind>` | literal records, runtime strings, scalar arguments (`serreg.arg.i32`, `serreg.arg.f64`, …) |
| `serreg.type` | the type name of each struct record (`serreg.type == "corpus::Order"`) |
| `serreg.call.name` | the frame's signature, e.g. `("new order", corpus::Order)` or `println("px={:.2f} qty={}", f64, u32)`: `serreg.call.name contains "Order"` |
| `serreg.message` | the rendered text of a `print`/`println` frame |
| `serreg.frame.len`, `.flags`, `.records`, `.call`, `.kind` | the frame header; `kind` is `compact`, `full` or `dictionary`, plus `print`/`println` |
| `serreg.file.*`, `serreg.dict.*` | the FILE header and the dictionary comparison (`serreg.dict.unknown > 0`) |

Values are shown as their C++ types: integers in decimal, `char` arrays as strings, `bool` as True/False, enums
as `name (value)`.

## The capture's own dictionary: compared, not used

The dissector does not decode with the capture's dictionary; it compares it with its own, by hash:

- `serreg.dict.known` / `serreg.dict.unknown` count the capture's entries the dissector has / lacks; the names of
  up to five unknown types are listed. Unknown entries raise the expert info
  `serreg.dict.unknown_entries` ("regenerate with `srdump --wireshark`").
- A type the dissector knows under the same schema hash but with another layout hash raises
  `serreg.dict.layout_mismatch` (error): the capture was written by a program whose type differs only in layout
  (review FR-R4).
- A different program hash is a note (`serreg.dict.program_differs`); a different ABI tag a warning
  (`serreg.abi_differs`).

Per frame, the call hash decides: a call hash in the generated tables covers the schema hash of every argument, so
the frame is decoded exactly as it was written. A call hash it does not know is `serreg.unknown_call`; the frame's
records are shown as one block of bytes.

## What it checks

The walk applies the same rules as `srdump` (`src/decoder.cpp`, `fmt::image_budget`): every slot of a dynamic
member must satisfy `n == 0 ⇔ off == 0`, lie after the root's fixed part and inside the image, and all blocks of
an image together fit in its size. A frame or record that breaks a rule is marked `serreg.malformed` and the next
frame is dissected; a corrupt capture never stops the dissector (`tests/wireshark/parity.py fuzz`: 3000 corrupted
frames). A `bool` byte other than 0/1 is shown as True with `serreg.noncanonical_bool` (review G2-12).

The automatic recognition needs a 64-bit call hash known to the dissector, so a false positive would take a
random match of 64 bits; turn it off in Analyze → Enabled Protocols (`serreg_udp_heur`, `serreg_tcp_heur`) if you
want ports only.

## Print messages

`print`/`println` frames are rendered as std::format renders them: argument ids (`{1} {0}`), fill and
alignment (`{:*>8}`, `{:^9}`), sign (`{:+}`, `{: }`), `#`, zero padding (`{:08.2f}`), width, precision (`{:.3}` on
strings and numbers), integer bases (`x X o b B d`, 64-bit values included), floating-point types (`e E f F g G a
A`, and the shortest round trip for `{}`), bool and char as text or number (`{:d}`). `tests/wireshark/print_capture.cpp`
writes 14 lines that cover them, and the parity test requires Wireshark's text to equal srdump's for each.
Not rendered: nested replacement fields for width or precision (`{:{}}`) — the field is shown as written.

## Differences from srdump

- Enumerators whose value does not fit in 53 bits are left out of the value table (Lua numbers are doubles).
  Such a value is shown as a number.
- 64-bit enums (8 bytes) show the enumerator name appended to the number; Wireshark has no 64-bit value strings
  for Lua fields.

## Compatibility

The generated file runs on Wireshark with Lua 5.2 (up to 4.4) and 5.4 (4.6 and later): it uses no `bit32` and no
integer operators, compares 64-bit ids as hex strings, and turns whole numbers back into integers before they
become text (Lua 5.4 prints the float 7.0 as `7.0`). It is tested with Wireshark 4.2.2 (Lua 5.2) and 4.6.8 (Lua
5.4), and every build runs its FILE-capture checks under plain Lua 5.2 and 5.4 (`wireshark_dissector_lua52`,
`wireshark_dissector_lua54`, below). Dissectors generated by older releases may show octal and binary print
fields and 8-byte enum values wrongly under Lua 5.4 (`{:o}` of 48879 as `0.10.30.70.30.50.7`); regenerate them.

## Tests

- `wireshark_generate`: the generated file for `orders.ser` has the expected filter fields (no Wireshark needed).
- `wireshark_dissector_parity` (`tests/wireshark/parity.py`, needs tshark and Python 3; skipped otherwise): for
  the golden captures, a back-patched capture, a USER0 pcap, UDP datagrams, a TCP stream with a frame split across
  segments (each with a port setting and recognised automatically), the print-format capture, and
  dissectors generated from the ELF sections of `make_fixtures` / `make_dynamic_fixtures`, every value tshark shows
  (in order) equals what `srdump --json` shows, and the rendered print messages are equal.
- `wireshark_dissector_lua52`, `wireshark_dissector_lua54` (need `lua5.2` / `lua5.4` and Python 3, no
  Wireshark): the `fixture`, `backpatched`, `prints` and `elf` checks with the dissector running under a plain Lua
  interpreter. `tests/wireshark/wslua_stub.lua` stands in for Wireshark's Lua API: it drives the generated file
  reader over the capture and prints the tree as PDML, which `parity.py --lua` compares with srdump as above.
- `example_embedded_cli_wireshark`: an embedding application writes the dissector of its own registry. It
  also checks the unknown-dictionary case and runs the 3000 corrupted frames. As root it runs tshark through
  `runuser -u nobody`, because Wireshark does not run Lua scripts for root.
