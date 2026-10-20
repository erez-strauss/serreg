// es/serreg/register_namespace.hpp — register every serialisable type of a namespace before main() (DECISIONS A-22).
//
// serialize<T>() registers T by instantiation: the first call site anywhere in the program is the registration.
// A program that only *reads*, a schema publisher, or a plugin whose types are only ever received has no such
// call site, and so its dictionary — the capture's, the binary's es_serreg_dict section, the running process's —
// would miss those types. This header walks a namespace with std::meta::members_of and odr-uses the same
// registrar guards serialize() would, for every complete class and enum type found there (nested namespaces
// included), filtered by a selector:
//
//   #include <es/serreg/register_namespace.hpp>
//   namespace sr = es::serreg;
//   ES_SERREG_REGISTER_NAMESPACE(app::messages);                                  // all of them (the default)
//   ES_SERREG_REGISTER_NAMESPACE(app::messages, sr::name_prefix<"Msg">{});       // a sub-group by name
//   ES_SERREG_REGISTER_NAMESPACE(app::messages, sr::annotated<sr::message_t>{}); // struct [[=sr::message]] Order {…};
//   static_cast<void>(&sr::register_namespace<^^app::messages, sr::name_glob<"*Request">{}>);   // the spelling behind the macro
//
// What is selected can be inspected at compile time — sr::selected_type_names<^^app::messages, Select>() is a span
// of canonical names, usable in a static_assert or a std::println — so a user can dump the whole namespace once
// and then narrow the selection with a pattern.
//
// Rules:
//  * The walk sees what is declared before the point of instantiation, in this translation unit: register after
//    the headers that declare the types, once per namespace (one translation unit is enough for the program).
//  * Only types the library can serialise are registered (detail::supported_type<T>: the same rules as
//    check_supported(), as a predicate); a helper struct with a pointer member is skipped, not an error.
//    A type marked [[=es::serreg::skip]] is skipped too. Class templates and incomplete types are never types.
//  * Name selectors match the unqualified identifier ("Order"); qualified_glob matches the canonical name
//    ("app::messages::Order"). Patterns are a convention the compiler cannot check — prefer the annotation
//    for a message set that must stay exact.
//  * Cost: each selected type costs what its first serialize() would (descriptor, blob, hash; ~250 B of
//    es_serreg_dict per type per translation unit). Nothing runs at start-up beyond the guards' push.
#pragma once
#include <es/serreg/hash.hpp>
#include <es/serreg/reflect.hpp>
#include <es/serreg/registry.hpp>

#include <meta>
#include <span>
#include <string_view>
#include <vector>

namespace es::serreg
{

// Annotation for the opt-in selector: struct [[=es::serreg::message]] Order { … };
struct message_t
{
};
inline constexpr message_t message{};

namespace detail
{
// check_supported() as a predicate: true when serialize<T>() would compile. Mirrors its branches without the
// diagnostics (a namespace holds helpers that are not messages; those are skipped, not reported).
template<class T> consteval bool supported_type()
{
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U> || std::is_enum_v<U>)
        return true;
    else if constexpr (array_traits<U>::is_array)
        return supported_type<typename array_traits<U>::elem>();
    else if constexpr (is_dyn_member_v<U>)
    {
        if constexpr (dyn_member<U>::k == kind::vector_ || dyn_member<U>::k == kind::optional_)
            return !(dyn_member<U>::k == kind::vector_ &&
                     std::is_same_v<typename dyn_member<U>::elem, bool>) &&
                   supported_type<typename dyn_member<U>::elem>();
        else if constexpr (dyn_member<U>::k == kind::variant_)
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (supported_type<std::variant_alternative_t<I, U>>() && ...);
            }(std::make_index_sequence<dyn_member<U>::count>{});
        else if constexpr (dyn_member<U>::k == kind::map_)
            return supported_type<typename dyn_member<U>::key>() &&
                   supported_type<typename dyn_member<U>::elem>() &&
                   (dyn_member<U>::ordered || std::totally_ordered<typename dyn_member<U>::key> ||
                    std::is_trivially_copyable_v<typename dyn_member<U>::key>);
        else if constexpr (dyn_member<U>::k == kind::set_)
            return supported_type<typename dyn_member<U>::key>() &&
                   (dyn_member<U>::ordered || std::totally_ordered<typename dyn_member<U>::key> ||
                    std::is_trivially_copyable_v<typename dyn_member<U>::key>);
        else
            return true; // a string
    }
    else if constexpr (is_tuple_like_v<U>)
    {
        if constexpr (std::tuple_size_v<U> == 0)
            return false;
        else
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (supported_type<std::tuple_element_t<I, U>>() && ...);
            }(std::make_index_sequence<std::tuple_size_v<U>>{});
    }
    else if constexpr (!std::is_class_v<U> || std::is_pointer_v<U> || std::is_member_pointer_v<U> ||
                       std::is_reference_v<U> || std::is_union_v<U> || std::is_polymorphic_v<U> ||
                       is_smart_pointer_v<U> || is_string_view_v<U>)
        return false;
    else if constexpr (in_std(std::meta::dealias(^^U)) && !has_properties_name<U>)
        return false;
    else if constexpr (recursive_type(^^U))
        return false;
    else
    { // a user class: every base and every non-skipped member must be serialisable
        bool ok = true;
        template for (constexpr auto b : std::define_static_array(
                          std::meta::bases_of(^^U, std::meta::access_context::unchecked()))) ok =
            ok&& supported_type<typename[:std::meta::type_of(b):]>();
        template for (constexpr auto m : members<U>()) if constexpr (!has_annotation<skip_t>(m))
            ok =
                ok&& supported_type<typename[:std::meta::type_of(m):]>();
        return ok;
    }
}

// '*' any run, '?' one character; no classes (a type name has no need of them)
consteval bool glob_match(const std::string_view pat, const std::string_view s)
{
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size())
    {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i]))
        {
            ++p;
            ++i;
        }
        else if (p < pat.size() && pat[p] == '*')
        {
            star = p++;
            mark = i;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            i = ++mark;
        }
        else
            return false;
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

// The complete class and enum types of a namespace, nested namespaces included when `recurse`, in declaration
// order. Templates, functions, variables, aliases and incomplete types are not types here.
consteval void collect_types(const std::meta::info ns, const bool recurse,
                             std::vector<std::meta::info>& out)
{
    for (const std::meta::info m :
         std::meta::members_of(ns, std::meta::access_context::unchecked()))
    {
        if (std::meta::is_namespace(m))
        {
            if (recurse)
                collect_types(m, recurse, out);
        }
        else if (std::meta::is_type(m) && !std::meta::is_type_alias(m) &&
                 (std::meta::is_class_type(m) || std::meta::is_enum_type(m)) &&
                 std::meta::is_complete_type(m) && !has_annotation<skip_t>(m))
            out.push_back(m);
    }
}
consteval std::string_view type_identifier(const std::meta::info t)
{
    return std::meta::has_identifier(t) ? std::meta::identifier_of(t) : std::string_view{};
}
} // namespace detail

// ---- selectors: consteval callables std::meta::info -> bool -------------------------------------------------
struct select_all
{
    consteval bool operator()(std::meta::info) const
    {
        return true;
    }
};
template<fixed_string P> struct name_prefix
{
    consteval bool operator()(const std::meta::info t) const
    {
        return detail::type_identifier(t).starts_with(P.view());
    }
};
template<fixed_string S> struct name_suffix
{
    consteval bool operator()(const std::meta::info t) const
    {
        return detail::type_identifier(t).ends_with(S.view());
    }
};
template<fixed_string G> struct name_glob
{
    consteval bool operator()(const std::meta::info t) const
    {
        return detail::glob_match(G.view(), detail::type_identifier(t));
    }
};
template<fixed_string G> struct qualified_glob
{ // against the canonical (display) name: "app::messages::Order"
    consteval bool operator()(const std::meta::info t) const
    {
        return detail::glob_match(G.view(), std::meta::display_string_of(t));
    }
};
template<class A> struct annotated
{ // struct [[=es::serreg::message]] T; or any annotation type of the user's
    consteval bool operator()(const std::meta::info t) const
    {
        return detail::has_annotation<A>(t);
    }
};
template<auto S> struct not_
{
    consteval bool operator()(const std::meta::info t) const
    {
        return !S(t);
    }
};
template<auto... S> struct all_of
{
    consteval bool operator()(const std::meta::info t) const
    {
        return (S(t) && ...);
    }
};
template<auto... S> struct any_of
{
    consteval bool operator()(const std::meta::info t) const
    {
        return (S(t) || ...);
    }
};

// ---- the selection, for inspection --------------------------------------------------------------------------
// The types of NS that Select accepts, serialisable or not (what a user sees when dumping a namespace).
template<std::meta::info NS, auto Select = select_all{}, bool Recurse = true>
consteval std::span<const std::meta::info> selected_types()
{
    std::vector<std::meta::info> all, out;
    detail::collect_types(NS, Recurse, all);
    for (const std::meta::info t : all)
        if (Select(t))
            out.push_back(t);
    return std::define_static_array(out);
}
template<std::meta::info NS, auto Select = select_all{}, bool Recurse = true>
consteval std::span<const char* const> selected_type_names()
{ // NUL-terminated names (a std::string_view is not a structural type): print them, or static_assert over them
    std::vector<const char*> names;
    for (const std::meta::info t : selected_types<NS, Select, Recurse>())
        names.push_back(std::define_static_string(std::meta::display_string_of(t)));
    return std::define_static_array(names);
}

// ---- the registration ----------------------------------------------------------------------------------------
template<std::meta::info NS, auto Select = select_all{}, bool Recurse = true>
struct namespace_registrar
{
    // Instantiating this function instantiates registrar<T>::g for every selected, serialisable T (and, through
    // detail::touch, for every component type): that is the registration. The call itself does nothing.
    [[gnu::always_inline]] static inline void touch_all() noexcept
    {
        template for (constexpr std::meta::info t : selected_types<
                          NS, Select,
                          Recurse>()) if constexpr (detail::supported_type<typename[:t:]>())
            detail::touch<typename[:t:]>();
    }
    static constexpr std::size_t count()
    {
        std::size_t n = 0;
        template for (constexpr std::meta::info t : selected_types<
                          NS, Select,
                          Recurse>()) if constexpr (detail::supported_type<typename[:t:]>())++ n;
        return n;
    }
    static inline const int g =
        (touch_all(), 0); // the odr-use anchor (as registrar<T>::g for one type)
};
// Odr-use this (take its address) anywhere in one translation unit of the program:
//   static_cast<void>(&es::serreg::register_namespace<^^app::messages>);
// (register_types<Ts...>() in registry.hpp is the per-type spelling; this is the per-namespace one.)
template<std::meta::info NS, auto Select = select_all{}, bool Recurse = true>
inline const int& register_namespace = namespace_registrar<NS, Select, Recurse>::g;
// How many types a registration covers (serialisable ones), for a static_assert in the registering TU.
template<std::meta::info NS, auto Select = select_all{}, bool Recurse = true>
inline constexpr std::size_t registered_type_count =
    namespace_registrar<NS, Select, Recurse>::count();

} // namespace es::serreg

// ES_SERREG_REGISTER_NAMESPACE(ns)              — every serialisable type of ns and its nested namespaces
// ES_SERREG_REGISTER_NAMESPACE(ns, selector...) — the sub-group a selector accepts (a selector object: name_prefix<"Msg">{})
#define ES_SERREG_REGISTER_NAMESPACE_CAT2(a, b) a##b
#define ES_SERREG_REGISTER_NAMESPACE_CAT(a, b) ES_SERREG_REGISTER_NAMESPACE_CAT2(a, b)
#define ES_SERREG_REGISTER_NAMESPACE(ns, ...)                                                      \
    namespace                                                                                      \
    {                                                                                              \
    [[maybe_unused]] const int& ES_SERREG_REGISTER_NAMESPACE_CAT(es_serreg_registered_,            \
                                                                 __LINE__) =                       \
        ::es::serreg::register_namespace<^^ns __VA_OPT__(, ) __VA_ARGS__>;                         \
    }
