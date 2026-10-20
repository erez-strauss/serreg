# `include/es/serreg/to_string.hpp`

*(DECISIONS A-10.)* The text and JSON forms of any value the library can serialise, rendered from
the process's own dictionary: `to_string(order)` gives `Order{id: 42, px: 101.5, sym: "AAPL"}` — the form a print
frame shows for a struct argument — and `to_json(order)` the value `srdump --json` would print. An opt-in
`std::formatter` makes `std::format("{}", v)` / `std::println("{}", v)` work for a type the user marks
`formattable`. No per-type code: the renderer is the dictionary-driven one the tools use, so every type —
flat or dynamic, enums, containers, nested types — renders the same way `srdump` renders it. It is a debugging and
logging convenience, **not for the hot path**: an allocation for the record and one for the string, a shared lock
and a dictionary walk per call (serialise the value instead and let a reader render it). The header is
header-only but links **`es::serreg_tools`** (decision G: the renderer lives there), and it needs reflection.
`serreg.hpp` does not include it, so a program that links only `es::serreg` is unaffected.

## Depends on / used by

Includes: `es/serreg/arg.hpp` (`arg_kind_v`, `arg_hash_v`, `detail::arg_scalar_kind`), `es/serreg/error.hpp`,
`es/serreg/registry.hpp` (`detail::touch_arg`), `es/serreg/serialize.hpp` (`detail::record_payload_bytes`,
`detail::put_value_record`), `es/serreg/tools.hpp` (`tools::render_value`, `tools::process_dictionary`,
`tools::format`), `<format>` and the usual containers.

Included by: nothing in the library. Users: `tests/unit/test_to_string.cpp`. Implemented in `src/formatters.cpp`
(`tools::render_value`, `tools::process_dictionary`).

## What it defines

| name | kind | meaning |
|---|---|---|
| `detail::record_of(v, payload&)` | function template | the compact record of one value, as `put_value_record` writes it into a frame (padded to 8); `payload` receives the record's size |
| `render(v, format = text)` | function template → `result<std::string>`, for a `serializable_arg` that is neither a literal nor a constant | registers the type, encodes the record, renders it; carries the error |
| `to_string(v)` | function template → `std::string` | `render(v, text)`; an error becomes `"<error: code>"` in the string |
| `to_json(v)` | function template → `std::string` | `render(v, json)`; an error becomes `{"error":"code"}` |
| `formattable<T>` | trait, `std::false_type` | specialise as `std::true_type` to get the formatter below |
| `std::formatter<T, char>` | partial specialisation, constrained on `formattable<T>::value` | spec `""` → `to_string`, `"j"` → `to_json` |

## How it works

### Register, encode, render

```cpp
template<class T>
    requires(detail::serializable_arg<T> && !is_literal_v<std::remove_cvref_t<T>> &&
             !is_constant_v<std::remove_cvref_t<T>>)
result<std::string> render(const T& v, const tools::format f = tools::format::text)
{
    using U = std::remove_cvref_t<T>;
    detail::touch_arg<U>(); // registers U (and its component types) exactly as serialize would
    std::size_t payload = 0;
    const std::vector<std::byte> rec = detail::record_of(v, payload);
    return tools::render_value(tools::process_dictionary(), arg_kind_v<U>, detail::arg_scalar_kind<U>(),
                               arg_hash_v<U>, std::span<const std::byte>(rec).first(payload), f);
}
```
(`to_string.hpp:60-73`.) Three steps:

1. **Registration.** `touch_arg<U>()` odr-uses the same guards a `serialize` call would (`registry.hpp`), so
   instantiating `to_string<Order>` anywhere puts `Order` and its component types into the registry before
   `main()` — and into the ELF dictionary section and every capture file's dictionary — whether or not the program
   ever serialises an `Order` ("A to_string registers the type and renders the one-line text form; to_json the
   JSON value").
2. **Encoding.** `detail::record_of` (`to_string.hpp:44-55`) writes the compact record a frame would carry: an
   object's bytes (scrubbed), a dynamic type's `u32 len, u32 0, canonical image`, a scalar's bytes, a string's
   `u32 len, bytes`.
3. **Rendering.** `tools::render_value` (`src/formatters.cpp`) takes the record with its category, scalar kind and
   schema hash and renders it from `tools::process_dictionary()` — the registry's own image, parsed once per
   registry version and shared under a `std::shared_mutex` (the first call after a `dlopen` pays the parse).
   Text is `arg_as_text`, the one-line form `render_message` uses for a struct argument; JSON is the dump's
   `json_visitor`.

### The forms

| value | `to_string` | `to_json` |
|---|---|---|
| struct | `ns::Type{a: 7, b: 2, c: 1.5}` | `{"a":7,"b":2,"c":1.5}` |
| enum | `green` (top level); `ns::Colour::red (1)` as a member | `{"name":"green","value":2}` |
| scalar | `-5`, `2.5`, `true` | `-5`, … ; 64-bit integers as JSON strings (`"18446744073709551615"`, gk 1.12) |
| string | `"hi there"` (quoted) | `"q\"uote"` (escaped) |
| vector / map (also as the value itself) | `[10, 20]`, `{"a": 1, "b": 2}` | `[10,20]`, `[{"key":"a","value":1},…]` |

The table restates the expectations of `tests/unit/test_to_string.cpp`.

### The opt-in formatter

```cpp
template<class T>
    requires(es::serreg::formattable<T>::value && requires(const T& v) { es::serreg::render(v); })
struct std::formatter<T, char>
```
(`to_string.hpp:100-102`.) Opt-in because a formatter for every serialisable type would hijack `std::format` for
types that already have one. `parse` accepts an empty spec or `j`; `format` copies `to_string(v)` or `to_json(v)`
to the output ("A formattable<T>: std::format and std::print render through to_string; {:j} is the JSON form").

## Maintaining it

- **No second renderer.** `to_string`/`to_json` must stay byte-identical to what `srdump` prints for the same
  value; change the renderers in `src/formatters.cpp`, not here.
- **`process_dictionary()` is a cache of `export_image()`.** Its returned reference is read after the shared lock
  is released; a concurrent re-parse after a `dlopen` replaces the dictionary it refers to (see the report on
  `tools.hpp`). Call it from one thread around `dlopen`/`dlclose`.
- A value whose type is unknown to the process dictionary cannot occur after `touch_arg` (the type registered
  itself); `unknown_type` is kept for the case of an entry gone with a `dlclose` edge.
- Tests: `tests/unit/test_to_string.cpp` (the three cases above), linked with `es::serreg_tools`.
