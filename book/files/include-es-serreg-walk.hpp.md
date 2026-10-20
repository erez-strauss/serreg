# `include/es/serreg/walk.hpp`

`tools::walk_values<Ts...>` walks every value of a capture. A value is **typed** when its type
is one the reader was compiled with, and **generic** otherwise: field by field, from the file's dictionary. It is
included by `serreg.hpp`. The walk also takes a path (read in windows, or mapped, chosen by
`tools::file_options` as for `for_each_frame(path)`), and a walk without compiled types needs only the visitor.

Includes: `<cstddef>`, `<span>`, `<string>`, `<string_view>`, `<type_traits>`, `deserialize.hpp` (`inspect`,
`record_view`, `detail::get_object`), `reflect.hpp` (`schema_hash_v`, `detail::decode_value`), `tools.hpp`
(`for_each_frame[_in_file]`, `visit_record`, `decoded_value_visitor`, `value_location`, `mapped_file`,
`block_options`, `file_options`).

| name | meaning |
|---|---|
| `walk_values<Ts...>(capture, on_value, rest, on_frame = {}, extra = nullptr)` | per data frame: `on_frame(dict, frame)` (optional; returning false skips the frame), then each record. Literal, scalar and string records go to `rest` as `literal` / `scalar` / `string` events named `"[i]"`. A type record is walked with `visit_record` through an adapter: at the record and at every nested struct value, outermost first, a value whose schema hash is `schema_hash_v<T>` for one of `Ts` is decoded into a `T` and passed to `on_value(field_name, const T&)`, and its interior is not walked. Everything else is forwarded to `rest`. Returns `for_each_frame`'s `frame_walk`, with records that could not be walked added to `problems`, and `typed` / `fallbacks` filled in |
| `walk_values(capture, rest, on_frame = {}, extra = nullptr)` | the same with no compiled types — `walk_values<>` with a never-called `on_value`; every value goes to `rest` (a dictionary-only reader such as `examples/user_example1/reader2.cpp`) |
| `walk_values_in_file<Ts...>(path, on_value, rest, on_frame = {}, block_options = {}, extra = nullptr)` | the same over `for_each_frame_in_file` (block reading) |
| `walk_values<Ts...>(std::string_view path, on_value, rest, on_frame = {}, file_options = {}, extra = nullptr)` | a capture on disk in one call: with `blocks` (the default, A6) or `path == "-"`, `walk_values_in_file` with `block_options{block_bytes, limits}` — windows of `decode_limits::window_bytes` (10 MiB) unless `block_bytes` says otherwise; with `{.blocks = false}`, `mapped_file::open(path, populate)` and the span walk, falling back to the windows for an `unsupported` open (a pipe, a device); any other open error is returned |
| `walk_values(std::string_view path, rest, on_frame = {}, file_options = {}, extra = nullptr)` | the path walk with no compiled types |
| `detail::decode_at<T>(out, where)` | returns `status`. `native` → `get_object` (flat `T`, size-checked, `bool`s repaired); otherwise `serreg::detail::decode_value` into `T` at `image_offset`, charging the walk's byte budget (`where.budget`) under the same slot rules; `invalid_value` for a valueless variant `T` cannot hold, and with the range check (decision D) for a leaf of an enum without a fixed underlying type outside its range (`walk.hpp:48-50`, `67-68`) — a fallback: reported, then walked generically, where the dictionary-driven walker prints the number |
| `detail::typed_adapter<F, Ts...>` | the `decoded_value_visitor` that implements `take_struct` with a fold over `Ts` and forwards all other events to `rest` (incl. `noncanonical_bool` and `begin_map`/`begin_entry`/`end_entry`/`end_map`, `begin_set`/`end_set`, `begin_transparent`/`end_transparent`, `walk.hpp:165-195`); counts `typed` values and `fallbacks`: a matching hash whose typed decode failed. Each fallback (review G2-9) is also a problem — `"frame N: record [i]: field: Type decoded generically (typed decode: invalid_value)"` — and the value is walked generically (a valueless variant appears as `null`) |
| `detail::walk_frame(dict, frame, adapter, problems)` | `inspect(frame, &dict)` and the per-record dispatch described above. A constant of the call arrives from `inspect` as the scalar or enum record it stands for, so it is walked like a run-time value; the `arg_const` case of the switch (`walk.hpp:244`) is never taken |

`on_value` is an overload set, e.g. `overloaded{[](std::string_view, const Order&) {…}, [](std::string_view,
const Fill&) {…}}`. With `Ts` empty it is never called; the overloads without `on_value` pass one
(`[](std::string_view, const auto&) {}`) themselves, so a generic reader writes `walk_values(path, visitor)`.

**The path overloads.**

```cpp
if (!o.blocks && path != "-")
{
    const auto m = mapped_file::open(path, o.populate);
    if (m)
        return walk_values<Ts...>(m->bytes(), on_value, rest, on_frame, extra);
    if (m.error() != error::unsupported)
        return std::unexpected(m.error());
}
return walk_values_in_file<Ts...>(path, on_value, rest, on_frame,
                                  block_options{o.block_bytes, o.limits}, extra);
```

(`walk.hpp:340-349`.) The same decision as `tools::for_each_frame(path)` in `src/reader.cpp`, repeated here because
the typed walk is a template and cannot go through the compiled overload. `blocks` defaults to `true`, so the
first branch is the opt-in: by default a capture of any size is walked in windows with bounded
memory (chapter 4 §4.6a). The mapping lives for the duration of the
walk; `frame_ref::offset` is a file offset in both branches. A path overload and a span overload differ in the
type of the first argument only (`std::string_view` vs `std::span<const std::byte>`), so a string literal selects
the path form.

**Why the match is safe.** The schema hash covers names, member types, offsets, sizes and the
`since`/`deprecated` annotations. An equal hash therefore means the file's type and the compiled `T` have the
same layout, and the decode is exact. A type that changed since the file was written has another hash; it is not
matched and is walked generically, so an old file never becomes an error.

**Containers as `Ts`.** `dynamic_type<T>` admits a container since format 1.6, so
`walk_values<std::vector<int>>(path, on_ids, rest)` is valid: a record of the transparent type `std::vector<i32>`
has that schema hash, `take_struct` runs before the walker's transparent branch and claims the whole record, and
`decode_at` decodes the image — the slot at 0 plus its tail — straight into the vector. A map or set *member* of a
typed `T` is decoded by the same `decode_value` as `deserialize` (a duplicate key is `invalid_value`, hence a
fallback); inside a generic walk it reaches `rest` through the eight new events.

**Why `Ts...` rather than "the compiled registry".** The registry knows at run time which hashes this program
has, but calling user code with a decoded object needs the C++ type, which only a template argument provides.
A reader that wants "every type I know" lists them once, for example in a `using known = …` pack at the call site.

**Cost.** One virtual `take_struct` call per struct value plus one hash comparison per `T`. Typed values are
decoded into a stack `T` (dynamic `T`s allocate their strings and vectors); generic values are only walked.

Tests: `test_dynamic.cpp` "2.0.6 walk_values delivers the outermost compiled type and walks the rest", and "G2-6 a
valueless variant: std::monostate when the variant has one, else invalid_value" (a fallback, reported). The first covers
the whole record typed; vector elements, an optional, a variant alternative, a flat base inside a dynamic class,
and a flat record (the native path) typed; and no types at all. There are also the `walk_values_in_file` case
and the `capture` fuzz target's `walk_values<>` pass. `test_readers.cpp` "2.1.10 for_each_frame(path) and
walk_values(path): mapped and in blocks agree" counts the values of `walk_values(path, visitor)` mapped, in blocks
and over a span (all equal) and checks `walk_values<corpus::DynMsg>(path, …)` delivers 50 typed values. The two
path overloads must stay in step with `for_each_frame(path)`: the same strategy, the same fallback on
`unsupported`. Examples: `examples/user_example1/reader2.cpp` (`walk_values(path, out_, on_frame, opts_.file)`)
and `reader3.cpp` (`walk_values<demo::Order>(path, …, opts_.file)`).
