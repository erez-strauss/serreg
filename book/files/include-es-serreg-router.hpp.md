# `include/es/serreg/router.hpp`

`es::serreg::frame_router`: one handler per call signature, chosen by the frame's call hash with
one hash-table lookup. A handler's parameter list names the signature it receives (`on(h)` is the frame
`serialize(sink, P1, P2, …)` writes), the frame is decoded by the same typed reader as `deserialize`, and the
handler is called with the decoded values. The router is used per frame (`route(frame)`, e.g. from an `es::ipc`
consumer) or as a `tools::frame_callback` for `tools::for_each_frame` / `tools::walk_values` (`callback()`). It
replaces a chain of trial `deserialize` calls, each of which compares the call hash once, by one lookup. It is
header-only, needs reflection (it includes `deserialize.hpp`), and is included by the umbrella `serreg.hpp`.

## Depends on / used by

Includes: `<algorithm>`, `<array>`, `<cstddef>`, `<cstdint>`, `<functional>` (`std::function` for the table
entries and the fallback handlers), `<span>`, `<string_view>`, `<tuple>`, `<type_traits>`, `<unordered_map>`,
`<utility>`; `deserialize.hpp`
(`detail::deserialize_frame`, `read_frame_header`, `result`, `status`), `print.hpp` (the print forms; `print_tag` and `println_tag` themselves are in `detail/blob_bytes.hpp`, reached through `blob.hpp`),
`tools.hpp` (`tools::frame_ref`, `tools::frame_callback`, `tools::log_record`, `tools::render_frame`,
`tools::render_log`, `dyn::dictionary`). Through them: `call_hash_v`
(`blob.hpp`), `is_literal_v`, `lit<S>`, `fixed_string` (`literal.hpp`), `is_form_tag_v` (`detail/blob_bytes.hpp`).

Used by: `serreg.hpp` (umbrella), `examples/user_example1/reader1.cpp` (`ue1::typed_reader` builds its
router in the constructor and walks with `tools::for_each_frame(path, router_.callback(), opts_.file)`), and
`tests/unit/test_readers.cpp` ("2.1.10 frame_router routes each frame to the handler of its call signature").

## Structure

- **Header comment** (`router.hpp:1-21`): a usage example and the rules: how a handler names its signature, one
  lookup per frame, storage reuse, `otherwise` / `on_error`, not thread-safe; also (T-10): registering a
  signature again replaces its handler, the handler's return value is ignored, the router is neither copyable nor
  movable.
- **`detail::handler_params<F>`**, **`handler_params_t<F>`** (`router.hpp:47-86`): the parameter types of a
  handler, `cv`/`ref` removed, as a `std::tuple`: specialisations for a function pointer and for member function
  pointers (`const`, `noexcept`, both); anything else goes through `decltype(&F::operator())`, so a non-generic
  lambda or a function object with one `operator()` works.
- **`detail::sig_records_t<Sig...>`**, **`sig_values_t<Sig...>`** (`router.hpp:91-98`): the records of a signature
  (the form tag removed) and the values among them (literals and constants removed too), as tuples.
- **`detail::value_position<I, Records>()`** (`router.hpp:100-110`): how many records that are neither literals nor
  constants come before record `I`, i.e. its index in the values tuple.
- **`detail::record_output<I, Records>(values)`** (`router.hpp:111-119`): the output argument of record `I` for the
  typed reader: `R{}` for a literal or a constant, a reference to the value's tuple slot otherwise.
- **`detail::prepend_literal<Tuple>::with<L>`** (`router.hpp:121-125`): `tuple<L, P...>` from `tuple<P...>`, for
  `on(lit, h)`.
- **`class frame_router`** (`router.hpp:128-395`):
  - deleted copy constructor and copy assignment (`router.hpp:133-134`);
  - `frame_handler`, `error_handler` (`router.hpp:136-137`): `std::function<void(const dyn::dictionary&, const
    tools::frame_ref&)>` and `std::function<void(const tools::frame_ref&, error)>`;
  - `on<Sig...>(h)` (`router.hpp:141-161`, requires `sizeof...(Sig) > 0`): the one function that fills the table;
  - `on(h)` (`router.hpp:163-181`), `on(lit<S>, h)` (`router.hpp:183-199`), `on_print(lit<F>, h)` /
    `on_println(lit<F>, h)` (`router.hpp:201-208`): signatures taken from `handler_params_t<H>`;
  - `log_handler` and `on_log(log_handler)` (`router.hpp:213-218`): every log frame without an exact handler,
    rendered as one line — `h(level, line)`;
  - `log_record_handler`, `on_log(std::uint8_t min_level, log_record_handler)` and the enum overload
    `on_log(L min_level, h)` (`router.hpp:225-237`): log frames split by level floor, delivered as a
    `tools::log_record`;
  - `otherwise(frame_handler)` (`router.hpp:238-242`), `on_error(error_handler)` (`router.hpp:244-248`);
  - `route(frame)` → `result<bool>` (`router.hpp:254-283`), which counts too;
  - `operator()(const dyn::dictionary&, const tools::frame_ref&)` → `bool` (`router.hpp:285-335`), the
    `frame_callback` form, which counts and holds the log dispatch;
  - `callback()` (`router.hpp:337-341`): a `tools::frame_callback` that calls `*this`;
  - `handled()`, `unhandled()`, `failed()`, `size()` (`router.hpp:343-358`);
  - private `on_tuple` and `on_form` (`router.hpp:361-380`): unpack a parameter tuple into `on<P...>`;
  - members (`router.hpp:382-394`): `table_` (`std::unordered_map<std::uint64_t,
    std::function<status(std::span<const std::byte>)>>`), `otherwise_`, `on_log_`, `by_level_`
    (`std::array<log_record_handler, 6>`, one slot per level floor) with the private helper `lowest_floor()`
    (`router.hpp:386-392`, the lowest registered floor or 6), `on_error_`, and the three counters.

## How it works

### A handler's parameters are the signature

```cpp
template<class F, class = void> struct handler_params
{
};
template<class F>
struct handler_params<F, std::void_t<decltype(&F::operator())>>
    : handler_params<decltype(&F::operator())>
{
};
template<class R, class... A> struct handler_params<R (*)(A...), void>
{
    using type = std::tuple<std::remove_cvref_t<A>...>;
};
```

(`router.hpp:44-56`.) A lambda `[&](const demo::Order& o)` has the parameter tuple `std::tuple<demo::Order>`, so
`on(h)` registers the signature `<demo::Order>`, the one `serialize(sink, order)` writes. The member-pointer and
function-type specialisations forward to the function-pointer one, so `const` and `noexcept` call operators give the
same tuple. A generic lambda (`auto` parameters) or an overloaded function object has no single `&F::operator()`:
the primary template then has no `type` (T-10), the concept
`fixed_signature_handler` is false, and `on(h)`, `on(lit, h)`, `on_print` and `on_println` stop with "the handler has
no single call signature … on<Sig...>(h)" (`negative_neg_router_generic_lambda`) instead of an error inside the
traits. `on(lit<S>, h)` puts the literal first (`prepend_literal`), and `on_print(fmt,
h)` / `on_println(fmt, h)` register `<print_tag, lit<F>, P...>` / `<println_tag, lit<F>, P...>` (`on_form`), the
signatures of `print` / `println`. All four end in `on<Sig...>`.

### One table entry per signature

```cpp
template<class... Sig, class H>
    requires(sizeof...(Sig) > 0)
frame_router& on(H&& h)
{
    using records = detail::sig_records_t<Sig...>;
    using values = detail::sig_values_t<Sig...>;
    table_.insert_or_assign(
        call_hash_v<Sig...>,
        [h = std::forward<H>(h),
         v = values{}](const std::span<const std::byte> b) mutable -> status {
```

(`router.hpp:137-146`.) The key is `call_hash_v<Sig...>`, a compile-time constant: the same hash the writer put
in the frame header. The value is a closure that owns the handler and a default-constructed `values` tuple — for
`on_println("  fill #{} of order {}: {} @ {:.2f}"_, h)` with `h(std::uint64_t, std::uint64_t, std::int32_t,
double)` that is `std::tuple<std::uint64_t, std::uint64_t, std::int32_t, double>`; the format literal and the tag
have no slot. `insert_or_assign` means a second handler for the same signature replaces the first (documented in
the header comment, T-10; a `std::string` and a `std::string_view` parameter are one
signature, since both decode a string); `size()` is the number of distinct signatures. The closure calls the
handler and ignores its return value: a handler that returns `bool` cannot stop a walk. Instantiating `on<Sig...>` instantiates `deserialize_frame<Sig...>`, which (as for
any `deserialize`) registers the signature and its types in the reading program's own registry.

### Decoding into the kept tuple

```cpp
template<std::size_t I, class Records, class Values> decltype(auto) record_output(Values& v)
{
    using R = std::tuple_element_t<I, Records>;
    if constexpr (is_literal_v<R> || is_constant_v<R>)
        return R{};
    else
        return (std::get<value_position<I, Records>()>(v));
}
```

(`router.hpp:109-116`.) The closure calls `detail::deserialize_frame<Sig...>(b, record_output<I, records>(v)...)`
over `I = 0 … tuple_size_v<records> - 1`: a literal record gets a `lit{}` value (its id is checked, nothing is
stored), a value record a non-const lvalue reference to its slot, whose index `value_position` computes by counting
the non-literal records before it. So `on<corpus::Cancel, decltype("why"_lit)>(h)` decodes into `get<0>(v)` and
checks the trailing literal.

**Constants.** A `constant<V>` in the signature is treated like a literal: it has no slot in the values
tuple, `record_output` passes `R{}` to the typed reader, and the handler does not receive it. The signature must
be spelled with `on<Sig...>`, because a handler's parameter list cannot name a constant:
`router.on<decltype("fill"_lit), constant<42>, demo::Order>([](const demo::Order& o) { … })` handles the frames of
`serialize(out, "fill"_, 42_, order)`, and frames written with `43_` have another call hash, so this handler does not see them.

`deserialize_frame` validates the header, the call hash (compact) or every record
hash and length (full) before writing an output; only when it succeeds does the closure run `std::apply(h,
std::as_const(v))`. `deserialize_frame` returns a `decode_error`; the router keeps its `error` code
only (`on_error` and `route` report a plain `error`). The handler therefore receives `const` lvalues: parameters by value or by `const&` work, a
non-const reference parameter does not compile.

The tuple lives as long as the router, so a `std::string`, `std::vector` or `std::optional` member decoded for
one frame keeps its capacity for the next frame of that signature: after the first frames, decoding allocates only
when a value grows. A handler that wants to keep a value copies it (`msgs.push_back(m)` in the test). After a
failed decode the tuple may hold part of that frame; the handler is not called and the next frame overwrites it.

### `route` and the callback form

```cpp
bool operator()(const dyn::dictionary& d, const tools::frame_ref& f)
{
    const auto it = table_.find(f.call_hash);
    if (it == table_.end())
    {
        if (f.call && f.call->is_log() && !f.call->args.empty())
        { // 2.2.1 (1.7): a log frame — the level is a constant of the call entry
            const auto lvl = static_cast<std::uint8_t>(f.call->args[0].value);
            // the structured handler with the highest floor at or below this level, if any
            for (int i = std::min<int>(lvl, 5); i >= 0; --i)
                if (by_level_[static_cast<std::size_t>(i)])
                {
                    if (const auto rec = tools::render_log(f.bytes, d))
                    {
                        by_level_[static_cast<std::size_t>(i)](*rec);
                        ++handled_;
                        return true;
                    }
                    break; // not renderable: fall through to the line handler / otherwise
                }
            if (on_log_)
                if (const auto line = tools::render_frame(f.bytes, d))
                {
                    std::string_view m = *line;
                    if (m.ends_with('\n'))
                        m.remove_suffix(1);
                    on_log_(lvl, m);
                    ++handled_;
                    return true;
                }
            if (!on_log_ && lowest_floor() <= 5 && lvl < lowest_floor())
            { // below every registered floor: dropped on purpose, counted as handled, never rendered
                ++handled_;
                return true;
            }
        }
        ++unhandled_;
        if (otherwise_)
            otherwise_(d, f);
        return true;
    }
```

(`router.hpp:285-325`.) A log frame (`call::is_log()`) that has no handler of its own is dispatched by its
level, which is the first constant of the call's entry (no decoding is needed to know it). The order is:

1. **A level floor** (`on_log(min_level, h)`): among the structured handlers registered in `by_level_`, the one
   with the highest floor at or below the frame's level wins (`trace` = 0 … `fatal` = 5; `on_log(min_level, h)`
   accepts the `std::uint8_t` or any `std::uint8_t`-based enum such as `es::serreg::level`). It receives a
   `tools::log_record` (`tools::render_log`: the envelope fields — level, file, line, mask, time, thread, sequence
   — the format literal's text and the message rendered *without* the envelope prefix). So `on_log(level::warn,
   alert)` plus `on_log(level::trace, keep)` sends `warn`/`error`/`fatal` to `alert` and everything below to
   `keep`; a frame goes to exactly one handler. Up to six floors, one per level; registering a floor again replaces
   its handler.
2. **The line handler** (`on_log(h)`): when no floor claims the frame — or `render_log` failed — and a line
   handler is set, it receives the level and `tools::render_frame` of the frame, the same text `srdump --level`
   prints (`<time> <LEVEL> <file>:<line> [tid] #seq message`), without the trailing newline. A plain `on_log(h)`
   is thus the floor `trace` for lines, and loses to a structured handler whose floor the frame reaches.
3. **Below every floor:** with floors registered but no line handler, a frame whose level is below the lowest
   floor (`lowest_floor()`) is dropped on purpose — counted in `handled()` without being rendered (a `debug` frame
   costs nothing when only `on_log(level::info, …)` is registered).
4. Otherwise the frame goes to `otherwise`, like any frame without a handler.

In every case a handler registered for a log call's exact signature (`on<Sig...>`) still takes precedence: the
table lookup comes first. As a walk callback, a frame whose call hash has no entry is counted in `unhandled()` and
passed to `otherwise` with the capture's dictionary (enough for `tools::render_frame` or `tools::visit_record`); a
frame whose entry fails to decode is counted in `failed()` and passed to `on_error` with the error; a decoded frame
is counted in `handled()`. The callback always returns `true`: the router never stops a walk. The walk only
delivers data frames, so meta frames (dictionary, checkpoint, writer info) never reach the router. `callback()` wraps `(*this)(d, f)` in a
`tools::frame_callback` that captures `this`; the router must outlive the walk. For that reason the router is
neither copyable nor movable (T-10): the copy constructor and copy assignment are
deleted, so no move operations are declared either, and a `callback()` taken earlier cannot be left pointing at a
moved-from object.

`route(frame)` is the same lookup for one raw frame, without the dictionary:

```cpp
result<bool> route(const std::span<const std::byte> frame)
{
    const auto h = read_frame_header(frame);
    if (!h)
    {
        ++failed_;
        return std::unexpected(h.error());
    }
    const auto it = table_.find(h->call_hash);
    if (it == table_.end())
    {
        ++unhandled_;
        return false;
    }
    if (const auto s = it->second(frame); !s)
    {
        ++failed_;
        if (on_error_)
            {
            tools::frame_ref f;
            f.bytes = frame;
            f.call_hash = h->call_hash;
            f.n_records = h->n_records;
            on_error_(f, s.error());
        }
        return std::unexpected(s.error());
    }
    ++handled_;
    return true;
}
```
(`router.hpp:254-283`.) It reads the header with `read_frame_header` (an error for a short or malformed header),
returns `false` when no handler is registered for the call hash, the decode error when the entry fails, and `true`
when a handler ran. It counts like the walk callback (T-10): a malformed header or a
failed decode in `failed()`, a frame without a handler in `unhandled()`, a decoded frame in `handled()`; and a decode
failure is passed to `on_error` with a `tools::frame_ref` built from the raw frame (`bytes`, `call_hash`, `n_records`;
`index`, `offset`, `full_records` and `call` keep their defaults). A malformed header is counted but not passed to
`on_error`. `otherwise` is never called by `route()`: it takes the capture's dictionary, which a raw frame does not
carry, so a frame without a handler is only counted. It suits a ring consumer (`es::ipc::consumer::poll`), which has frames but no dictionary.

## Maintaining it

- **Tests:** `tests/unit/test_readers.cpp`
  "2.1.10 frame_router routes each frame to the handler of its call signature": `on(h)`, `on(lit, h)`, `on<Sig...>`
  with a trailing literal, `on_println`, a dynamic type decoded twice through the same slot (`corpus::DynMsg`, the
  second frame with an empty vector), a frame without a handler going to `otherwise`, the counters, `route()` on raw
  frames (a handled frame, a corrupted dynamic image returning an error without calling the handler, a frame without a
  handler returning `false`), and `on_error` during a walk. `test_readers.cpp`
  "2.1.12 T-10 frame_router::route counts like the walk callback and reports decode errors": `route()` on a handled
  frame, a frame without a handler (counted, nothing called) and a cut-short record (counted in `failed()`, `on_error`
  called with the frame's call hash). `tests/unit/test_log.cpp` "A log: the envelope by policy … renders in srdump"
  covers the log dispatch: `on_log(h)` receiving every log frame rendered, `on_log(level::warn, …)` +
  `on_log(level::error, …)` splitting by floor (the `info` frame below both floors is dropped and counted in
  `handled()`, `unhandled()` stays 0), a line handler beside a floor (the `info` frame goes to the line handler,
  `warn`/`error` to the floor) and `tools::render_log` on one frame (`level`, `message`, `format`). The negative
  test `negative_neg_router_generic_lambda` checks the diagnostic for
  a generic lambda. `examples/user_example1` (ctests `user_example1_reader1`, `user_example1_reader1_blocks`) uses it
  on a real capture.
- **The log dispatch order is a contract.** Exact signature (`table_`) first; then the highest floor ≤ level
  (`by_level_`); then the line handler; then the silent drop below the lowest floor (only when no line handler is
  set); then `otherwise`. The drop is counted in `handled()` on purpose — a reader that filters by level should
  not see those frames as unhandled. `render_log` and `render_frame` need the capture's dictionary, so neither
  path runs from `route()` (raw frames): log frames there behave like any frame without an exact handler.
- **Handler parameters must be exactly the serialized types.** The call hash covers every argument's type, so a
  handler taking `std::int64_t` for a value written as `std::int32_t`, `float` for `double`, or `unsigned` for
  `std::uint64_t` names another signature: the frame is not decoded wrongly, it goes to `otherwise` and counts as
  unhandled. The same holds for the literals, their order, and plain vs `print` / `println`. A string argument may
  be received as `std::string` or `std::string_view` (the call hash records the category); a `string_view` points
  into the frame and is valid only during the handler.
- **A handler without parameters has no signature for `on(h)`**, and `on(h)` stops the build with a
  `static_assert` saying so (without it, `on<>(h)` would fail the `sizeof...(Sig) > 0` constraint and overload
  resolution would pick `on(h)` again, recursing at run time). Register a frame of literals only with
  `on(lit, h)` or `on<Sig...>(h)`, e.g. `on("eod"_, [&] { … })`.
- **Keep the router in place.** `callback()` captures `this`; do not make the class copyable or movable again, and
  keep a router that feeds a walk alive until the walk ends.
- **Not thread-safe.** The table, the kept value tuples and the counters are mutated by every call; `route` and the
  callback must not run concurrently on one router. Use one router per reading thread (each has its own storage).
  Setting up (`on`, `otherwise`, `on_error`) while frames are routed is not allowed either.
- **Storage reuse is part of the design.** Do not construct the tuple per frame: the kept tuple is what makes the
  steady state allocation-free for dynamic types. Handlers see each frame's values, never the previous frame's,
  because `deserialize_frame` writes every output (resizing vectors, resetting optionals) before the handler runs.
- **Allocation happens at setup and in decoded values only.** `on` allocates a table node and the `std::function`
  (for a large closure); `route` and the callback do one `unordered_map::find`, one indirect call and the decode,
  which allocates only when a dynamic value needs more capacity than it had. The `otherwise` and `on_error`
  handlers are the caller's.
- **The lookup must stay the decision.** A handler is chosen by `frame_ref::call_hash` / the header's call hash
  only; the typed reader then validates the frame again (the call hash of a compact frame, the record hashes of a
  full one). Keep both: the lookup picks, the reader proves.
