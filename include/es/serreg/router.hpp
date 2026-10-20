// es/serreg/router.hpp — frame_router (2.1.10): one handler per call signature, chosen by the frame's call hash.
//
//   es::serreg::frame_router router;
//   router.on([&](const demo::Book& b) { ... })                          // serialize(sink, book)
//         .on("order"_, [&](const demo::Order& o) { ... })               // serialize(sink, "order"_, order)
//         .on_println("fill #{} of order {}: {} @ {:.2f}"_,               // println(sink, fmt, …)
//                     [&](std::uint64_t exec, std::uint64_t order, std::int32_t qty, double px) { ... })
//         .otherwise([&](const dyn::dictionary& d, const tools::frame_ref& f) { ... });
//   tools::for_each_frame("capture.ser", router.callback());             // or router.route(frame) per frame
//
// A handler's parameters name the signature: on(h) decodes the frame as serialize(sink, P1, P2, …) did, where Pi
// are h's parameter types without cv/ref; on(lit, h) puts a literal first; on_print / on_println add the print
// form and its format literal; on<Sig...>(h) spells any signature, literals anywhere (lit<"x">), and h receives
// the non-literal values in order. The call hash of each signature is a compile-time constant, so a frame finds
// its handler with one hash-table lookup (an unordered_map filled when the router is set up; nothing allocates
// per frame except what the decoded values themselves need — their storage is kept and reused between frames).
// The frame is decoded with the same typed reader as deserialize (validation first, then the outputs), and the
// handler runs only when that succeeds; a frame whose call hash has no handler goes to `otherwise`, a frame that
// fails to decode to `on_error`. Not thread-safe: one router per reading thread. Registering a signature again
// replaces its handler (a std::string and a std::string_view parameter are one signature: both decode a string);
// the handler's return value is ignored. The router is neither copyable nor movable (callback() points at it).
#pragma once

#include <es/serreg/deserialize.hpp>
#include <es/serreg/print.hpp>
#include <es/serreg/tools.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace es::serreg
{
namespace detail
{
// The parameter types of a handler (a non-generic lambda, a function object with one operator(), or a function).
// A handler without one fixed signature (a generic lambda, an overloaded function object) has no `type`; the
// router then says so instead of failing inside this template (2.1.12, review T-10).
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
template<class R, class... A>
struct handler_params<R (*)(A...) noexcept, void> : handler_params<R (*)(A...)>
{
};
template<class R, class... A> struct handler_params<R(A...), void> : handler_params<R (*)(A...)>
{
};
template<class R, class... A>
struct handler_params<R(A...) noexcept, void> : handler_params<R (*)(A...)>
{
};
template<class R, class C, class... A>
struct handler_params<R (C::*)(A...), void> : handler_params<R (*)(A...)>
{
};
template<class R, class C, class... A>
struct handler_params<R (C::*)(A...) const, void> : handler_params<R (*)(A...)>
{
};
template<class R, class C, class... A>
struct handler_params<R (C::*)(A...) noexcept, void> : handler_params<R (*)(A...)>
{
};
template<class R, class C, class... A>
struct handler_params<R (C::*)(A...) const noexcept, void> : handler_params<R (*)(A...)>
{
};
template<class F> using handler_params_t = typename handler_params<std::remove_cvref_t<F>>::type;
template<class F>
concept fixed_signature_handler =
    requires { typename handler_params<std::remove_cvref_t<F>>::type; };

// The records of a signature (the form tag is not a record) and the values among them (literals and constants carry none).
template<class... Sig>
using sig_records_t = decltype(std::tuple_cat(
    std::declval<std::conditional_t<is_form_tag_v<Sig>, std::tuple<>, std::tuple<Sig>>>()...));
template<class... Sig>
using sig_values_t = decltype(std::tuple_cat(
    std::declval<std::conditional_t<is_form_tag_v<Sig> || is_literal_v<Sig> || is_constant_v<Sig>,
                                    std::tuple<>, std::tuple<Sig>>>()...));

// Position of record I among the values of Records (the number of value records before it).
template<std::size_t I, class Records> consteval std::size_t value_position()
{
    return []<std::size_t... J>(std::index_sequence<J...>) {
        return (std::size_t{0} + ... +
                (is_literal_v<std::tuple_element_t<J, Records>> ||
                         is_constant_v<std::tuple_element_t<J, Records>>
                     ? 0u
                     : 1u));
    }(std::make_index_sequence<I>{});
}
// The output argument of record I for the typed reader: the literal itself, or the value's storage.
template<std::size_t I, class Records, class Values> decltype(auto) record_output(Values& v)
{
    using R = std::tuple_element_t<I, Records>;
    if constexpr (is_literal_v<R> || is_constant_v<R>)
        return R{};
    else
        return (std::get<value_position<I, Records>()>(v));
}

template<class Tuple> struct prepend_literal;
template<class... P> struct prepend_literal<std::tuple<P...>>
{
    template<class L> using with = std::tuple<L, P...>;
};
} // namespace detail

class frame_router
{
public:
    frame_router() = default;
    // 2.1.12 (review T-10): callback() hands out a pointer to this router, so it stays where it was built.
    frame_router(const frame_router&) = delete;
    frame_router& operator=(const frame_router&) = delete;

    using frame_handler = std::function<void(const dyn::dictionary&, const tools::frame_ref&)>;
    using error_handler = std::function<void(const tools::frame_ref&, error)>;

    // Any signature: literals as lit<"…">, a leading print_tag / println_tag for the print forms. h is called
    // with the signature's non-literal values, in order.
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
                const auto r = [&]<std::size_t... I>(std::index_sequence<I...>) {
                    return detail::deserialize_frame<Sig...>(
                        b, detail::record_output<I, records>(v)...);
                }(std::make_index_sequence<std::tuple_size_v<records>>{});
                if (!r)
                    return std::unexpected(r.error());
                std::apply(h, std::as_const(v));
                return {};
            });
        return *this;
    }
    // The signature is h's parameter list: serialize(sink, a, b, …).
    template<class H> frame_router& on(H&& h)
    {
        if constexpr (!detail::fixed_signature_handler<H>)
        {
            static_assert(detail::fixed_signature_handler<H>,
                          "frame_router: the handler has no single call signature (a generic "
                          "lambda or an overloaded "
                          "function object); name the signature instead: on<Sig...>(h)");
            return *this;
        }
        else
        {
            static_assert(
                std::tuple_size_v<detail::handler_params_t<H>> > 0,
                "frame_router::on(h): the handler's parameters are the signature, so it needs at "
                "least one; a frame of literals only is on<lit<\"...\">>(h)");
            return on_tuple(std::type_identity<detail::handler_params_t<H>>{}, std::forward<H>(h));
        }
    }
    // A literal, then h's parameters: serialize(sink, "order"_, order).
    template<fixed_string S, class H> frame_router& on(lit<S>, H&& h)
    {
        if constexpr (!detail::fixed_signature_handler<H>)
        {
            static_assert(detail::fixed_signature_handler<H>,
                          "frame_router: the handler has no single call signature (a generic "
                          "lambda or an overloaded "
                          "function object); name the signature instead: on<Sig...>(h)");
            return *this;
        }
        else
        {
            using sig = typename detail::prepend_literal<
                detail::handler_params_t<H>>::template with<lit<S>>;
            return on_tuple(std::type_identity<sig>{}, std::forward<H>(h));
        }
    }
    // print / println: the format literal, then h's parameters (the arguments).
    template<fixed_string F, class H> frame_router& on_print(lit<F>, H&& h)
    {
        return on_form<print_tag, F>(std::forward<H>(h));
    }
    template<fixed_string F, class H> frame_router& on_println(lit<F>, H&& h)
    {
        return on_form<println_tag, F>(std::forward<H>(h));
    }
    // Frames whose call hash has no handler (with the capture's dictionary, e.g. for tools::render_frame).
    // 2.2.1 (1.7): every log frame (any level, any call site, any format) rendered through the dictionary:
    // h(level, rendered line) — the line as `srdump --level` prints it, without the trailing newline. Log frames
    // with a handler registered for their exact signature (on_println-style) still go to that handler first.
    using log_handler = std::function<void(std::uint8_t level, std::string_view line)>;
    frame_router& on_log(log_handler h)
    {
        on_log_ = std::move(h);
        return *this;
    }
    // Log frames split by level: `on_log(min_level, h)` takes every log frame whose level is at or above
    // `min_level` and below the next registered floor, as a tools::log_record (envelope fields + the message
    // without the prefix). The level is a constant of the call entry, so frames below the lowest registered floor
    // are skipped without rendering; a frame goes to exactly one handler — the one with the highest floor ≤ its
    // level. A plain on_log(h) (above) is the floor `trace` for lines, and loses to a structured handler whose
    // floor the frame reaches. Up to six floors (one per level).
    using log_record_handler = std::function<void(const tools::log_record&)>;
    frame_router& on_log(const std::uint8_t min_level, log_record_handler h)
    {
        if (min_level < 6)
            by_level_[min_level] = std::move(h);
        return *this;
    }
    template<class L>
        requires std::is_enum_v<L> && std::is_same_v<std::underlying_type_t<L>, std::uint8_t>
    frame_router& on_log(const L min_level, log_record_handler h)
    {
        return on_log(static_cast<std::uint8_t>(min_level), std::move(h));
    }
    frame_router& otherwise(frame_handler f)
    {
        otherwise_ = std::move(f);
        return *this;
    }
    // Frames whose call hash has a handler but whose bytes do not decode as that signature.
    frame_router& on_error(error_handler f)
    {
        on_error_ = std::move(f);
        return *this;
    }

    // One raw frame (header included), e.g. from an es::ipc ring: true when a handler ran, false when none is
    // registered for its call hash; the decode error otherwise. Since 2.1.12 (review T-10) it counts like the
    // walk callback (handled / unhandled / failed) and reports a decode failure to `on_error` too; `otherwise`
    // needs a dictionary, which a raw frame does not carry, so a frame without a handler is only counted.
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
    // A tools::frame_callback: routes the frame, counts it, and keeps the walk going.
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
        if (const auto s = it->second(f.bytes); !s)
        {
            ++failed_;
            if (on_error_)
                on_error_(f, s.error());
            return true;
        }
        ++handled_;
        return true;
    }
    // For tools::for_each_frame / walk_values: calls this router (which must outlive the walk).
    tools::frame_callback callback()
    {
        return
            [this](const dyn::dictionary& d, const tools::frame_ref& f) { return (*this)(d, f); };
    }

    std::uint64_t handled() const noexcept
    {
        return handled_;
    }
    std::uint64_t unhandled() const noexcept
    {
        return unhandled_;
    }
    std::uint64_t failed() const noexcept
    {
        return failed_;
    }
    std::size_t size() const noexcept
    {
        return table_.size();
    }

private:
    template<class... P, class H>
    frame_router& on_tuple(std::type_identity<std::tuple<P...>>, H&& h)
    {
        return on<P...>(std::forward<H>(h));
    }
    template<class Tag, fixed_string F, class H> frame_router& on_form(H&& h)
    {
        if constexpr (!detail::fixed_signature_handler<H>)
        {
            static_assert(detail::fixed_signature_handler<H>,
                          "frame_router: the handler has no single call signature (a generic "
                          "lambda or an overloaded "
                          "function object); name the signature instead: on<Sig...>(h)");
            return *this;
        }
        else
            return [&]<class... P>(std::type_identity<std::tuple<P...>>) -> frame_router& {
                return on<Tag, lit<F>, P...>(std::forward<H>(h));
            }(std::type_identity<detail::handler_params_t<H>>{});
    }

    std::unordered_map<std::uint64_t, std::function<status(std::span<const std::byte>)>> table_;
    frame_handler otherwise_;
    log_handler on_log_;                           // 2.2.1
    std::array<log_record_handler, 6> by_level_{}; // on_log(level, h): one handler per floor
    std::uint8_t lowest_floor() const noexcept
    {
        for (std::uint8_t i = 0; i < 6; ++i)
            if (by_level_[i])
                return i;
        return 6;
    }
    error_handler on_error_;
    std::uint64_t handled_ = 0, unhandled_ = 0, failed_ = 0;
};

} // namespace es::serreg
