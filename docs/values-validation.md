# Value validation — validation hints as attributes, checked by compile-time policy

**Status:** proposal, 2026-10-08, against library 2.2.4 / wire format 1.7. No code has changed. Nothing here is
decided. If the maintainer accepts it, it becomes DECISIONS entries (an A-* entry for the API and a P-* entry for
the dictionary record), and the open questions in §9 become decision rows.

**The idea in one paragraph.** A member can carry validation *hints* as C++26 annotations, the same mechanism as
`since`, `deprecated` and `skip`: for example `[[=es::serreg::range(1, 1'000'000)]]`. The hints never change
the bytes on the wire. Whether they are checked is a **compile-time policy**, chosen separately for the writer
and for the reader: off, report, or reject. The hints are also recorded in the dictionary. That lets `srdump
--verify`, the Python reader and the Wireshark dissector check an archive *offline*, against the constraints the
writer declared, without the writer's source. No surveyed serialization library combines these two things.

---

## 1. What already exists: the enum range check (a policy, off by default)

The library already validates one kind of value:

- An enum **without a fixed underlying type** (`enum E { … }`, no base) may only hold values in the range of the
  smallest bit-field that holds all its enumerators ([dcl.enum]). Holding any other value is undefined behaviour.
- With `ES_SERREG_VALIDATE_READ=1` (a compiler define, per translation unit — the same pattern as
  `ES_SERREG_LOG_MIN_LEVEL`) the **typed readers** check every such enum leaf on every read. The check is a
  compile-time table (`enum_ranges_v<T>`) that compiles to nothing when the type has no such leaf. A bad value is
  set to 0, decoding continues, and the call reports `error::invalid_value`. `view<T>` refuses the frame instead.
- With the default (`ES_SERREG_VALIDATE_READ=0`) the tables are not instantiated and the bytes are copied as they
  are. The dictionary readers (`srdump --verify`, the Python reader) never check enum values.

This is the first piece of the read-side policy of §5.4 (`off` and `report`; `reject` comes with the rest). A
reader that must not hold an out-of-range enum value either turns the policy on or gives the enum a fixed
underlying type.

Everything else in this document is different in kind. A value outside a declared range is a **domain error**,
not undefined behaviour: a `std::int64_t px = -5` is a perfectly valid object. So checking it is a policy
decision, made per side, with zero cost when it is off.

## 2. What other libraries do

Pages opened on 2026-10-08 where noted; the rest is from each project's documentation as generally known, marked
"(not re-checked)".

| System | Where constraints are declared | What can be expressed | When they are checked | Constraints travel with the data? |
|---|---|---|---|---|
| **protovalidate** (Buf; successor of protoc-gen-validate) | annotations on `.proto` fields | numeric `const`, `lt`, `lte`, `gt`, `gte`, `in`, `not_in`, `finite` (floats); string length, `pattern`, `prefix`/`suffix`/`contains`, formats (`email`, `uuid`, `ip`, `uri`, …); bytes likewise; enum `defined_only`, `in`, `not_in`; repeated `min_items`, `max_items`, `unique`; map `min_pairs`/`max_pairs`, keys/values; `required`; message- and field-level CEL expressions | an explicit validation call in each language runtime, separate from (de)serialization | no — they are in the `.proto` descriptor, not in the message |
| **Protocol Buffers** itself (not re-checked) | `.proto` | no value constraints. Enums are *open* (unknown values kept) or *closed* (unknown values moved to unknown fields) — proto2 closed, proto3 open, editions choose per enum | on parse (enum openness only) | no |
| **SBE** (FIX Simple Binary Encoding) | XML schema: `minValue`, `maxValue`, `nullValue`, `presence` (`required`/`optional`/`constant`) on encoded types | numeric bounds and an explicit null sentinel | the schema states them; generated codecs expose them as constants. To our reading the reference codecs do not reject out-of-range values on decode | no — the schema is out of band |
| **Kaitai Struct** (format descriptions, parsers only) | `valid:` key on a field (since 0.9) | equality, any-of, less/greater than, arbitrary expression | **while parsing**; failure throws `ValidationNotEqualError`, `ValidationAnyOfError`, `ValidationLessThanError`, `ValidationGreaterThanError`, `ValidationExprError` | the `.ksy` is out of band |
| **ASN.1** (not re-checked) | constraints in the type: `INTEGER (0..255)`, `SIZE (1..10)`, permitted alphabets | ranges, sizes, sets, extensibility markers | by the codec. **PER uses the constraints to choose the encoding width** — a constraint changes the bytes | the module is out of band |
| **JSON Schema** (not re-checked) | the schema document | `minimum`, `maximum`, `exclusiveMinimum`/`exclusiveMaximum`, `multipleOf`, `minLength`/`maxLength`, `pattern`, `format`, `enum`, `const`, `minItems`/`maxItems`, `uniqueItems`, `required`, conditionals | by a validator, separate from parsing | sometimes (`$schema` link), usually out of band |
| **Rust `validator` crate** | `#[validate(...)]` derive attributes on fields | `range`, `length`, `email`, `url`, `contains`/`does_not_contain`, `regex`, `custom`, `must_match`, `required`, `credit_card`, `non_control_character` | an explicit `.validate()` call after deserializing with serde | no |
| **Pydantic** (Python; not re-checked) | `Field(ge=, le=, gt=, lt=, multiple_of=, min_length=, max_length=, pattern=)`, validator functions | ranges, lengths, patterns, custom | **during model construction / parsing** | no |
| **Avro** (not re-checked) | the schema | enum symbols (with a default for unknown symbols on read), logical types (decimal precision/scale); no numeric ranges | on read (enum resolution) | the container file embeds the schema |
| **FlatBuffers, Cap'n Proto** (not re-checked) | the schema | none for values. Their *verifiers* check structure (offsets, bounds, depth), not values | verifier before access | no |
| **cereal, Boost.Serialization, bitsery** (not re-checked) | hand-written functions | none built in; whatever the user's function checks | in the user's code | no |
| **C++26 contracts** (P2900; GCC `-fcontracts`) | `pre`, `post`, `contract_assert` | any boolean predicate | per the **evaluation semantic**, chosen at build time in GCC (`-fcontract-evaluation-semantic=ignore|observe|enforce|quick_enforce`, default `enforce`) | n/a — a language feature, not a data format |

**What we take from this survey:**

1. **Range, size, membership, finite and required checks are the common core** (protovalidate, SBE, Kaitai,
   ASN.1, JSON Schema, validator, Pydantic). Formats such as email, URI and IP addresses, and regular expressions,
   are where libraries diverge. They belong to application validation, not to a binary log format.
2. **Check timing splits three ways**: during the parse (Kaitai, Pydantic, ASN.1), a separate validation step
   (protovalidate, validator, JSON Schema), or no check at all, with the constraint only exposed (SBE). A
   compile-time policy lets one library offer all three.
3. **Only ASN.1 PER lets constraints change the encoding.** That is incompatible with serreg's `memcpy` flat path
   and canonical image. So the hints here must never change bytes (§3).
4. **None of the surveyed systems records the constraints in the data artifact.** Avro embeds its schema, but the
   schema has no ranges. serreg's dictionary already travels with every capture, which makes **offline validation
   of an archive** possible: "were these 400 GB of last month's orders within the declared limits?" answered by
   `srdump --verify`, without the producer's code. That is the strongest reason to do this.
5. **C++26 contracts already give the right vocabulary for policies**: ignore, observe, enforce. The policy
   names below map onto them, with one change: a reader never terminates, because of R-2.

## 3. Principles

1. **Hints never change the wire.** Same bytes, same offsets, same `memcpy` path, same canonical image, whether or
   not hints are present and whatever the policy. Validation is a check *beside* the encoding, never part of it.
2. **Zero cost when off.** Every check is a compile-time table per type, like `enum_ranges_v<T>`. An empty table,
   or a policy of `off`, removes the code through `if constexpr`.
3. **The policy is compile-time and per side.** The writer and the reader choose independently. The defaults are
   writer `off`, typed reader `off`, tools `report`.
4. **A failed check on read is a soft error, never a crash, never undefined behaviour.** The value is
   representable, so nothing needs repairing. The library reports, and the caller decides.
5. **The writer's constraints travel in the dictionary.** Offline tools check against what the *writer* declared.
   Typed readers check against what *they* were compiled with.
6. **Constraints are metadata, not identity.** They do not enter the schema hash (§9 Q1 is the one real open
   question), so tightening a range does not make old readers return `hash_mismatch`.
7. **No dependencies, no regex engine in the library, no format zoo.** C-6 holds.

## 4. The hints (annotations)

The same annotation mechanism as `since(N)` and `skip` (P3394 annotations, read through `annotations_of`). All
values are compile-time constants, so the tables are built at compile time.

### 4.1 Scalars (integers, floating point, `char`, `bool`)

| Hint | Meaning | Applies to |
|---|---|---|
| `[[=range(lo, hi)]]` | `lo ≤ v ≤ hi` (inclusive) | integers, floats, fixed-type enums (by value) |
| `[[=minimum(lo)]]`, `[[=maximum(hi)]]` | one-sided bounds (inclusive) | same |
| `[[=exclusive_minimum(lo)]]`, `[[=exclusive_maximum(hi)]]` | strict bounds (`>`, `<`) — needed for floats such as "price > 0" | same |
| `[[=one_of(a, b, c, …)]]` | `v ∈ {a, b, c, …}` (a small set, up to 64 values) | integers, `char`, fixed-type enums |
| `[[=not_zero]]` | `v ≠ 0` | integers, floats |
| `[[=finite]]` | not NaN, not ±infinity | floats |
| `[[=multiple_of(k)]]` | `v % k == 0` — tick sizes, lot sizes | integers |

### 4.2 Enums

| Hint | Meaning |
|---|---|
| `[[=es::serreg::closed]]` on the **enum type** | the value must be one of the named enumerators (protovalidate's `defined_only`, a protobuf *closed* enum). Not the default, because flag enums legitimately combine bits (`r | w = 3`). |
| `[[=one_of(…)]]` on a **member** of enum type | this member accepts only these enumerators: a subset per use, such as "an order's side is buy or sell, never the `none` enumerator that other messages use" |

The enum range check for unfixed enums (§1) stays always on and is not a hint.

### 4.3 Dynamic members (`std::string`, `std::vector`, `std::optional`, maps, sets, arrays)

| Hint | Meaning |
|---|---|
| `[[=size(lo, hi)]]`, `[[=min_size(n)]]`, `[[=max_size(n)]]` | element count of a vector, map or set; byte length of a string |
| `[[=non_empty]]` | `size ≥ 1` |
| `[[=engaged]]` | an `optional` must hold a value: protovalidate's `required`, SBE's `presence="required"` |
| `[[=utf8]]` | a string is well-formed UTF-8. The library already has a UTF-8 checker, used for JSON output. |
| `[[=elements(hint…)]]` | apply scalar hints to every element: `[[=elements(range(1, 100))]] std::vector<int> qty;` |
| `[[=keys(hint…)]]`, `[[=values(hint…)]]` | the same for map keys and values |

A string or vector length is already *encoded* on the wire. These hints check its value against the declared
bounds, and `decode_limits` still bounds the work regardless of the hints.

### 4.4 Whole types (cross-member rules)

| Hint | Meaning |
|---|---|
| `[[=es::serreg::check<&pred>]]` on the **class** | a `constexpr bool pred(const T&)` the typed reader and writer call, such as `bid <= ask` or `expiry > start`. Recorded in the dictionary **by name only**: offline tools report "type has a custom check `pred` (not evaluated)" rather than running C++ |

### 4.5 Example

```cpp
enum class Side : std::uint8_t { none = 0, buy = 1, sell = 2 };

struct [[=es::serreg::check<&valid_order>]] Order
{
    [[=es::serreg::not_zero]]                     std::uint64_t id;
    [[=es::serreg::exclusive_minimum(0)]]         std::int64_t  px;     // price in ticks, strictly positive
    [[=es::serreg::range(1, 1'000'000)]]
    [[=es::serreg::multiple_of(100)]]             std::uint32_t qty;    // round lots
    [[=es::serreg::one_of(Side::buy, Side::sell)]] Side         side;
    [[=es::serreg::size(1, 8)]]                   std::string   sym;
};
constexpr bool valid_order(const Order& o) { return o.sym.size() <= 8; }  // a cross-field rule, if needed
```

### 4.6 Compile-time rules on the hints themselves

These are diagnosed at compile time:

- A hint that does not apply to the member's kind (`finite` on an integer, `size` on a scalar).
- Bounds outside the member type's range (`range(0, 300)` on a `std::uint8_t`).
- `lo > hi`, or an empty `one_of`.
- Contradictory hints (`range(1, 5)` together with `minimum(10)`).
- `closed` on an enum without enumerators.

## 5. Policies

### 5.1 Writer side

| Policy | Behaviour | Cost when hints exist | Maps to |
|---|---|---|---|
| `write::off` (default) | no check | none — the code is not generated | contracts `ignore` |
| `write::debug` | check; on failure call the violation handler (`detail::report_line`, or `contract_assert` where the compiler supports contracts), **then write anyway** | one table pass per frame | `observe` |
| `write::reject` | check **before** anything is claimed or written; on failure `serialize` returns `error::invalid_value` and no bytes reach the sink | one table pass per frame, before the copy | `enforce` (as an error, not a termination) |

The check reads the source object, not the claimed buffer. So a rejected frame never claims sink space: there is
nothing to roll back, which matters for `es::ipc` rings and capture files.

### 5.2 Typed reader side

Covers `deserialize`, `dispatch`, `frame_router`, typed `walk_values` and `view<T>`.

| Policy | Behaviour | Maps to |
|---|---|---|
| `read::off` (default) | no check | `ignore` |
| `read::report` | decode normally. Violations are counted in `decode_error` / `frame_walk` (a new `constraint_violations` count and the first failing member), and the call **succeeds** | `observe` |
| `read::reject` | decode normally, then return `error::constraint_violation` (a new soft error, like `invalid_value`: the rest of the frame is still decoded, and the outputs hold the values *as written*). `frame_router` routes the frame to a `on_invalid` handler if one is installed, else counts it as failed. `view<T>` returns the error and no view | `enforce` (soft) |

No reader policy terminates: R-2 rules it out. A reader that must not *act* on a bad value uses `reject` and
handles the error.

### 5.3 Tools (dictionary readers)

Covers `srdump`, the Python reader and the Wireshark dissector.

| Mode | Behaviour |
|---|---|
| default | `--verify` evaluates the **writer's** recorded constraints and lists violations in their own section (`constraint: Order.qty = 150 not a multiple of 100 (frame 17)`). They count as problems and set exit status 1 |
| `--constraints=report` | list violations without failing the verify |
| `--constraints=off` | ignore them |
| dump | marks the value (`qty: 150 ⚠ multiple_of(100)`); JSON adds `"violations": […]` on the record |
| Wireshark | expert info (`serreg.constraint`), filterable |

### 5.4 How a policy is selected

There are three levels; the most specific one wins. The pattern is the same as the logger's compile-time
options.

1. **Translation-unit default:** `ES_SERREG_VALIDATE_WRITE=off|debug|reject` and
   `ES_SERREG_VALIDATE_READ=off|report|reject`, set as compiler defines. This is the analogue of
   `-fcontract-evaluation-semantic=`.
2. **Sink type:** a sink may declare `static constexpr es::serreg::write_policy validation = …;`. So an archive
   capture file can reject bad values while a logger sink stays `off`. Readers take the policy from a router or
   walker option type.
3. **Call site:** `serialize<es::serreg::validate::reject>(out, …)` and
   `deserialize<es::serreg::validate::report>(frame, …)`.

## 6. The dictionary record

Constraints go into a **new ENTRY tag** (`tag_constraints`), one entry per constrained type, keyed by the
type's schema hash. It lists each member path with its hints in a compact form: kind of hint, then one or two
`i64`/`f64` operands or a small value set. `check<&pred>` is recorded only as the predicate's name.

- **No format break:** 1.7 readers already skip unknown ENTRY tags (`dictionary::add_entries` in
  `src/dictionary.cpp`: "future tags are skipped"; an unknown ENTRY *version* is skipped too). A 2.2.x reader
  shown a file with constraints reads it exactly as before and ignores them. The Python reader must follow the
  same rule; that needs to be confirmed in `python/serreg/dictionary.py` before implementation.
- **Same carriers as the rest of the dictionary:** the ELF section, the capture's opening block and its deltas,
  `.rri` files, scoped dictionaries (the constraints of a type travel with the type).
- **Versioning (G-2):** adding an ENTRY tag that old readers skip is additive. It would still ship in a library
  *minor* (2.3.x or 2.4.0), because the API grows.
- `srdump --diff` reports a constraint change as its own verdict, `constraints-changed`, which is breaking under
  no direction. `--diff --compat` (evolution level 1) can warn when a newer reader's constraints would reject
  values an older writer was allowed to write.

## 7. Cost

| Path | Off | On |
|---|---|---|
| **Writer, flat type** | 0 | one pass over the compile-time table, before the copy: per hint a load and 1–2 compares (`multiple_of` adds a division; `one_of` with ≤ 8 values compares inline, larger sets use a binary search on a constant array). Roughly 1 ns per hint. No allocation |
| **Writer, dynamic type** | 0 | sizes are already measured by the encoder's measure pass; element hints run during that pass |
| **Typed reader** | 0 | the same table run on the decoded value (flat: on the completed copy, like the `bool`/enum repair; dynamic: during decode) |
| **`view<T>`** | 0 | the table on the frame bytes, as `bools_canonical` already does |
| **Code size** | 0 | one small table per constrained type; check code per kind of hint, shared across types |
| **Dictionary size** | 0 | the constraints entry: a few bytes per hint (about 16–24), only for annotated types |
| **Compile time** | 0 | one more annotation scan per member; the same order as `since`/`skip` today |
| **Tools** | — | one extra pass per constrained record in `--verify`; irrelevant next to decoding |

The hot path of the library's main users (logging, market data) stays at zero: hints are opt-in per member, and
both runtime policies default to `off`.

## 8. Interactions

- **Enums:** the unfixed-enum range check (§1) is independent and always on. `closed` and `one_of` are the
  policy-governed checks layered on top.
- **Schema evolution (`schema-evolution-specs.md`):** level-1 projection widens values (rules R2–R4), and a
  widened value can violate the *reader's* constraints. The order is projection first, then the reader's policy
  on the projected value. `projection_report` and `--diff --compat` mention constraints that differ between
  versions.
- **Logger:** log sinks default to `off`; a log line is the wrong place to reject data.
- **`as_flat<T>`:** hints apply to the mapped type's members, not the original's.
- **`to_string` / `to_json`:** unchanged. The JSON dump of a record gains `"violations"` only in `srdump`.
- **`--cxx`:** the generated header re-emits the annotations, so a generated reader validates like the original.
- **`decode_limits`:** unchanged. Limits bound the *work*, hints check the *values*. Hints never relax a limit.

## 9. Open questions (for the maintainer)

| # | Question | Options | Recommendation |
|---|---|---|---|
| Q1 | Do constraints enter the **schema hash**? | (a) no — metadata, like `renamed_from`; (b) yes — two definitions with different ranges are different types | **(a).** Tightening a range should not break every typed reader with `hash_mismatch`. `--diff` reports the change. The cost: two binaries can share a schema hash but disagree on constraints, so each typed reader checks with its *own* compiled hints and tools check with the *writer's*. Both behaviours are stated explicitly |
| Q2 | Reader `reject`: what do the outputs hold? | (a) the values as written; (b) value-initialised members | **(a).** The value is representable, so repairing it would hide information. Unfixed enums are the exception (§1), because there the value is undefined behaviour |
| Q3 | Allow a writer policy that **clamps** (`write::clamp`: store the nearest bound)? | yes / no | **No.** Silently changing data in a capture format defeats the purpose of a faithful record |
| Q4 | **Regex patterns** and string formats (email, URI, IP)? | library / tools-only / no | **No in the library** (no regex engine, C-6). Possibly a tools-only `pattern` later, evaluated by `srdump` with `std::regex` and recorded in the dictionary as text |
| Q5 | Cross-member `check<&pred>`: also a dictionary-readable expression language (CEL-like)? | yes / name only | **Name only.** An expression language is a dependency and a parser; offline tools report that the check exists |
| Q6 | Which release? | 2.3.x with evolution / 2.4.0 / later | **After A-16a (2.2.3) and level 1 (2.3.0):** one minor for the annotations, the tables, both policies and the tools (`tag_constraints`). The enum `closed` hint could come earlier, with A-16a, if wanted, since it needs only one flag bit in the enum entry. That depends on whether 1.7 readers ignore unknown enum flag bits, which is unverified |

## 10. What this proposal does not do

- It does not change any byte on the wire, and it does not use constraints to compress (unlike ASN.1 PER).
- It does not add a regex engine, string formats, or an expression language to the library.
- It does not terminate a reader on bad data, or clamp or rewrite values.
- It does not validate *across frames* (sequence gaps, monotonic timestamps). That is application logic, or a
  future `srdump` plugin.
- It does not replace `decode_limits`, the structural checks, or the unfixed-enum range check.

---

## Sources

- protovalidate rule reference: https://protovalidate.com/reference/rules/ — also https://github.com/bufbuild/protovalidate
- Kaitai Struct v0.9 release (`valid` key, validation exceptions): https://kaitai.io/news/2020/10/16/kaitai-struct-v0.9-released.html
- SBE schema (`minValue`, `maxValue`, `nullValue`, `presence`): https://git.speice.io/mirror-github-speiceio/marketdata-shootout/raw/branch/master/sbe.xsd ; overview https://www.baeldung.com/java-sbe
- Rust `validator` crate: https://docs.rs/validator
- C++26 contracts (assertion kinds, evaluation semantics): https://www.cppreference.com/cpp/language/contracts
- GCC contracts implementation (`-fcontracts`, `-fcontract-evaluation-semantic=`): https://gcc.gnu.org/pipermail/gcc-cvs/2026-January/448373.html
- ASN.1 PER, constraint-driven encoding: https://www.wireshark.org/docs/dfref/p/per.html
- Project: `include/es/serreg/reflect.hpp` (`enum_ranges_v`, `copy_flat`, `bools_canonical`), `src/dictionary.cpp`
  (`dictionary::add_entries`, unknown tags skipped), `docs/schema-evolution-specs.md`, `docs/DECISIONS.md` (R-2, C-6, G-2, A-16).
