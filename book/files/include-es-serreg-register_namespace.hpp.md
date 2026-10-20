# `include/es/serreg/register_namespace.hpp`

Registration of every serialisable type of a **namespace** before `main()` (DECISIONS A-22). `serialize<T>()`
registers `T` by instantiation, so the first call site anywhere in the program is the registration; a program
that only reads, a schema publisher, or a plugin whose types are only received has no call site, and
`register_types<Ts...>()` (`registry.hpp`, A1) makes the user list the types by hand. This header lists them for
the user: it walks a namespace with `std::meta::members_of`, keeps the complete class and enum types (nested
namespaces included), applies a selector, drops what the library cannot serialise, and odr-uses the same
`registrar<T>::g` guards `serialize()` would. Part of the **registry layer**; reflection-dependent (it is the only
header besides `reflect.hpp` that enumerates with `members_of` — on namespaces, not classes).

## Depends on / used by

Includes: `es/serreg/hash.hpp` (`fixed_string` for the pattern NTTPs), `es/serreg/reflect.hpp` (the type
predicates, `has_annotation`, `skip_t`, `members<T>()`), `es/serreg/registry.hpp` (`detail::touch<T>`),
`<meta>`, `<span>`, `<string_view>`, `<vector>`.

Included by: nothing in the library (opt-in, like `log.hpp`); `examples/register_namespace.cpp`,
`tests/unit/test_register_namespace.cpp`.

## What it defines

| name | kind | meaning |
|---|---|---|
| `message_t`, `message` | annotation tag | `struct [[=es::serreg::message]] Order {…};` — the opt-in mark the `annotated<message_t>` selector looks for |
| `detail::supported_type<T>()` | consteval predicate | `check_supported()` as a `bool`: true when `serialize<T>()` would compile (`register_namespace.hpp:55-113`); mirrors its branches — scalars, enums, arrays, the dyn members (no `vector<bool>`, ordered or orderable keys), tuple-likes, no pointers/references/unions/polymorphic types/smart pointers/`string_view`, std types only from the vocabulary, no recursive types, and for a user class every base and every non-skipped member supported |
| `detail::glob_match(pat, s)` | consteval | `*` any run, `?` one character, iterative with backtracking to the last `*` (`:115-143`) |
| `detail::collect_types(ns, recurse, out)` | consteval | the complete class and enum types of a namespace in declaration order; recurses into nested namespaces; skips templates, functions, variables, aliases, incomplete types and `[[=skip]]` types (`:145-162`) |
| `select_all`, `name_prefix<P>`, `name_suffix<S>`, `name_glob<G>`, `qualified_glob<G>`, `annotated<A>`, `not_<S>`, `all_of<S...>`, `any_of<S...>` | selectors | consteval callables `info → bool`; the name selectors match the unqualified identifier, `qualified_glob` the canonical name (`display_string_of`), `annotated<A>` any annotation type (`:169-233`) |
| `selected_types<NS, Select, Recurse>()` | consteval | the selection as a `span<const std::meta::info>` — serialisable or not (`:236`) |
| `selected_type_names<NS, Select, Recurse>()` | consteval | the same as NUL-terminated canonical names (`const char*`: a `string_view` is not a structural type), for a `static_assert` or a dump (`:246`) |
| `namespace_registrar<NS, Select, Recurse>` | class template | `touch_all()` expands `template for` over the selection and calls `detail::touch<T>()` for every supported type — the instantiation is the registration, the call does nothing; `count()`; `g`, the odr-use anchor (`:256-279`) |
| `register_namespace<NS, Select = select_all{}, Recurse = true>` | variable template | `namespace_registrar<…>::g`; taking its address in one translation unit registers the namespace (`:282`) |
| `registered_type_count<NS, Select, Recurse>` | variable template | how many types the registration covers, for a `static_assert` beside it (`:285`) |
| `ES_SERREG_REGISTER_NAMESPACE(ns[, selector])` | macro | an anonymous-namespace reference to `register_namespace<^^ns, selector>` (`:294`); `ns` is the namespace *name*, the macro adds `^^` |

## How it works

`members_of(^^ns, access_context::unchecked())` returns the members declared **before the point of
instantiation, in this translation unit** — there is no program-wide "all instantiations" query; this is a
declaration walk, so the registration line goes after the headers that declare the types, once per namespace
per program. `selected_types` is `define_static_array` of the filtered list, which makes it usable in a
`template for` with a `constexpr` loop variable; inside the loop `if constexpr (supported_type<[:t:]>())` keeps a
`struct Helper { void* p; }` out silently (listed by the selection, skipped by the registration) instead of
tripping `check_supported`'s `static_assert`. `touch<T>()` then does exactly what a `serialize(out, T{})` would:
odr-use `registrar<T>::g` and, recursively, the guards of every component type, so `v2::MsgOrder`'s `std::string`
and the enum `Side` register with their owners. Because the whole thing is an instantiation side effect, `g`'s
initialiser `(touch_all(), 0)` runs nothing of consequence; the guards' own constructors do the pushes during
the image's static initialisation, as for any other type.

Cost: each selected type costs what its first `serialize()` would — descriptor, blob, hash machinery at compile
time, about 250 B of `es_serreg_dict` per type per translation unit. Patterns are a convention the compiler cannot
check (`Msg*` matches a `MsgBuilder` and misses a renamed type); the annotation is the exact form.

Tests: `tests/unit/test_register_namespace.cpp` — the walk (7 of 8 declared types: the `[[=skip]]` one, the
template, the alias and the incomplete type are out; recursion on and off), every selector and the glob
itself, `supported_type` on a pointer and a polymorphic type, `registered_type_count`, and the registry before
`main()` holding the five types (and not a type of an unregistered namespace), also through `export_image()`.
Example: `examples/register_namespace.cpp` (ctest `example_register_namespace`).
