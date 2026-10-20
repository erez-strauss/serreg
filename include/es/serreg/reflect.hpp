// es/serreg/reflect.hpp — the ONLY header that uses C++26 reflection (<meta>, P2996). It turns a C++ type into
//   * descriptor_v<T>      constexpr type_desc in .rodata                             (spec §6.1)
//   * schema_hash_v<T>, layout_hash_v<T>  64-bit XXH3 fingerprints (fingerprint64) of canonical description strings (spec §6.2, FR-H1/H4)
//   * canonical_name<T>()  qualified name built by walking parent_of (spec §6.2b): an explicit name first
//                          (es::serreg::name annotation, es::serreg::type_properties<T>), the frozen std vocabulary,
//                          class parents named canonically (Outer<i32>::Inner), std's reserved namespaces dropped;
//                          a type without a linkage-unique name (unnamed namespace, local class) must be named
// Supported (v1, "flat"): arithmetic, bool, char family, std::byte, enums, C arrays, std::array, nested flat
// class types (all members incl. private, bases as "<base:Name>" pseudo-fields). Anything else → static_assert
// naming the type and member (FR-S7). Customisation: as_flat<T> (FR-S8), [[=skip]], [[=since(N)]],
// [[=deprecated(N)]] (FR-S11).
//
// Implementation note: member iteration uses index_sequence over define_static_array(...)[I] (the spec §6.1
// fallback) rather than `template for`, so it depends on as few P2996 companion papers as possible; `template for`
// appears only over a template's arguments (canonical_name) and an enum's enumerators (enumerators_fit).
#pragma once
#include <es/serreg/descriptor.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <flat_map>
#include <flat_set>
#include <forward_list>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <meta>
#include <optional>
#include <set>
#include <stack>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace es::serreg
{

// ---- customisation points ----------------------------------------------------------------------------
// Specialise to serialise T *as* another flat type: `using type = U; static U to(const T&); static T from(const U&);`
template<class T> struct as_flat;
template<class T>
concept has_as_flat = requires { typename as_flat<T>::type; };

// Annotations (P3394): struct S { [[=es::serreg::skip]] int cache; [[=es::serreg::since(3)]] int venue; };
struct skip_t
{
};
inline constexpr skip_t skip{};
struct since
{
    std::uint16_t v;
};
struct deprecated
{
    std::uint16_t v;
};

// 2.1.14 (review C-03/C-05, maintainer 2026-10-02): an explicit canonical name, for a type that has no
// linkage-unique name of its own (a class in an unnamed namespace or local to a function — two .cpp files can each
// define their own `X`), or to keep a name stable when a type moves:
//   namespace { struct [[=es::serreg::name("app::Quote")]] Quote { ... }; }
// The name replaces the qualified name in the schema string (and so in the schema hash) and in the dictionary.
// Allowed characters: letters, digits, '_', ':', '<', '>', ',' and '-'.
// Explicit names are a contract between programs, not something the library can check: two different types that
// two programs label with the same explicit name and the same layout are indistinguishable to every reader (the
// registry reports a conflict only when both definitions are loaded in one process and their layouts differ).
template<std::size_t N> struct name
{
    char s[N]{};
    consteval name(const char (&a)[N]) noexcept
    {
        for (std::size_t i = 0; i < N; ++i)
            s[i] = a[i];
    }
    consteval std::string_view view() const noexcept
    {
        return {s, N - 1};
    }
};

// 2.1.14 (DECISIONS P-3, the maintainer's "properties template"): specialise with
//   static consteval std::string name();
// to give any type — also a standard-library or third-party type that cannot be annotated — its canonical name.
// The library specialises it for the std vocabulary (std::string, std::vector<E>, std::optional<E>, …) so names
// never depend on the standard library's spelling. A type named both here and by the annotation is rejected.
template<class T> struct type_properties
{
};

// ---- scalar kinds ------------------------------------------------------------------------------------
namespace detail
{

template<class T> constexpr bool is_std_array_v = false;
template<class E, std::size_t N> constexpr bool is_std_array_v<std::array<E, N>> = true;

template<class T> struct array_traits
{
    static constexpr bool is_array = false;
};
template<class E, std::size_t N> struct array_traits<E[N]>
{
    static constexpr bool is_array = true;
    using elem = E;
    static constexpr std::size_t extent = N;
};
template<class E, std::size_t N> struct array_traits<std::array<E, N>>
{
    static constexpr bool is_array = true;
    using elem = E;
    static constexpr std::size_t extent = N;
};

template<class T> consteval kind scalar_kind()
{
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        return kind::boolean;
    else if constexpr (std::is_same_v<U, char>)
        return kind::char_;
    else if constexpr (std::is_same_v<U, char8_t>)
        return kind::char8;
    else if constexpr (std::is_same_v<U, char16_t>)
        return kind::char16;
    else if constexpr (std::is_same_v<U, char32_t>)
        return kind::char32;
    else if constexpr (std::is_same_v<U, wchar_t>)
        return kind::wchar;
    else if constexpr (std::is_same_v<U, std::byte>)
        return kind::byte_;
    else if constexpr (std::is_same_v<U, float>)
    {
        static_assert(sizeof(float) == 4);
        return kind::f32;
    }
    else if constexpr (std::is_same_v<U, double>)
    {
        static_assert(sizeof(double) == 8);
        return kind::f64;
    }
    else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
    {
        if constexpr (sizeof(U) == 1)
            return kind::i8;
        else if constexpr (sizeof(U) == 2)
            return kind::i16;
        else if constexpr (sizeof(U) == 4)
            return kind::i32;
        else
        {
            static_assert(sizeof(U) == 8, "es::serreg: 128-bit integers are not flat scalars");
            return kind::i64;
        }
    }
    else if constexpr (std::is_integral_v<U>)
    {
        if constexpr (sizeof(U) == 1)
            return kind::u8;
        else if constexpr (sizeof(U) == 2)
            return kind::u16;
        else if constexpr (sizeof(U) == 4)
            return kind::u32;
        else
        {
            static_assert(sizeof(U) == 8, "es::serreg: 128-bit integers are not flat scalars");
            return kind::u64;
        }
    }
    else
    {
        static_assert(
            sizeof(U) == 0,
            "es::serreg: not a supported scalar type (long double, __int128 ... are not flat)");
    }
}

template<class T>
constexpr bool is_scalar_v = (std::is_arithmetic_v<std::remove_cv_t<T>> &&
                              !std::is_same_v<std::remove_cv_t<T>, long double>) ||
                             std::is_same_v<std::remove_cv_t<T>, std::byte>;

// The type actually serialised for T (identity unless as_flat<T> is specialised).
template<class T> struct mapped
{
    using type = std::remove_cv_t<T>;
};
template<class T>
    requires has_as_flat<std::remove_cv_t<T>>
struct mapped<T>
{
    using type = typename as_flat<std::remove_cv_t<T>>::type;
};
template<class T> using mapped_t = typename mapped<T>::type;

// ---- dynamic members (v2, plan v2.0 §1): std::string, std::vector<T>, std::optional<T>, std::variant<Ts...> ----
// A "dyn member type" is one of these four; a "dynamic class" is a class type with a dyn member anywhere in
// its tree (members, bases, array elements, nested classes). Both are encoded as canonical images (§2).
template<class T> struct dyn_member : std::false_type
{
};
// 2.1.5 (review R§9.3): any std::basic_string<char, std::char_traits<char>, Alloc> — std::string, std::pmr::string,
// a string with an arena allocator. Same wire kind (string_), same schema text ("string"): the allocator is a
// property of the reader's object, not of the data, so it is neither encoded nor hashed.
template<class A>
struct dyn_member<std::basic_string<char, std::char_traits<char>, A>> : std::true_type
{
    static constexpr kind k = kind::string_;
};
template<class E, class A> struct dyn_member<std::vector<E, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = E;
};
template<class E> struct dyn_member<std::optional<E>> : std::true_type
{
    static constexpr kind k = kind::optional_;
    using elem = E;
};
template<class... Es> struct dyn_member<std::variant<Es...>> : std::true_type
{
    static constexpr kind k = kind::variant_;
    static constexpr std::size_t count = sizeof...(Es);
};
// 1.6 (2.2.0, plans-for-containers.md): maps and sets. `key`/`elem` are the key and the mapped type (a set's elem
// is its key); `ordered` says whether the container iterates in the canonical order already — ascending by the
// key's operator< (std::less<K> or the transparent std::less<>); every other container (unordered, or ordered by
// another comparator) is sorted by the writer, so that identical values give identical bytes (canonical order, §3
// of the plan). Floating-point keys are ordered by std::strong_order (review B2: a NaN key breaks operator<); a
// user key type must provide a strict total operator< (else, trivially copyable, it is compared by its bytes).
// The comparator, hasher and allocator are properties of the reader's object, not of the data: the
// wire shape is the same `map_`/`set_` kind for every container that maps onto it, and the kindspec in the schema
// string is `map<K,V>` / `set<K>` whatever the C++ container (as `string` is for every std::basic_string<char, …>).
template<class K, class C>
constexpr bool is_canonical_less_v =
    std::is_same_v<C, std::less<K>> || std::is_same_v<C, std::less<void>>;
template<class K, class V, class C, class A>
struct dyn_member<std::map<K, V, C, A>> : std::true_type
{
    static constexpr kind k = kind::map_;
    using key = K;
    using elem = V;
    static constexpr bool ordered = is_canonical_less_v<K, C>;
};
template<class K, class V, class H, class E, class A>
struct dyn_member<std::unordered_map<K, V, H, E, A>> : std::true_type
{
    static constexpr kind k = kind::map_;
    using key = K;
    using elem = V;
    static constexpr bool ordered = false;
};
template<class K, class V, class C, class KC, class MC>
struct dyn_member<std::flat_map<K, V, C, KC, MC>> : std::true_type
{
    static constexpr kind k = kind::map_;
    using key = K;
    using elem = V;
    static constexpr bool ordered = is_canonical_less_v<K, C>;
};
template<class K, class C, class A> struct dyn_member<std::set<K, C, A>> : std::true_type
{
    static constexpr kind k = kind::set_;
    using key = K;
    using elem = K;
    static constexpr bool ordered = is_canonical_less_v<K, C>;
};
template<class K, class H, class E, class A>
struct dyn_member<std::unordered_set<K, H, E, A>> : std::true_type
{
    static constexpr kind k = kind::set_;
    using key = K;
    using elem = K;
    static constexpr bool ordered = false;
};
template<class K, class C, class KC> struct dyn_member<std::flat_set<K, C, KC>> : std::true_type
{
    static constexpr kind k = kind::set_;
    using key = K;
    using elem = K;
    static constexpr bool ordered = is_canonical_less_v<K, C>;
};
// Sequence containers other than std::vector are the same `vector` wire shape: a block of elements. They differ
// from std::vector only in how the writer walks them (no indexing) and how the reader fills them (resize + walk).
template<class E, class A> struct dyn_member<std::deque<E, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = E;
};
template<class E, class A> struct dyn_member<std::list<E, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = E;
};
template<class E, class A> struct dyn_member<std::forward_list<E, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = E;
};
// A multimap is a vector of std::pair<K, V> entries sorted by key (the schema kindspec `multimap<K,V>` keeps it
// apart from a std::vector<std::pair<K, V>>, the layout string is the vector's): duplicate keys are data, and the
// order among equal keys is the container's — std::multimap's insertion order, a stable sort by key for
// std::unordered_multimap.
template<class K, class V, class C, class A>
struct dyn_member<std::multimap<K, V, C, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = std::pair<K, V>;
    static constexpr bool multi = true;
    static constexpr bool sort_by_key = !is_canonical_less_v<K, C>;
};
template<class K, class V, class H, class E, class A>
struct dyn_member<std::unordered_multimap<K, V, H, E, A>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = std::pair<K, V>;
    static constexpr bool multi = true;
    static constexpr bool sort_by_key = true;
};
// std::stack is its underlying container (deque by default): the `vector` shape, bottom first.
template<class T, class C> struct dyn_member<std::stack<T, C>> : std::true_type
{
    static constexpr kind k = kind::vector_;
    using elem = T;
    static constexpr bool adapter = true;
};
template<class T> constexpr bool is_dyn_member_v = dyn_member<std::remove_cv_t<T>>::value;
template<class T> consteval bool is_multimap_member()
{
    if constexpr (requires { dyn_member<std::remove_cv_t<T>>::multi; })
        return dyn_member<std::remove_cv_t<T>>::multi;
    else
        return false;
}
template<class T> consteval bool is_container_adapter()
{
    if constexpr (requires { dyn_member<std::remove_cv_t<T>>::adapter; })
        return dyn_member<std::remove_cv_t<T>>::adapter;
    else
        return false;
}
// Tuple-like classes (std::pair, std::tuple) are serialised as positional parts named first/second and _0, _1, …
// at canonical offsets, always element-wise through std::get (their memory layout is the standard library's: a
// libstdc++ tuple stores its elements in reverse order), so they are dynamic classes even when every part is flat.
template<class T> constexpr bool is_tuple_like_v = false;
template<class A, class B> constexpr bool is_tuple_like_v<std::pair<A, B>> = true;
template<class... Ts> constexpr bool is_tuple_like_v<std::tuple<Ts...>> = true;
template<class T> constexpr bool is_smart_pointer_v = false;
template<class T, class D> constexpr bool is_smart_pointer_v<std::unique_ptr<T, D>> = true;
template<class T> constexpr bool is_smart_pointer_v<std::shared_ptr<T>> = true;
template<class T> constexpr bool is_smart_pointer_v<std::weak_ptr<T>> = true;
template<class T> consteval bool is_map_or_set()
{
    if constexpr (is_dyn_member_v<T>)
        return dyn_member<std::remove_cv_t<T>>::k == kind::map_ ||
               dyn_member<std::remove_cv_t<T>>::k == kind::set_;
    else
        return false;
}
// 1.6: a container passed as an argument (not a member) is a type of its own — a transparent wrapper whose only
// field is the container (§1 of the plan). A top-level std::string stays the `string` argument category.
template<class T> consteval bool is_container_arg()
{
    if constexpr (is_dyn_member_v<T>)
        return dyn_member<std::remove_cv_t<T>>::k != kind::string_;
    else
        return false;
}
template<class T> constexpr bool is_container_arg_v = is_container_arg<T>();
template<class T>
constexpr bool is_string_view_v = std::is_same_v<std::remove_cv_t<T>, std::string_view>;

template<class T> consteval bool is_dynamic(); // any dyn member in the tree of T (class, array, …)
template<class T> consteval bool is_dynamic_class() // a class that is encoded as a canonical image
{
    using U = mapped_t<T>;
    return std::is_class_v<U> && !is_dyn_member_v<U> && !array_traits<U>::is_array &&
           is_dynamic<U>();
}

template<class E> consteval std::uint32_t image_stride();
// 2.1.12 (review D-05): an element that occupies no bytes in the image (std::array<T, 0> of a dynamic T) cannot be
// told apart by position: the dictionary refuses it, and the typed decoder accepted any count of them (2^32-1 from
// a 16-byte image). Such an element is rejected where it is declared.
template<class E> consteval void check_nonempty_element()
{
    static_assert(
        image_stride<E>() != 0,
        "es::serreg: a vector, optional or variant element must occupy bytes in the image; "
        "std::array<T, 0> of a dynamic T occupies none");
}

// 2.1.14 (review C-07): does a class contain itself — through members, bases, arrays, std::array, std::vector,
// std::optional or std::variant? Such a type has no finite canonical description; without this check the compiler
// fails with an opaque recursion error. Walks reflections (no template instantiation), visiting each type once.
consteval bool in_std(std::meta::info t)
{ // stops at a function: a local class is never std's, and GCC 16 cannot take parent_of(main) (review C-05)
    for (std::meta::info p = std::meta::parent_of(t); p != ^^::&&!std::meta::is_function(p);
         p = std::meta::parent_of(p))
        if (p == ^^std)
            return true;
    return false;
}
consteval void component_types(const std::meta::info type, std::vector<std::meta::info>& out)
{
    const std::meta::info t = std::meta::dealias(
        std::meta::remove_all_extents(std::meta::remove_cv(std::meta::dealias(type))));
    if (!std::meta::is_class_type(t))
        return;
    if (std::meta::has_template_arguments(t) && in_std(t))
    {
        const std::meta::info tmpl = std::meta::template_of(t);
        if (tmpl == ^^std::vector || tmpl == ^^std::optional || tmpl == ^^std::array ||
            tmpl == ^^std::variant || tmpl == ^^std::map || tmpl == ^^std::unordered_map ||
            tmpl == ^^std::flat_map || tmpl == ^^std::set || tmpl == ^^std::unordered_set ||
            tmpl == ^^std::flat_set || tmpl == ^^std::deque || tmpl == ^^std::list ||
            tmpl == ^^std::forward_list || tmpl == ^^std::multimap ||
            tmpl == ^^std::unordered_multimap || tmpl == ^^std::stack || tmpl == ^^std::pair ||
            tmpl == ^^std::tuple)
            for (const std::meta::info a : std::meta::template_arguments_of(t))
                if (std::meta::is_type(a))
                    out.push_back(
                        a); // the key, the mapped type (and the comparator/hasher: no user type)
        return;             // std::basic_string and every other std class: no user type inside
    }
    if (in_std(t))
        return;
    for (const std::meta::info m :
         std::meta::nonstatic_data_members_of(t, std::meta::access_context::unchecked()))
        out.push_back(std::meta::type_of(m));
    for (const std::meta::info b : std::meta::bases_of(t, std::meta::access_context::unchecked()))
        out.push_back(std::meta::type_of(b));
}
consteval bool recursive_type(const std::meta::info root)
{
    const std::meta::info start = std::meta::dealias(std::meta::remove_cv(root));
    std::vector<std::meta::info> todo, seen;
    component_types(start, todo);
    while (!todo.empty())
    {
        std::meta::info t = todo.back();
        todo.pop_back();
        t = std::meta::dealias(
            std::meta::remove_all_extents(std::meta::remove_cv(std::meta::dealias(t))));
        if (t == start)
            return true;
        if (std::ranges::find(seen, t) != seen.end())
            continue;
        seen.push_back(t);
        component_types(t, todo);
    }
    return false;
}

template<class T>
concept has_properties_name = requires {
    { type_properties<T>::name() };
};

// Compile-time rejection with a useful message (FR-S7).
template<class T> consteval void check_supported()
{
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U> || std::is_enum_v<U>)
    {
    }
    else if constexpr (array_traits<U>::is_array)
    {
        check_supported<typename array_traits<U>::elem>();
    }
    else if constexpr (is_dyn_member_v<U>)
    { // v2: the element types must be serialisable themselves
        if constexpr (dyn_member<U>::k == kind::vector_)
            static_assert(!std::is_same_v<typename dyn_member<U>::elem, bool>,
                          "es::serreg: std::vector<bool> is a bitset, not a container of bool; use "
                          "std::vector<std::uint8_t>");
        if constexpr (dyn_member<U>::k == kind::vector_ || dyn_member<U>::k == kind::optional_)
        {
            check_supported<typename dyn_member<U>::elem>();
            check_nonempty_element<typename dyn_member<U>::elem>();
        }
        else if constexpr (dyn_member<U>::k == kind::variant_)
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (check_supported<std::variant_alternative_t<I, U>>(), ...);
                (check_nonempty_element<std::variant_alternative_t<I, U>>(), ...);
            }(std::make_index_sequence<dyn_member<U>::count>{});
        else if constexpr (dyn_member<U>::k == kind::map_ || dyn_member<U>::k == kind::set_)
        { // 1.6: the key (and the mapped type) must be serialisable; an unordered container is sorted by the
            // writer, which needs an order on the key (operator< or a flat key compared by its bytes)
            using K = typename dyn_member<U>::key;
            check_supported<K>();
            check_nonempty_element<K>();
            if constexpr (dyn_member<U>::k == kind::map_)
            {
                check_supported<typename dyn_member<U>::elem>();
                check_nonempty_element<typename dyn_member<U>::elem>();
            }
            static_assert(
                dyn_member<U>::ordered || std::totally_ordered<K> ||
                    std::is_trivially_copyable_v<K>,
                "es::serreg: an unordered container, or one ordered by a comparator other than "
                "std::less, is written in canonical order, which needs operator< on its key (or a "
                "trivially copyable key, compared by its bytes)");
        }
    }
    else if constexpr (is_tuple_like_v<U>)
    { // positional parts: every part must be serialisable; an empty tuple has no bytes to describe
        static_assert(std::tuple_size_v<U> != 0,
                      "es::serreg: std::tuple<> has no parts to serialise");
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (check_supported<std::tuple_element_t<I, U>>(), ...);
        }(std::make_index_sequence<std::tuple_size_v<U>>{});
    }
    else
    {
        static_assert(
            !is_smart_pointer_v<U>,
            "es::serreg: smart pointers (std::unique_ptr, std::shared_ptr, std::weak_ptr) are "
            "not serialisable: ownership is not data; store the value, or std::optional<T> "
            "for a nullable one");
        if constexpr (std::is_class_v<U>)
            static_assert(
                !in_std(std::meta::dealias(^^U)) || has_properties_name<U>,
                "es::serreg: this standard-library type is not in the serialisable vocabulary "
                "(std::string, std::vector, std::deque, std::list, std::forward_list, std::array, "
                "std::optional, std::variant, std::pair, std::tuple, std::map, std::unordered_map, "
                "std::flat_map, std::multimap, std::unordered_multimap, std::set, "
                "std::unordered_set, "
                "std::flat_set, std::stack); map it with as_flat<T> or give it a canonical name "
                "with "
                "es::serreg::type_properties<T>");
        if constexpr (std::is_class_v<U>)
            static_assert(
                !recursive_type(^^U),
                "es::serreg: recursive types are not supported (the type contains itself through "
                "a member, base, array, std::vector, std::optional or std::variant)");
        static_assert(!std::is_pointer_v<U> && !std::is_member_pointer_v<U>,
                      "es::serreg: pointers are not serialisable; provide as_flat<T> to map the "
                      "enclosing type");
        static_assert(!std::is_reference_v<U>, "es::serreg: references are not serialisable");
        static_assert(!std::is_union_v<U>,
                      "es::serreg: unions are not serialisable; use std::variant or as_flat<T>");
        static_assert(
            !std::is_polymorphic_v<U>,
            "es::serreg: polymorphic types (virtual functions/bases) are not serialisable");
        static_assert(std::is_class_v<U>, "es::serreg: unsupported member type");
        static_assert(
            !is_string_view_v<U>,
            "es::serreg: std::string_view is not serialisable as a member (it could not be "
            "read back); use std::string");
        static_assert(
            std::is_trivially_copyable_v<U> || is_dynamic<U>(),
            "es::serreg: non-trivially-copyable types are not serialisable unless they "
            "are std::string (any allocator), std::vector, std::optional, std::variant, "
            "std::map, std::unordered_map, std::flat_map, std::set, std::unordered_set, "
            "std::flat_set or a class built from those; use as_flat<T> for anything else");
    }
}

// ---- reflection helpers ------------------------------------------------------------------------------
consteval std::meta::access_context ctx()
{
    return std::meta::access_context::unchecked();
} // FR-S9

template<class T> consteval auto members()
{
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx()));
}
template<class T> consteval auto bases()
{
    return std::define_static_array(std::meta::bases_of(^^T, ctx()));
}
template<class E> consteval auto enumerators()
{
    return std::define_static_array(std::meta::enumerators_of(^^E));
}

// ---- parts: the positional view of a class every walk uses ------------------------------------------------
// A class has bases (none for a tuple-like type) and parts: its non-static data members, or for std::pair and
// std::tuple the elements, named first/second and _0, _1, … and reached through std::get. Everything that walks a
// class — fields, canonical strings, image layout, measure/encode/decode, registration — goes through these, so
// the two kinds of class share one code path.
template<class T> consteval std::size_t base_count()
{
    if constexpr (is_tuple_like_v<T>)
        return 0;
    else
        return bases<T>().size();
}
template<class T> consteval std::size_t part_count()
{
    if constexpr (is_tuple_like_v<T>)
        return std::tuple_size_v<T>;
    else
        return members<T>().size();
}
template<class T, std::size_t I, bool = is_tuple_like_v<T>> struct part_type
{
    using type = typename[:std::meta::type_of(members<T>()[I]):];
};
template<class T, std::size_t I> struct part_type<T, I, true>
{
    using type = std::tuple_element_t<I, T>;
};
template<class T, std::size_t I> using part_t = typename part_type<T, I>::type;
template<class T, std::size_t I> using base_t = typename[:std::meta::type_of(bases<T>()[I]):];
template<class T, std::size_t I> consteval std::string part_name()
{
    if constexpr (is_tuple_like_v<T>)
    {
        if constexpr (requires(T& t) { t.first; })
            return I == 0 ? "first" : "second";
        else
        {
            std::string s = "_";
            append_uint(s, I);
            return s;
        }
    }
    else
        return std::string(std::meta::identifier_of(members<T>()[I]));
}
template<std::size_t I, class T> constexpr decltype(auto) get_part(T& v) noexcept
{
    if constexpr (is_tuple_like_v<std::remove_cv_t<T>>)
        return (std::get<I>(v));
    else
        return (v.[:members<std::remove_cv_t<T>>()[I]:]);
}

template<class T, std::size_t I> consteval bool member_skipped();
template<class A, class T, std::size_t I> consteval std::uint16_t part_annotation();

template<class A> consteval bool has_annotation(const std::meta::info m)
{
    for (const auto a : std::meta::annotations_of(m))
        if (std::meta::remove_cv(std::meta::type_of(a)) == ^^A)
            return true;
    return false;
}
template<class A> consteval std::uint16_t annotation_value(const std::meta::info m)
{ // since/deprecated; 0 when absent
    for (const auto a : std::meta::annotations_of(m))
        if (std::meta::remove_cv(std::meta::type_of(a)) == ^^A)
            return std::meta::extract<A>(a).v;
    return 0;
}

// Canonical qualified name (§6.2b): walk parent_of up to the global namespace.
template<class T> consteval std::string canonical_name();

// Builtin types by kind name, so template arguments never leak compiler spellings (int vs long).
consteval std::string_view scalar_name(const std::meta::info t)
{
    struct e
    {
        std::meta::info t;
        std::string_view n;
    };
    constexpr e table[]{{^^bool, "bool"},
                        {^^char, "char"},
                        {^^signed char, "i8"},
                        {^^unsigned char, "u8"},
                        {^^short, "i16"},
                        {^^unsigned short, "u16"},
                        {^^int, "i32"},
                        {^^unsigned int, "u32"},
                        {^^long, sizeof(long) == 8 ? "i64" : "i32"},
                        {^^unsigned long, sizeof(long) == 8 ? "u64" : "u32"},
                        {^^long long, "i64"},
                        {^^unsigned long long, "u64"},
                        {^^float, "f32"},
                        {^^double, "f64"},
                        {^^char8_t, "char8"},
                        {^^char16_t, "char16"},
                        {^^char32_t, "char32"},
                        {^^wchar_t, "wchar"},
                        {^^std::byte, "byte"}};
    for (const e& x : table)
        if (x.t == t)
            return x.n;
    return {};
}

// 2.1.14: does r lack a linkage-unique name? True for an entity declared (directly, or through enclosing
// namespaces) in an unnamed namespace or inside a function; an enclosing class carries its own rule.
consteval bool lacks_linkage(const std::meta::info r)
{
    std::meta::info p = std::meta::parent_of(r);
    while (p != ^^::)
    {
        if (std::meta::is_type(p))
            return false;
        if (std::meta::is_function(p))
            return true;
        if (std::meta::is_namespace(p) && !std::meta::has_identifier(p))
            return true;
        p = std::meta::parent_of(p);
    }
    return false;
}
// A namespace of the standard library's implementation (libstdc++ std::__cxx11, std::chrono::_V2; libc++ std::__1):
// a reserved identifier inside std. Dropped from names, so they do not depend on the library (review C-04).
consteval bool std_implementation_namespace(const std::meta::info ns)
{
    if (!std::meta::has_identifier(ns) || std::meta::identifier_of(ns).empty() ||
        std::meta::identifier_of(ns)[0] != '_')
        return false;
    for (std::meta::info p = std::meta::parent_of(ns); p != ^^::; p = std::meta::parent_of(p))
        if (p == ^^std)
            return true;
    return false;
}
// "scope::" for the scope P of a named entity: namespaces by identifier, a class (also a class-template
// specialisation, review C-03) by its own canonical name.
template<std::meta::info P> consteval std::string scope_of()
{
    if constexpr (P == ^^::)
        return {};
    else if constexpr (std::meta::is_type(P))
        return canonical_name<typename[:P:]>() + "::";
    else
    {
        std::string s = scope_of<std::meta::parent_of(P)>();
        if constexpr (!std_implementation_namespace(P))
        {
            s += std::meta::identifier_of(P);
            s += "::";
        }
        return s;
    }
}

// Explicit names.
template<class U>
consteval std::string annotated_name() // "" when the type carries no es::serreg::name
{
    std::string out;
    if constexpr (std::is_class_v<U> || std::is_enum_v<U>)
        template for (constexpr std::meta::info a :
                      std::define_static_array(std::meta::annotations_of(std::meta::dealias(^^U))))
        {
            constexpr std::meta::info ty = std::meta::remove_cv(std::meta::type_of(a));
            if constexpr (std::meta::has_template_arguments(ty) &&
                          std::meta::template_of(ty) == ^^es::serreg::name)
                out = std::string(std::meta::extract<typename[:ty:]>(a).view());
        }
    return out;
}
// The explicit names as static strings (one evaluation per type; clang-p2996 rejects a std::string temporary in the
// conditions below, so both compilers use these).
template<class U>
inline constexpr std::string_view annotated_name_v = std::define_static_string(annotated_name<U>());
template<class U> consteval std::string properties_name()
{
    if constexpr (has_properties_name<U>)
        return std::string(type_properties<U>::name());
    else
        return {};
}
template<class U>
inline constexpr std::string_view properties_name_v =
    std::define_static_string(properties_name<U>());
consteval bool valid_explicit_name(const std::string_view n)
{
    if (n.empty())
        return false;
    for (const char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == ':' || c == '<' || c == '>' || c == ',' || c == '-'))
            return false;
    return true;
}
template<class> inline constexpr bool always_false_v = false;

// Template-argument values (cc F3): never a compiler spelling. bool → true/false, integral and character
// types → decimal value, enums → enum(<underlying decimal>); anything else is rejected.
template<class V> consteval std::string render_value_arg(const V v)
{
    std::string s;
    if constexpr (std::is_same_v<V, bool>)
    {
        s += v ? "true" : "false";
    }
    else if constexpr (std::is_enum_v<V>)
    {
        s += "enum(";
        append_int(s, static_cast<std::int64_t>(std::to_underlying(v)));
        s += ')';
    }
    else if constexpr (std::is_integral_v<V> && std::is_signed_v<V>)
    {
        append_int(s, static_cast<std::int64_t>(v));
    }
    else if constexpr (std::is_integral_v<V>)
    {
        append_uint(s, static_cast<std::uint64_t>(v));
    }
    else
        static_assert(sizeof(V) == 0, "es::serreg: only bool, integral, character and enum "
                                      "template arguments can appear in a canonical name");
    return s;
}

template<class T> consteval std::string canonical_name()
{
    using U = std::remove_cv_t<T>;
    if constexpr (is_scalar_v<U>)
        return std::string(to_string(scalar_kind<U>()));
    else
    {
        constexpr std::meta::info r = std::meta::dealias(^^U);
        static_assert(
            annotated_name_v<U>.empty() || !has_properties_name<U>,
            "es::serreg: the canonical name is given twice (the es::serreg::name annotation and "
            "es::serreg::type_properties<T>)");
        if constexpr (has_properties_name<U>)
        {
            static_assert(
                valid_explicit_name(properties_name_v<U>),
                "es::serreg: an explicit canonical name must be non-empty and use only letters, "
                "digits, '_', ':', '<', '>', ',' and '-'");
            return std::string(properties_name_v<U>);
        }
        else if constexpr (!annotated_name_v<U>.empty())
        {
            static_assert(
                valid_explicit_name(annotated_name_v<U>),
                "es::serreg: an explicit canonical name must be non-empty and use only letters, "
                "digits, '_', ':', '<', '>', ',' and '-'");
            return std::string(annotated_name_v<U>);
        }
        else if constexpr (!std::meta::has_template_arguments(r) && !std::meta::has_identifier(r))
        {
            static_assert(always_false_v<U>,
                          "es::serreg: unnamed types cannot be serialised; give the type a name or "
                          "use as_flat<T>");
            return {};
        }
        else if constexpr (lacks_linkage(r))
        {
            static_assert(
                always_false_v<U>,
                "es::serreg: a type in an unnamed namespace or local to a function has no unique "
                "canonical name (another one may exist elsewhere with the same spelling); name it "
                "with [[=es::serreg::name(\"...\")]] or es::serreg::type_properties<T>");
            return {};
        }
        else if constexpr (std::meta::has_template_arguments(r))
        {
            std::string s = scope_of<std::meta::parent_of(r)>();
            s += std::meta::identifier_of(std::meta::template_of(r));
            s += '<';
            bool first = true;
            template for (constexpr std::meta::info a :
                          std::define_static_array(std::meta::template_arguments_of(r)))
            {
                if (!first)
                    s += ',';
                first = false;
                if constexpr (std::meta::is_type(a))
                    s += canonical_name<typename[:a:]>();
                else if constexpr (std::meta::is_value(a) || std::meta::is_object(a))
                    s += render_value_arg([:a:]);
                else
                    static_assert(std::meta::is_type(a), "es::serreg: template-template arguments "
                                                         "cannot appear in a canonical name");
            }
            s += '>';
            return s;
        }
        else
            return scope_of<std::meta::parent_of(r)>() + std::string(std::meta::identifier_of(r));
    }
}
template<class... Ts> consteval std::string canonical_list() // "A,B,C" (std vocabulary)
{
    std::string s;
    ((s += (s.empty() ? "" : ","), s += canonical_name<Ts>()), ...);
    return s;
}

} // namespace detail

// ---- the std vocabulary (2.1.14, review C-04): frozen names, independent of the standard library -------------
// No inline namespace (std::__cxx11, std::__1), no default argument, no allocator (it is not data: a std::pmr::string
// and a std::string have the same wire form and the same name). Other std types fall back to the general rule,
// which drops std's reserved namespaces (std::chrono::duration<i64,std::ratio<1,1000>>).
template<class A> struct type_properties<std::basic_string<char, std::char_traits<char>, A>>
{
    static consteval std::string name()
    {
        return "std::string";
    }
};
template<class C, class A> struct type_properties<std::basic_string<C, std::char_traits<C>, A>>
{
    static consteval std::string name()
    {
        return "std::basic_string<" + detail::canonical_name<C>() + ">";
    }
};
template<class E, class A> struct type_properties<std::vector<E, A>>
{
    static consteval std::string name()
    {
        return "std::vector<" + detail::canonical_name<E>() + ">";
    }
};
template<class E, std::size_t N> struct type_properties<std::array<E, N>>
{
    static consteval std::string name()
    {
        std::string s = "std::array<" + detail::canonical_name<E>() + ",";
        detail::append_uint(s, N);
        return s + ">";
    }
};
template<class E> struct type_properties<std::optional<E>>
{
    static consteval std::string name()
    {
        return "std::optional<" + detail::canonical_name<E>() + ">";
    }
};
template<class... Es> struct type_properties<std::variant<Es...>>
{
    static consteval std::string name()
    {
        return "std::variant<" + detail::canonical_list<Es...>() + ">";
    }
};
// 1.6: containers. A comparator, hasher or equality other than the defaults is part of the C++ type and enters
// the name (two maps with different comparators iterate — and so encode — in different orders); the allocator does
// not (as for std::string).
namespace detail
{
template<class K, class C> consteval std::string cmp_suffix()
{
    if constexpr (std::is_same_v<C, std::less<K>>)
        return "";
    else
        return "," + canonical_name<C>();
}
template<class K, class H, class E> consteval std::string hash_suffix()
{
    if constexpr (std::is_same_v<H, std::hash<K>> && std::is_same_v<E, std::equal_to<K>>)
        return "";
    else
        return "," + canonical_name<H>() + "," + canonical_name<E>();
}
} // namespace detail
template<class K, class V, class C, class A> struct type_properties<std::map<K, V, C, A>>
{
    static consteval std::string name()
    {
        return "std::map<" + detail::canonical_list<K, V>() + detail::cmp_suffix<K, C>() + ">";
    }
};
template<class K, class V, class H, class E, class A>
struct type_properties<std::unordered_map<K, V, H, E, A>>
{
    static consteval std::string name()
    {
        return "std::unordered_map<" + detail::canonical_list<K, V>() +
               detail::hash_suffix<K, H, E>() + ">";
    }
};
template<class K, class V, class C, class KC, class MC>
struct type_properties<std::flat_map<K, V, C, KC, MC>>
{
    static consteval std::string name()
    {
        return "std::flat_map<" + detail::canonical_list<K, V>() + detail::cmp_suffix<K, C>() + ">";
    }
};
template<class K, class C, class A> struct type_properties<std::set<K, C, A>>
{
    static consteval std::string name()
    {
        return "std::set<" + detail::canonical_name<K>() + detail::cmp_suffix<K, C>() + ">";
    }
};
template<class K, class H, class E, class A> struct type_properties<std::unordered_set<K, H, E, A>>
{
    static consteval std::string name()
    {
        return "std::unordered_set<" + detail::canonical_name<K>() +
               detail::hash_suffix<K, H, E>() + ">";
    }
};
template<class K, class C, class KC> struct type_properties<std::flat_set<K, C, KC>>
{
    static consteval std::string name()
    {
        return "std::flat_set<" + detail::canonical_name<K>() + detail::cmp_suffix<K, C>() + ">";
    }
};
template<class E, class A> struct type_properties<std::deque<E, A>>
{
    static consteval std::string name()
    {
        return "std::deque<" + detail::canonical_name<E>() + ">";
    }
};
template<class E, class A> struct type_properties<std::list<E, A>>
{
    static consteval std::string name()
    {
        return "std::list<" + detail::canonical_name<E>() + ">";
    }
};
template<class E, class A> struct type_properties<std::forward_list<E, A>>
{
    static consteval std::string name()
    {
        return "std::forward_list<" + detail::canonical_name<E>() + ">";
    }
};
template<class K, class V, class C, class A> struct type_properties<std::multimap<K, V, C, A>>
{
    static consteval std::string name()
    {
        return "std::multimap<" + detail::canonical_name<K>() + "," + detail::canonical_name<V>() +
               detail::cmp_suffix<K, C>() + ">";
    }
};
template<class K, class V, class H, class E, class A>
struct type_properties<std::unordered_multimap<K, V, H, E, A>>
{
    static consteval std::string name()
    {
        return "std::unordered_multimap<" + detail::canonical_name<K>() + "," +
               detail::canonical_name<V>() + detail::hash_suffix<K, H, E>() + ">";
    }
};
template<class T, class C> struct type_properties<std::stack<T, C>>
{
    static consteval std::string name()
    {
        return "std::stack<" + detail::canonical_name<T>() + ">";
    }
};
template<class A, class B> struct type_properties<std::pair<A, B>>
{
    static consteval std::string name()
    {
        return "std::pair<" + detail::canonical_list<A, B>() + ">";
    }
};
template<class... Es> struct type_properties<std::tuple<Es...>>
{
    static consteval std::string name()
    {
        return "std::tuple<" + detail::canonical_list<Es...>() + ">";
    }
};
template<> struct type_properties<std::monostate>
{
    static consteval std::string name()
    {
        return "std::monostate";
    }
};

namespace detail
{
// ---- field construction ------------------------------------------------------------------------------
template<class T> consteval std::uint64_t schema_hash_of();
template<class T> consteval std::uint64_t layout_hash_of();
// Memoised per type (cc F9): nested references reuse these instead of re-deriving the canonical strings.
template<class T>
ES_SERREG_HIDDEN inline constexpr std::uint64_t schema_hash_memo = schema_hash_of<T>();
template<class T>
ES_SERREG_HIDDEN inline constexpr std::uint64_t layout_hash_memo = layout_hash_of<T>();

// Element descriptor for arrays (recursive: arrays of arrays), lives in .rodata.
template<class E> consteval field_desc element_field();
template<class E> ES_SERREG_HIDDEN inline constexpr field_desc element_field_v = element_field<E>();
// v2 (defined with the canonical layout below): image geometry and canonical offsets of dynamic classes.
template<class M> consteval std::uint32_t image_size();
template<class M> consteval std::uint32_t image_align();
template<class T, std::size_t I> consteval std::uint32_t member_image_offset();
template<class T, std::size_t I> consteval std::uint32_t base_image_offset();
template<class V, std::size_t... I> consteval auto make_variant_alts(std::index_sequence<I...>)
{
    return std::array<field_desc, sizeof...(I)>{
        element_field<std::variant_alternative_t<I, V>>()...};
}
template<class V>
ES_SERREG_HIDDEN inline constexpr auto variant_alts_v =
    make_variant_alts<V>(std::make_index_sequence<dyn_member<V>::count>{});

constexpr std::uint64_t align_up(const std::uint64_t n, const std::uint64_t a) noexcept
{
    return (n + a - 1) / a * a;
}

// 1.6: one (key, value) entry of a map block is laid out like a two-member dynamic class: the key at 0, the value
// at the first offset its alignment allows, the entry padded to the larger alignment. A set's entry is its key.
struct entry_layout
{
    std::uint32_t value_offset, size, align;
};
template<class K, class V> consteval entry_layout map_entry_layout()
{
    const std::uint32_t ka = image_align<K>(), va = image_align<V>();
    const std::uint32_t al = ka > va ? ka : va;
    const std::uint32_t vo = static_cast<std::uint32_t>(align_up(image_size<K>(), va));
    return {vo, static_cast<std::uint32_t>(align_up(vo + image_size<V>(), al)), al};
}
template<class M> consteval entry_layout container_entry_layout()
{
    using U = mapped_t<M>;
    if constexpr (dyn_member<U>::k == kind::map_)
        return map_entry_layout<typename dyn_member<U>::key, typename dyn_member<U>::elem>();
    else
        return {0, image_stride<typename dyn_member<U>::key>(),
                image_align<typename dyn_member<U>::key>()};
}
template<class M> consteval std::array<field_desc, 2> make_map_elems();
template<class M> ES_SERREG_HIDDEN inline constexpr auto map_elems_v = make_map_elems<M>();

template<class M>
consteval field_desc field_for(const std::string_view name, const std::uint32_t offset,
                               const std::uint16_t since_v, const std::uint16_t deprecated_v)
{
    using U = mapped_t<M>;
    check_supported<M>();
    static_assert(!has_as_flat<std::remove_cv_t<M>>,
                  "es::serreg: as_flat<T> applies to top-level arguments only in v1; a nested "
                  "member of a mapped type "
                  "would be copied unconverted. Provide as_flat for the enclosing type instead.");
    field_desc f{};
    f.name = name;
    f.offset = offset;
    f.size =
        image_size<U>(); // native size for flat members; slot / fixed-part size for dynamic ones
    f.align = static_cast<std::uint16_t>(image_align<U>());
    f.since = since_v;
    f.deprecated = deprecated_v;
    if constexpr (is_scalar_v<U>)
    {
        f.k = scalar_kind<U>();
    }
    else if constexpr (std::is_enum_v<U>)
    {
        f.k = kind::enum_;
        f.type_hash = schema_hash_memo<U>;
    }
    else if constexpr (array_traits<U>::is_array)
    {
        f.k = kind::array_;
        f.extent = static_cast<std::uint32_t>(array_traits<U>::extent);
        f.elem = &element_field_v<typename array_traits<U>::elem>;
    }
    else if constexpr (is_dyn_member_v<U>)
    { // v2: reference slot {u32 off, u32 n}; the element descriptor(s) describe the tail block
        constexpr kind k = dyn_member<U>::k;
        f.k = k;
        if constexpr (k == kind::vector_ || k == kind::optional_)
            f.elem = &element_field_v<typename dyn_member<U>::elem>;
        else if constexpr (k == kind::variant_)
        {
            f.extent = static_cast<std::uint32_t>(dyn_member<U>::count);
            f.elem = variant_alts_v<U>.data();
        }
        else if constexpr (k == kind::map_)
        { // 1.6: elem[0] the key at entry offset 0, elem[1] the value at its canonical entry offset; `extent`
            // carries the entry stride so a reader lays the block out without recomputing it
            f.extent = container_entry_layout<U>().size;
            f.elem = map_elems_v<U>.data();
        }
        else if constexpr (k == kind::set_)
        {
            f.extent = container_entry_layout<U>().size;
            f.elem = &element_field_v<typename dyn_member<U>::key>;
        }
    }
    else
    {
        f.k = kind::struct_;
        f.type_hash = schema_hash_memo<U>;
    }
    return f;
}

template<class E> consteval field_desc element_field()
{
    return field_for<E>({}, 0, 0, 0);
}
template<class M> consteval std::array<field_desc, 2> make_map_elems()
{
    using U = mapped_t<M>;
    field_desc key = field_for<typename dyn_member<U>::key>({}, 0, 0, 0);
    field_desc value =
        field_for<typename dyn_member<U>::elem>({}, container_entry_layout<U>().value_offset, 0, 0);
    return {key, value};
}
// The offset a field is described with: the native offsetof for flat classes, the canonical image offset for
// dynamic classes (plan v2.0 §2).
template<class T, std::size_t I> consteval std::uint32_t member_offset()
{
    if constexpr (is_dynamic<T>())
        return member_image_offset<T, I>();
    else
        return static_cast<std::uint32_t>(std::meta::offset_of(members<T>()[I]).bytes);
}
template<class T, std::size_t I> consteval std::uint32_t base_offset()
{
    if constexpr (is_dynamic<T>())
        return base_image_offset<T, I>();
    else
        return static_cast<std::uint32_t>(std::meta::offset_of(bases<T>()[I]).bytes);
}

template<class T, std::size_t I> consteval field_desc member_field()
{
    if constexpr (!is_tuple_like_v<T>)
        static_assert(!std::meta::is_bit_field(members<T>()[I]),
                      "es::serreg: bit-field members are not supported (use as_flat<T>)");
    // 2.2.6 (review B4): a reader memcpy's / assigns into the member, which is undefined for a const object and
    // not what a volatile asks for. Only members that are written are instantiated here: a [[=skip]] member
    // (never written) and a `mutable` member (writable through a const object) pass.
    static_assert(
        !std::is_const_v<part_t<T, I>> && !std::is_volatile_v<part_t<T, I>>,
        "es::serreg: const and volatile members are not serialisable; a reader would have to "
        "modify a const object; mark it [[=skip]] or remove the qualifier");
    return field_for<part_t<T, I>>(std::define_static_string(part_name<T, I>()),
                                   member_offset<T, I>(), part_annotation<since, T, I>(),
                                   part_annotation<deprecated, T, I>());
}

template<class T, std::size_t I> consteval field_desc base_field()
{
    constexpr std::meta::info b = bases<T>()[I];
    static_assert(!std::meta::is_virtual(b), "es::serreg: virtual base classes are not supported");
    using B = typename[:std::meta::type_of(b):];
    std::string n = "<base:";
    n += canonical_name<B>();
    n += '>';
    field_desc f = field_for<B>(std::define_static_string(n), base_offset<T, I>(), 0, 0);
    return f;
}

template<class T, std::size_t I> consteval bool member_skipped()
{
    if constexpr (is_tuple_like_v<T>)
        return false;
    else
        return has_annotation<skip_t>(members<T>()[I]);
}
template<class A, class T, std::size_t I> consteval std::uint16_t part_annotation()
{
    if constexpr (is_tuple_like_v<T>)
        return 0;
    else
        return annotation_value<A>(members<T>()[I]);
}

template<class T, std::size_t... I>
consteval std::size_t kept_member_count(std::index_sequence<I...>)
{
    return (std::size_t{0} + ... + (member_skipped<T, I>() ? 0 : 1));
}

template<class T> consteval std::size_t field_count()
{
    return base_count<T>() + kept_member_count<T>(std::make_index_sequence<part_count<T>()>{});
}

template<class T, std::size_t I, std::size_t N>
consteval void put_member_field(std::array<field_desc, N>& out, std::size_t& n)
{
    if constexpr (!member_skipped<T, I>())
        out[n++] = member_field<T, I>(); // skipped members are never instantiated
}
template<class T, std::size_t... B, std::size_t... M>
consteval auto make_fields_impl(std::index_sequence<B...>, std::index_sequence<M...>)
{
    std::array<field_desc, field_count<T>()> out{};
    std::size_t n = 0;
    ((out[n++] = base_field<T, B>()), ...);
    (put_member_field<T, M>(out, n), ...);
    static_cast<void>(n);
    static_assert(field_count<T>() <= 0xFFFF,
                  "es::serreg: more than 65535 fields do not fit the dictionary encoding");
    return out;
}
template<class T> consteval auto make_fields()
{
    return make_fields_impl<T>(std::make_index_sequence<base_count<T>()>{},
                               std::make_index_sequence<part_count<T>()>{});
}
template<class T> ES_SERREG_HIDDEN inline constexpr auto fields_v = make_fields<T>();

// ---- dynamic types: detection and the canonical image layout (v2, plan v2.0 §2) ----------------------------
template<class T, std::size_t I> consteval bool member_dynamic()
{
    if constexpr (member_skipped<T, I>())
        return false;
    else
        return is_dynamic<part_t<T, I>>();
}
template<class T> consteval bool is_dynamic()
{
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U> || std::is_enum_v<U>)
        return false;
    else if constexpr (array_traits<U>::is_array)
        return is_dynamic<typename array_traits<U>::elem>();
    else if constexpr (is_dyn_member_v<U> || is_tuple_like_v<U>)
        return true;
    else if constexpr (std::is_class_v<U>)
    {
        return [&]<std::size_t... B, std::size_t... M>(std::index_sequence<B...>,
                                                       std::index_sequence<M...>) {
            return (false || ... || is_dynamic<base_t<U, B>>()) ||
                   (false || ... || member_dynamic<U, M>());
        }(std::make_index_sequence<base_count<U>()>{}, std::make_index_sequence<part_count<U>()>{});
    }
    else
        return false;
}

// 2.2.6 (review B1): does writing a T sort anything? True when T — or any base, member, array element, container
// element, key, mapped type or variant alternative, recursively — is an unordered container (sorted by key when
// serialize_options::canonical_unordered, the default) or a multimap shape sorted by key. That sort builds its
// order table on the heap above 64 entries, so serialize() of such a value is not noexcept whatever the sink
// (serialize.hpp: serialize_noexcept). canonical_unordered = false skips the sort at run time; the trait is a
// compile-time bound and stays true.
template<class T> consteval bool may_sort();
template<class T, std::size_t I> consteval bool member_may_sort()
{
    if constexpr (member_skipped<T, I>())
        return false; // a skipped member is never written, whatever its type
    else
        return may_sort<part_t<T, I>>();
}
template<class T> consteval bool may_sort()
{
    using U = mapped_t<T>;
    if constexpr (is_scalar_v<U> || std::is_enum_v<U>)
        return false;
    else if constexpr (array_traits<U>::is_array)
        return may_sort<typename array_traits<U>::elem>();
    else if constexpr (is_dyn_member_v<U>)
    {
        constexpr kind k = dyn_member<U>::k;
        if constexpr (k == kind::string_)
            return false;
        else if constexpr (k == kind::vector_)
        {
            if constexpr (is_multimap_member<U>())
                if (dyn_member<U>::sort_by_key)
                    return true;
            return may_sort<typename dyn_member<U>::elem>();
        }
        else if constexpr (k == kind::optional_)
            return may_sort<typename dyn_member<U>::elem>();
        else if constexpr (k == kind::variant_)
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                return (false || ... || may_sort<std::variant_alternative_t<I, U>>());
            }(std::make_index_sequence<dyn_member<U>::count>{});
        else
        { // map_ / set_
            if (!dyn_member<U>::ordered)
                return true;
            if constexpr (k == kind::map_)
                return may_sort<typename dyn_member<U>::key>() ||
                       may_sort<typename dyn_member<U>::elem>();
            else
                return may_sort<typename dyn_member<U>::key>();
        }
    }
    else if constexpr (std::is_class_v<U>)
    {
        return [&]<std::size_t... B, std::size_t... M>(std::index_sequence<B...>,
                                                       std::index_sequence<M...>) {
            return (false || ... || may_sort<base_t<U, B>>()) ||
                   (false || ... || member_may_sort<U, M>());
        }(std::make_index_sequence<base_count<U>()>{}, std::make_index_sequence<part_count<U>()>{});
    }
    else
        return false;
}
template<class T> constexpr bool may_sort_v = may_sort<T>();

inline constexpr std::uint32_t ref_slot_size = fmt::slot_size; // {u32 off, u32 n}
inline constexpr std::uint32_t ref_slot_align = 4;

// Fixed-part geometry of a dynamic class: canonical offsets (declaration order, bases first, natural
// alignment of the members' image sizes), not the native offsetof.
struct fixed_layout
{
    std::uint32_t size, align;
};
template<class T> consteval fixed_layout compute_fixed_layout();
template<class T>
ES_SERREG_HIDDEN inline constexpr fixed_layout fixed_layout_v = compute_fixed_layout<T>();

// Size and alignment of one value's image inside a fixed part (or of one element inside a vector block).
template<class M> consteval std::uint32_t image_size()
{
    using U = mapped_t<M>;
    if constexpr (is_dyn_member_v<U>)
        return ref_slot_size;
    else if constexpr (array_traits<U>::is_array && is_dynamic<U>())
        return static_cast<std::uint32_t>(array_traits<U>::extent *
                                          align_up(image_size<typename array_traits<U>::elem>(),
                                                   image_align<typename array_traits<U>::elem>()));
    else if constexpr (is_dynamic_class<U>())
        return fixed_layout_v<U>.size;
    else
        return static_cast<std::uint32_t>(sizeof(U));
}
template<class M> consteval std::uint32_t image_align()
{
    using U = mapped_t<M>;
    if constexpr (is_dyn_member_v<U>)
        return ref_slot_align;
    else if constexpr (array_traits<U>::is_array && is_dynamic<U>())
        return image_align<typename array_traits<U>::elem>();
    else if constexpr (is_dynamic_class<U>())
        return fixed_layout_v<U>.align;
    else
        return static_cast<std::uint32_t>(alignof(U));
}
// Element stride inside a vector block / dynamic array.
template<class E> consteval std::uint32_t image_stride()
{
    return static_cast<std::uint32_t>(align_up(image_size<E>(), image_align<E>()));
}

// Canonical offset of base B / kept member I of a dynamic class T.
template<class T, std::size_t... B, std::size_t... M>
consteval std::array<std::uint32_t, sizeof...(B) + sizeof...(M)> compute_canonical_offsets(
    std::index_sequence<B...>, std::index_sequence<M...>)
{
    std::array<std::uint32_t, sizeof...(B) + sizeof...(M)> out{};
    std::uint64_t cur = 0;
    std::size_t i = 0;
    const auto place = [&](const std::uint32_t sz, const std::uint32_t al) {
        cur = align_up(cur, al);
        out[i++] = static_cast<std::uint32_t>(cur);
        cur += sz;
    };
    (place(image_size<base_t<T, B>>(), image_align<base_t<T, B>>()), ...);
    ((member_skipped<T, M>() ? (out[i++] = 0, void())
                             : place(image_size<part_t<T, M>>(), image_align<part_t<T, M>>())),
     ...);
    return out;
}
template<class T>
ES_SERREG_HIDDEN inline constexpr auto canonical_offsets_v = compute_canonical_offsets<T>(
    std::make_index_sequence<base_count<T>()>{}, std::make_index_sequence<part_count<T>()>{});
template<class T, std::size_t I> consteval std::uint32_t base_image_offset()
{
    return canonical_offsets_v<T>[I];
}
template<class T, std::size_t I> consteval std::uint32_t member_image_offset()
{
    return canonical_offsets_v<T>[base_count<T>() + I];
}
template<class T> consteval fixed_layout compute_fixed_layout()
{
    std::uint64_t end = 0, al = 1;
    [&]<std::size_t... B, std::size_t... M>(std::index_sequence<B...>, std::index_sequence<M...>) {
        const auto take = [&](const std::uint32_t off, const std::uint32_t sz,
                              const std::uint32_t a) {
            if (off + sz > end)
                end = off + sz;
            if (a > al)
                al = a;
        };
        (take(base_image_offset<T, B>(), image_size<base_t<T, B>>(), image_align<base_t<T, B>>()),
         ...);
        ((member_skipped<T, M>() ? void()
                                 : take(member_image_offset<T, M>(), image_size<part_t<T, M>>(),
                                        image_align<part_t<T, M>>())),
         ...);
    }(std::make_index_sequence<base_count<T>()>{}, std::make_index_sequence<part_count<T>()>{});
    return {static_cast<std::uint32_t>(align_up(end, al)), static_cast<std::uint32_t>(al)};
}

// ---- enumerators -------------------------------------------------------------------------------------
template<class E> consteval bool enumerators_fit()
{
    bool ok = enumerators<E>().size() <= 0xFFFF;
    template for (constexpr std::meta::info e : enumerators<E>())
    {
        const auto v = std::to_underlying([:e:]);
        if constexpr (std::is_unsigned_v<decltype(v)>)
        {
            if (static_cast<std::uint64_t>(v) > static_cast<std::uint64_t>(INT64_MAX))
                ok = false;
        }
        if (std::meta::identifier_of(e).size() > 0xFFFF)
            ok = false;
    }
    return ok;
}
template<class E, std::size_t... I> consteval auto make_enumerators_impl(std::index_sequence<I...>)
{
    static_assert(enumerators_fit<E>(), "es::serreg: enumerator values must fit int64_t and "
                                        "names/counts must fit uint16_t (dictionary encoding)");
    return std::array<enumerator_desc, sizeof...(I)>{
        enumerator_desc{std::define_static_string(std::meta::identifier_of(enumerators<E>()[I])),
                        static_cast<std::int64_t>(std::to_underlying([:enumerators<E>()[I]:]))}...};
}
template<class E> consteval auto make_enumerators()
{
    return make_enumerators_impl<E>(std::make_index_sequence<enumerators<E>().size()>{});
}
template<class E> ES_SERREG_HIDDEN inline constexpr auto enumerators_v = make_enumerators<E>();

// 2.1.16 (decision D): the underlying kind of an enum in every canonical string and descriptor.
// An enum with a fixed underlying type (`enum class`, `enum E : T`) has the kind of that type. An unscoped enum
// *without* one (`enum Color { red, green };`) has an implementation-defined underlying type — GCC and Clang pick
// `unsigned int` unless an enumerator is negative, MSVC picks `int` — so its kind is derived from the source
// instead of the compiler's trait: the unsigned kind of sizeof(E), or the signed kind when an enumerator is
// negative. On GCC that is what std::underlying_type_t gives, so no hash moved; on a compiler that chooses
// `int` the canonical strings now agree with GCC's. Detection: list-initialisation from an int is well-formed only
// for an enum with a fixed underlying type.
template<class E> constexpr bool has_fixed_underlying_type_v = requires { E{0}; };
template<class E> consteval bool has_negative_enumerator()
{
    for (const auto& e : enumerators_v<E>)
        if (e.value < 0)
            return true;
    return false;
}
template<class E> consteval kind enum_underlying_kind()
{
    if constexpr (has_fixed_underlying_type_v<E>)
        return scalar_kind<std::underlying_type_t<E>>();
    else
    {
        constexpr bool neg = has_negative_enumerator<E>();
        switch (sizeof(E))
        {
        case 1:
            return neg ? kind::i8 : kind::u8;
        case 2:
            return neg ? kind::i16 : kind::u16;
        case 4:
            return neg ? kind::i32 : kind::u32;
        default:
            return neg ? kind::i64 : kind::u64;
        }
    }
}

// ---- canonical strings (§6.2) ------------------------------------------------------------------------
// kindspec for a field: scalar kind | enum<name,u8> (schema) / enum<u8> (layout) | array<kindspec,N> | struct<0x..> / struct<L:0x..>
template<class M> consteval void append_kindspec(std::string& s, const bool schema)
{
    using U = mapped_t<M>;
    if constexpr (is_scalar_v<U>)
        s += to_string(scalar_kind<U>());
    else if constexpr (std::is_enum_v<U>)
    {
        s += "enum<";
        if (schema)
        {
            s += canonical_name<U>();
            s += ',';
        }
        s += to_string(enum_underlying_kind<U>()); // 2.1.16 (decision D)
        s += '>';
    }
    else if constexpr (array_traits<U>::is_array)
    {
        s += "array<";
        append_kindspec<typename array_traits<U>::elem>(s, schema);
        s += ',';
        append_uint(s, array_traits<U>::extent);
        s += '>';
    }
    else if constexpr (is_dyn_member_v<U>)
    { // v2: string | vector<E> | optional<E> | variant<E0|E1|…>
        constexpr kind k = dyn_member<U>::k;
        if constexpr (k == kind::string_)
            s += "string";
        else if constexpr (k == kind::vector_ && is_multimap_member<U>())
        { // the schema names the shape; the layout string is the vector of pairs it is
            using P = typename dyn_member<U>::elem;
            if (schema)
            {
                s += "multimap<";
                append_kindspec<typename P::first_type>(s, schema);
                s += ',';
                append_kindspec<typename P::second_type>(s, schema);
            }
            else
            {
                s += "vector<";
                append_kindspec<P>(s, schema);
            }
            s += '>';
        }
        else if constexpr (k == kind::vector_ || k == kind::optional_)
        {
            s += k == kind::vector_ ? "vector<" : "optional<";
            append_kindspec<typename dyn_member<U>::elem>(s, schema);
            s += '>';
        }
        else if constexpr (k == kind::variant_)
        {
            s += "variant<";
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((I ? s += '|' : s, append_kindspec<std::variant_alternative_t<I, U>>(s, schema)),
                 ...);
            }(std::make_index_sequence<dyn_member<U>::count>{});
            s += '>';
        }
        else if constexpr (k == kind::map_)
        { // 1.6: map<K,V> — the shape, whatever the C++ container
            s += "map<";
            append_kindspec<typename dyn_member<U>::key>(s, schema);
            s += ',';
            append_kindspec<typename dyn_member<U>::elem>(s, schema);
            s += '>';
        }
        else
        {
            s += "set<";
            append_kindspec<typename dyn_member<U>::key>(s, schema);
            s += '>';
        }
    }
    else
    {
        if constexpr (is_dynamic_class<U>())
            s += schema ? "dyn<" : "dyn<L:";
        else
            s += schema ? "struct<" : "struct<L:";
        append_hex(s, schema ? schema_hash_memo<U> : layout_hash_memo<U>);
        s += '>';
    }
}

consteval void append_dims(std::string& s, const std::uint64_t size, const std::uint64_t align,
                           const std::uint16_t since_v, const std::uint16_t deprecated_v,
                           const bool schema)
{
    s += '[';
    append_uint(s, size);
    s += ',';
    append_uint(s, align);
    if (schema && (since_v || deprecated_v))
    {
        s += ";s";
        append_uint(s, since_v);
        s += ";d";
        append_uint(s, deprecated_v);
    }
    s += ']';
}

template<class T, std::size_t I> consteval void append_member(std::string& s, const bool schema)
{
    if constexpr (!member_skipped<T, I>())
    {
        using M = part_t<T, I>;
        if (schema)
        {
            s += part_name<T, I>();
            s += ':';
        }
        append_kindspec<M>(s, schema);
        s += '@';
        append_uint(s, member_offset<T, I>());
        append_dims(s, image_size<M>(), image_align<M>(), part_annotation<since, T, I>(),
                    part_annotation<deprecated, T, I>(), schema);
        s += ';';
    }
}
template<class T, std::size_t I> consteval void append_base(std::string& s, const bool schema)
{
    constexpr std::meta::info b = bases<T>()[I];
    using B = typename[:std::meta::type_of(b):];
    if (schema)
    {
        s += "<base:";
        s += canonical_name<B>();
        s += ">:";
    }
    append_kindspec<B>(s, schema);
    s += '@';
    append_uint(s, base_offset<T, I>());
    append_dims(s, image_size<B>(), image_align<B>(), 0, 0, schema);
    s += ';';
}

template<class T, std::size_t... B, std::size_t... M>
consteval std::string struct_string(const bool schema, std::index_sequence<B...>,
                                    std::index_sequence<M...>)
{
    std::string s =
        is_dynamic<T>() ? "dyn" : "struct"; // a flat and a dynamic definition never collide
    if (schema)
    {
        s += '<';
        s += canonical_name<T>();
        s += '>';
    }
    append_dims(s, image_size<T>(), image_align<T>(),
                annotation_value<since>(std::meta::dealias(^^T)),
                annotation_value<deprecated>(std::meta::dealias(^^T)), schema);
    s += '{';
    (append_base<T, B>(s, schema), ...);
    (append_member<T, M>(s, schema), ...);
    s += '}';
    return s;
}

template<class E> consteval std::string enum_string(const bool schema)
{
    std::string s = "enum";
    if (schema)
    {
        s += '<';
        s += canonical_name<E>();
        s += ',';
    }
    else
        s += '<';
    s += to_string(enum_underlying_kind<E>()); // 2.1.16 (decision D)
    s += '>';
    append_dims(s, sizeof(E), alignof(E), annotation_value<since>(std::meta::dealias(^^E)),
                annotation_value<deprecated>(std::meta::dealias(^^E)), schema);
    if (schema)
    {
        s += '{';
        for (const auto& e : enumerators_v<E>)
        {
            s += e.name;
            s += '=';
            append_int(s, e.value);
            s += ';';
        }
        s += '}';
    }
    return s;
}

// 1.6: the synthetic type of a container argument: a dynamic class with one unnamed field — the container — at 0.
template<class U> consteval std::string wrapper_string(const bool schema)
{
    std::string s = "dyn";
    if (schema)
    {
        s += '<';
        s += canonical_name<U>();
        s += '>';
    }
    append_dims(s, image_size<U>(), image_align<U>(), 0, 0, schema);
    s += '{';
    if (schema)
        s += ':';
    append_kindspec<U>(s, schema);
    s += "@0";
    append_dims(s, image_size<U>(), image_align<U>(), 0, 0, schema);
    s += ";}";
    return s;
}
template<class U> consteval std::array<field_desc, 1> make_wrapper_fields()
{
    return {field_for<U>({}, 0, 0, 0)};
}
template<class U>
ES_SERREG_HIDDEN inline constexpr auto wrapper_fields_v = make_wrapper_fields<U>();

template<class T> consteval std::string canonical_string(const bool schema)
{
    using U = mapped_t<T>;
    if constexpr (std::is_enum_v<U>)
        return enum_string<U>(schema);
    else if constexpr (is_container_arg_v<U>)
    {
        check_supported<U>();
        return wrapper_string<U>(schema);
    }
    else
    {
        static_assert(std::is_class_v<U>,
                      "es::serreg: descriptors exist for class and enum types only");
        check_supported<U>();
        return struct_string<U>(schema, std::make_index_sequence<base_count<U>()>{},
                                std::make_index_sequence<part_count<U>()>{});
    }
}

template<class T> consteval std::uint64_t schema_hash_of()
{
    return fingerprint64(canonical_string<T>(true));
}
template<class T> consteval std::uint64_t layout_hash_of()
{
    return fingerprint64(canonical_string<T>(false));
}

// ---- padding gaps (FR-S6) ----------------------------------------------------------------------------
// Coverage is collected as [offset, offset+len) intervals of leaf scalars (one interval per array whose element
// type has no internal padding), merged, and complemented over [0, sizeof(T)). No per-byte work, so large
// buffer members do not hit the constexpr operation limit.
// Leaf walk shared by the gap table (FR-S6) and the bool table (gk 1.7): `out` receives the covered
// [offset, offset+len) intervals, `bools` the offsets of every bool leaf. Skipped members are neither
// covered nor instantiated (cc F4), so their bytes are scrubbed like padding.
template<class U, std::size_t I>
consteval void collect_member_leaves(std::vector<gap>& out, std::vector<std::uint32_t>& bools,
                                     std::size_t base);

template<class M>
consteval void collect_leaves(std::vector<gap>& out, std::vector<std::uint32_t>& bools,
                              const std::size_t base)
{
    using U = mapped_t<M>;
    if constexpr (is_scalar_v<U> || std::is_enum_v<U>)
    {
        out.push_back(gap{static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(sizeof(U))});
        if constexpr (std::is_same_v<U, bool>)
            bools.push_back(static_cast<std::uint32_t>(base));
    }
    else if constexpr (array_traits<U>::is_array)
    {
        using E = typename array_traits<U>::elem;
        if constexpr ((std::has_unique_object_representations_v<E> || is_scalar_v<E>) &&
                      !std::is_same_v<std::remove_cv_t<E>, bool>)
            out.push_back(
                gap{static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(sizeof(U))});
        else
            for (std::size_t i = 0; i < array_traits<U>::extent; ++i)
                collect_leaves<E>(out, bools, base + i * sizeof(E));
    }
    else
    {
        [&]<std::size_t... B, std::size_t... Mi>(std::index_sequence<B...>,
                                                 std::index_sequence<Mi...>) {
            (collect_leaves<typename[:std::meta::type_of(bases<U>()[B]):]>(
                 out, bools,
                 base + static_cast<std::size_t>(std::meta::offset_of(bases<U>()[B]).bytes)),
             ...);
            (collect_member_leaves<U, Mi>(out, bools, base), ...);
        }(std::make_index_sequence<bases<U>().size()>{},
          std::make_index_sequence<members<U>().size()>{});
    }
}
template<class U, std::size_t I>
consteval void collect_member_leaves(std::vector<gap>& out, std::vector<std::uint32_t>& bools,
                                     const std::size_t base)
{
    if constexpr (!member_skipped<U, I>())
        collect_leaves<typename[:std::meta::type_of(members<U>()[I]):]>(
            out, bools,
            base + static_cast<std::size_t>(std::meta::offset_of(members<U>()[I]).bytes));
}
template<class T> consteval std::vector<gap> compute_gaps()
{
    std::vector<gap> iv;
    std::vector<std::uint32_t> bools;
    collect_leaves<T>(iv, bools, 0);
    std::sort(iv.begin(), iv.end(), [](const gap a, const gap b) { return a.offset < b.offset; });
    std::vector<gap> out;
    std::size_t pos = 0;
    for (const gap g : iv)
    {
        if (g.offset > pos)
            out.push_back(
                gap{static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(g.offset - pos)});
        if (g.offset + g.len > pos)
            pos = g.offset + g.len;
    }
    if (pos < sizeof(T))
        out.push_back(
            gap{static_cast<std::uint32_t>(pos), static_cast<std::uint32_t>(sizeof(T) - pos)});
    return out;
}
template<class T> consteval std::size_t gap_count()
{
    return compute_gaps<T>().size();
}
template<class T> consteval auto make_gaps()
{
    std::array<gap, gap_count<T>()> out{};
    const auto v = compute_gaps<T>();
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = v[i];
    return out;
}
template<class T> ES_SERREG_HIDDEN inline constexpr auto gaps_v = make_gaps<T>();

// bool leaves: normalised to 0/1 when scrubbing (gk 1.7 — bool has no unique object representation).
template<class T> consteval std::vector<std::uint32_t> compute_bools()
{
    std::vector<gap> iv;
    std::vector<std::uint32_t> bools;
    collect_leaves<T>(iv, bools, 0);
    return bools;
}
template<class T> consteval std::size_t bool_count()
{
    return compute_bools<T>().size();
}
template<class T> consteval auto make_bools()
{
    std::array<std::uint32_t, bool_count<T>()> out{};
    const auto v = compute_bools<T>();
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = v[i];
    return out;
}
template<class T> ES_SERREG_HIDDEN inline constexpr auto bools_v = make_bools<T>();

// ---- FR-D5: enum leaves whose values are bounded (read-side value validation) --------------------------------
// An enum *without* a fixed underlying type holds only the values of the minimal-width integer that represents
// every enumerator ([dcl.enum]): reading an object of it that holds another value is undefined behaviour. With
// ES_SERREG_VALIDATE_READ=1 the readers check such leaves (members, array elements, top-level arguments) when
// they copy a flat value: a leaf outside its range is set to 0 — always inside the range — and the read is
// reported as error::invalid_value (the rest of the frame is still decoded, as for a valueless variant, G2-6).
// Enums with a fixed underlying type (`enum class`, `enum E : T`) hold every value of T and are never checked.
// The check is a compile-time policy of the translation unit, like ES_SERREG_LOG_MIN_LEVEL: 0 (the default) is
// off — the tables are not even instantiated and the readers copy the bytes as they are; 1 is "report".
#ifndef ES_SERREG_VALIDATE_READ
#define ES_SERREG_VALIDATE_READ 0
#endif
inline constexpr bool validate_read_enums = ES_SERREG_VALIDATE_READ != 0;
struct enum_range
{
    std::uint32_t offset;
    std::uint32_t size;
    bool is_signed;
    std::int64_t lo, hi;
};
template<class E> consteval enum_range enum_range_of(const std::size_t offset)
{
    std::int64_t mn = 0, mx = 0;
    bool first = true;
    for (const auto& e : enumerators_v<E>)
    {
        mn = first ? e.value : std::min(mn, e.value);
        mx = first ? e.value : std::max(mx, e.value);
        first = false;
    }
    const unsigned width = 8 * sizeof(E);
    enum_range r{static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(sizeof(E)), mn < 0,
                 0, 0};
    const auto bits_for = [](const std::uint64_t v) { // bits of the magnitude
        unsigned b = 0;
        for (std::uint64_t x = v; x != 0; x >>= 1)
            ++b;
        return b;
    };
    if (!r.is_signed)
    {
        unsigned m = std::max(1u, bits_for(static_cast<std::uint64_t>(mx)));
        m = std::min(m, width);
        r.lo = 0;
        r.hi = m >= 63 ? INT64_MAX : static_cast<std::int64_t>((std::uint64_t{1} << m) - 1);
    }
    else
    { // two's complement: enough bits for the sign, the largest positive and the most negative value
        unsigned m = std::max(bits_for(static_cast<std::uint64_t>(mx < 0 ? 0 : mx)) + 1,
                              bits_for(static_cast<std::uint64_t>(-(mn + 1))) + 1);
        m = std::min(std::max(m, 1u), width);
        r.lo = m >= 64 ? INT64_MIN : -(std::int64_t{1} << (m - 1));
        r.hi = m >= 64 ? INT64_MAX : (std::int64_t{1} << (m - 1)) - 1;
    }
    return r;
}
template<class U, std::size_t I>
consteval void collect_member_enum_ranges(std::vector<enum_range>&, std::size_t);
template<class T> consteval std::size_t enum_range_count();
template<class M>
consteval void collect_enum_ranges(std::vector<enum_range>& out, const std::size_t base)
{
    using U = mapped_t<M>;
    if constexpr (std::is_enum_v<U>)
    {
        if constexpr (!has_fixed_underlying_type_v<U>)
            out.push_back(enum_range_of<U>(base));
    }
    else if constexpr (is_scalar_v<U>)
    {
    }
    else if constexpr (array_traits<U>::is_array)
    {
        using E = typename array_traits<U>::elem;
        if constexpr (enum_range_count<E>() !=
                      0) // a 1 MiB buffer of bytes must not iterate per element
            for (std::size_t i = 0; i < array_traits<U>::extent; ++i)
                collect_enum_ranges<E>(out, base + i * sizeof(E));
    }
    else if constexpr (std::is_class_v<U> && !is_dyn_member_v<U>)
    {
        [&]<std::size_t... B, std::size_t... Mi>(std::index_sequence<B...>,
                                                 std::index_sequence<Mi...>) {
            (collect_enum_ranges<typename[:std::meta::type_of(bases<U>()[B]):]>(
                 out, base + static_cast<std::size_t>(std::meta::offset_of(bases<U>()[B]).bytes)),
             ...);
            (collect_member_enum_ranges<U, Mi>(out, base), ...);
        }(std::make_index_sequence<bases<U>().size()>{},
          std::make_index_sequence<members<U>().size()>{});
    }
}
template<class U, std::size_t I>
consteval void collect_member_enum_ranges(std::vector<enum_range>& out, const std::size_t base)
{
    if constexpr (!member_skipped<U, I>())
        collect_enum_ranges<typename[:std::meta::type_of(members<U>()[I]):]>(
            out, base + static_cast<std::size_t>(std::meta::offset_of(members<U>()[I]).bytes));
}
template<class T> consteval std::size_t enum_range_count()
{
    std::vector<enum_range> v;
    collect_enum_ranges<T>(v, 0);
    return v.size();
}
template<class T> consteval auto make_enum_ranges()
{
    std::array<enum_range, enum_range_count<T>()> out{};
    std::vector<enum_range> v;
    collect_enum_ranges<T>(v, 0);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = v[i];
    return out;
}
template<class T> ES_SERREG_HIDDEN inline constexpr auto enum_ranges_v = make_enum_ranges<T>();
// The leaf at p, read as the enum's integer, is inside its range.
inline bool enum_leaf_in_range(const enum_range& r, const std::byte* const p) noexcept
{
    std::uint64_t u = 0;
    std::memcpy(&u, p, r.size); // little-endian host (format.hpp)
    std::int64_t v;
    if (r.is_signed && r.size < 8)
    {
        const unsigned shift = 64 - 8 * r.size;
        v = static_cast<std::int64_t>(u << shift) >> shift; // sign-extend
    }
    else
        v = static_cast<std::int64_t>(u);
    if (!r.is_signed && r.size == 8 && u > static_cast<std::uint64_t>(INT64_MAX))
        return false; // above every enumerator a u64 enum can name (values fit int64 by construction)
    return v >= r.lo && v <= r.hi;
}

// ---- dynamic images: measure / encode / decode (v2, plan v2.0 §2, §4) --------------------------------------
// The three walks below mirror each other exactly. Reflection is used to reach every member (private ones
// included, via splices) — which is why they live in this header.
inline constexpr int max_image_depth = fmt::max_nesting;

// zero-fill + scrub of one flat object image (the same bytes the 1.0 flat path produces). 2.1.13: a C array whose
// elements are classes or bools is scrubbed element by element, as a top-level record's gap and bool tables do (until
// 2.1.12 it was copied as is inside dynamic images, padding bytes included); std::array is a class, so its own tables
// already covered it.
template<class U> inline void put_flat_image(std::byte* const dst, const U& v) noexcept
{
    if constexpr (std::is_array_v<U>)
    {
        using E = std::remove_extent_t<U>;
        if constexpr (std::is_class_v<std::remove_all_extents_t<U>> ||
                      std::is_same_v<std::remove_cv_t<std::remove_all_extents_t<U>>, bool>)
        {
            for (std::size_t i = 0; i < std::extent_v<U>; ++i)
                put_flat_image(dst + i * sizeof(E), v[i]);
            return;
        }
    }
    std::memcpy(dst, &v, sizeof(U));
    if constexpr (std::is_class_v<U>)
    {
        for (const gap g : gaps_v<U>)
            std::memset(dst + g.offset, 0, g.len);
        for (const std::uint32_t b : bools_v<U>)
            dst[b] = dst[b] == std::byte{0} ? std::byte{0} : std::byte{1};
    }
    else if constexpr (std::is_same_v<std::remove_cv_t<U>, bool>)
        dst[0] = dst[0] == std::byte{0} ? std::byte{0}
                                        : std::byte{1}; // on the copied byte: reading a bool
    // that holds 2..255 is UB, and -O2 copies it
}

struct image_cursor
{
    std::byte* base;  // image start
    std::size_t tail; // next free tail byte (starts at the root's fixed size)
    std::size_t cap;  // measured image length
    // serialize_options::canonical_unordered: unordered containers sorted by key (identical values ⇒ identical
    // bytes) or written in their iteration order (no sort, no allocation above 64 entries). Readers accept both.
    bool canonical_unordered = true;
    // G2-11: off < cap ≤ 0xFFFFFFFF — serialize_frame refuses a frame whose length does not fit u32 before any
    // image is encoded (invalid_argument), so the narrowing below cannot truncate. Keep that check if the cap moves.
    std::uint32_t alloc(const std::size_t n, const std::size_t a) noexcept
    {
        tail =
            (tail + a - 1) / a * a; // the skipped gap is already zero: encode_image zeroed [0, cap)
        const std::size_t off = tail;
        tail += n;
        return static_cast<std::uint32_t>(off);
    }
};
inline void put_slot(std::byte* const dst, const std::uint32_t off, const std::uint32_t n) noexcept
{
    std::memcpy(dst, &off, 4);
    std::memcpy(dst + 4, &n, 4);
}

template<class M> void measure_value(const M& v, std::size_t& tail);
template<class M> void encode_value(const M& v, std::byte* dst, image_cursor& c);
struct decode_ctx;
template<class M> bool decode_value(M& out, decode_ctx& c, std::size_t at, int depth);

// per-member walks over a dynamic class (bases first, then members): fb(integral_constant<B>),
// fm(integral_constant<I>); skipped members are left alone by the callers.
template<class T, class FB, class FM> inline void for_each_part(FB&& fb, FM&& fm)
{
    [&]<std::size_t... B, std::size_t... I>(std::index_sequence<B...>, std::index_sequence<I...>) {
        (fb(std::integral_constant<std::size_t, B>{}), ...);
        (fm(std::integral_constant<std::size_t, I>{}), ...);
    }(std::make_index_sequence<base_count<T>()>{}, std::make_index_sequence<part_count<T>()>{});
}

// key_less may throw only through a user key's operator< (review B2); floating-point and byte-compared keys never do.
template<class K> consteval bool key_less_noexcept()
{
    if constexpr (std::floating_point<K> || !std::totally_ordered<K>)
        return true;
    else
        return noexcept(std::declval<const K&>() < std::declval<const K&>());
}
template<class K> inline bool key_less(const K& a, const K& b) noexcept(key_less_noexcept<K>());
// The sequence behind a `vector`-shaped member: the container itself, or a std::stack's protected container.
template<class V> constexpr decltype(auto) sequence_of(V& v) noexcept
{
    using U = std::remove_cv_t<V>;
    if constexpr (is_container_adapter<U>())
    {
        static_assert(members<U>().size() == 1, "es::serreg: a container adapter with one member");
        return (v.[:members<U>()[0]:]);
    }
    else
        return (v);
}
template<class S> inline std::size_t sequence_size(const S& s) noexcept
{
    if constexpr (requires { s.size(); })
        return s.size();
    else
        return static_cast<std::size_t>(std::ranges::distance(s)); // std::forward_list
}
// Walks a sequence in wire order: ordered already, or (an unordered multimap) stably sorted by key.
template<class U, class S, class F>
inline void for_each_in_wire_order(const S& s, const bool canonical, F&& f)
{
    constexpr bool sort = is_multimap_member<U>() && [] {
        if constexpr (is_multimap_member<U>())
            return dyn_member<U>::sort_by_key;
        else
            return false;
    }();
    if constexpr (!sort)
    {
        for (const auto& e : s)
            f(e);
        return;
    }
    else if (!canonical)
    {
        for (const auto& e : s)
            f(e);
        return;
    }
    else
    { // 2.2.6 (review B1): std::sort over (entry, original index) with the index as tie-break — the stability
        // std::stable_sort gave, without its temporary buffer; the only allocation left is the table above 64
        using entry_t = std::remove_cvref_t<decltype(*s.begin())>;
        struct slot
        {
            const entry_t* e;
            std::size_t i;
        };
        slot small[64];
        std::vector<slot> big;
        slot* order = small;
        const std::size_t n = sequence_size(s);
        if (n > 64)
        {
            big.resize(n);
            order = big.data();
        }
        std::size_t i = 0;
        for (const auto& e : s)
        {
            order[i] = slot{&e, i};
            ++i;
        }
        std::sort(order, order + n, [](const slot& a, const slot& b) {
            if (key_less(a.e->first, b.e->first))
                return true;
            if (key_less(b.e->first, a.e->first))
                return false;
            return a.i < b.i;
        });
        for (std::size_t j = 0; j < n; ++j)
            f(*order[j].e);
    }
}

// 1.6: the canonical order of an unordered container's entries — by operator< on the key when the key has one,
// else by the bytes of the key's flat image (deterministic for any trivially copyable key, padding scrubbed).
// The order is built over pointers to the entries: on the stack up to 64 entries, on the heap above that (the
// one allocation a serialize() of an unordered container may make, like a >16 KiB image on a plain sink).
// 2.2.6 (review B2): a floating-point key is ordered by std::strong_order (IEEE totalOrder: -0 before +0, NaNs
// after +inf, ordered by payload) — operator< is not a strict weak order once a NaN key is in the container (an
// unordered container admits one: it compares with ==), and std::sort on it is undefined. A user-defined key
// type must provide a strict total operator<; the comparator is noexcept exactly when that operator< is.
template<class K> inline bool key_less(const K& a, const K& b) noexcept(key_less_noexcept<K>())
{
    if constexpr (std::floating_point<K>)
        return std::strong_order(a, b) < 0;
    else if constexpr (std::totally_ordered<K>)
        return a < b;
    else
    {
        alignas(16) std::byte x[sizeof(K)], y[sizeof(K)];
        put_flat_image(x, a);
        put_flat_image(y, b);
        return std::memcmp(x, y, sizeof(K)) < 0;
    }
}
template<class U, class V, class F> void for_each_in_canonical_order(const V& v, F&& f)
{
    using entry_t = typename V::value_type;
    const auto key_of = [](const entry_t& e) -> const auto& {
        if constexpr (dyn_member<U>::k == kind::map_)
            return e.first;
        else
            return e;
    };
    const entry_t* small[64];
    std::vector<const entry_t*> big;
    const entry_t** order = small;
    if (v.size() > 64)
    {
        big.resize(v.size());
        order = big.data();
    }
    std::size_t n = 0;
    for (const auto& e : v)
        order[n++] = &e;
    std::sort(order, order + n,
              [&](const entry_t* a, const entry_t* b) { return key_less(key_of(*a), key_of(*b)); });
    for (std::size_t i = 0; i < n; ++i)
        f(*order[i]);
}

// 1.6: one entry of a map or set block: the key at the entry's start, a map's value at its canonical offset.
template<class U>
void encode_entry(const typename U::value_type& e, std::byte* const at, image_cursor& c)
{
    if constexpr (dyn_member<U>::k == kind::map_)
    {
        encode_value(e.first, at, c);
        encode_value(e.second, at + container_entry_layout<U>().value_offset, c);
    }
    else
        encode_value(e, at, c);
}

// 2.2.6 (review B7): the measure's arithmetic is checked. A tail that would overflow, or pass measure_limit
// (one past the largest u32 image), saturates at measure_limit and stays there; serialize_frame's u32 check then
// returns error::invalid_argument before anything is encoded, so a lying size() (or a real > 4 GiB value) can
// never wrap into a small image. One overflow-checked multiply and add per block — the measure runs once per
// dynamic argument.
inline constexpr std::size_t measure_limit = std::size_t{1} << 32;
inline void add_block(std::size_t& tail, const std::size_t align, const std::size_t count,
                      const std::size_t stride) noexcept
{
    std::size_t bytes = 0, t = 0;
    if (__builtin_mul_overflow(count, stride, &bytes) ||
        __builtin_add_overflow(static_cast<std::size_t>(align_up(tail, align)), bytes, &t) ||
        t > measure_limit)
        t = measure_limit;
    tail = t;
}
template<class M> void measure_value(const M& v, std::size_t& tail)
{
    using U = mapped_t<M>;
    if constexpr (!is_dynamic<U>())
    {
    }
    else if constexpr (array_traits<U>::is_array)
    {
        for (std::size_t i = 0; i < array_traits<U>::extent; ++i)
            measure_value(v[i], tail);
    }
    else if constexpr (is_dyn_member_v<U>)
    {
        constexpr kind k = dyn_member<U>::k;
        if constexpr (k == kind::string_)
            add_block(tail, 1, v.size(), 1);
        else if constexpr (k == kind::vector_)
        {
            using E = typename dyn_member<U>::elem;
            const auto& seq = sequence_of(v);
            const std::size_t n = sequence_size(seq);
            if (n == 0)
                return; // an empty sequence is the slot {0, 0} and allocates nothing — as encode_value does
            add_block(tail, image_align<E>(), n, image_stride<E>());
            if constexpr (is_dynamic<E>())
                for (const auto& e : seq)
                {
                    if (tail >= measure_limit)
                        return; // saturated: the frame is refused whatever follows
                    measure_value(static_cast<const E&>(e), tail);
                }
        }
        else if constexpr (k == kind::optional_)
        {
            using E = typename dyn_member<U>::elem;
            if (v)
            {
                add_block(tail, image_align<E>(), 1, image_size<E>());
                measure_value(*v, tail);
            }
        }
        else if constexpr (k == kind::map_ || k == kind::set_)
        { // 1.6: a block of entries, then the entries' own tails (order does not change the size)
            constexpr entry_layout el = container_entry_layout<U>();
            if (v.empty())
                return;
            add_block(tail, el.align, v.size(), std::size_t{el.size});
            if constexpr (k == kind::map_)
            {
                if constexpr (is_dynamic<typename dyn_member<U>::key>() ||
                              is_dynamic<typename dyn_member<U>::elem>())
                    for (const auto& e : v)
                    {
                        measure_value(e.first, tail);
                        measure_value(e.second, tail);
                    }
            }
            else if constexpr (is_dynamic<typename dyn_member<U>::key>())
                for (const auto& e : v)
                    measure_value(e, tail);
        }
        else
        {
            if (!v.valueless_by_exception())
                std::visit(
                    [&]<class A>(const A& a) {
                        add_block(tail, image_align<A>(), 1, image_size<A>());
                        measure_value(a, tail);
                    },
                    v);
        }
    }
    else
    { // dynamic class: its fixed part is already counted by the caller; add the members' tails
        for_each_part<U>(
            [&](auto bi) {
                using Bt = base_t<U, decltype(bi)::value>;
                measure_value((const Bt&)v, tail);
            },
            [&](auto mi) {
                constexpr std::size_t I = decltype(mi)::value;
                if constexpr (!member_skipped<U, I>())
                    measure_value(get_part<I>(v), tail);
            });
    }
}
// Total image length of a dynamic root value (fixed part + tails).
template<class T> std::size_t measure_image(const T& v)
{
    std::size_t tail = image_size<T>();
    measure_value(v, tail);
    return tail;
}

template<class M> void encode_value(const M& v, std::byte* const dst, image_cursor& c)
{
    using U = mapped_t<M>;
    if constexpr (!is_dynamic<U>())
        put_flat_image(dst, v);
    else if constexpr (array_traits<U>::is_array)
    {
        using E = typename array_traits<U>::elem;
        for (std::size_t i = 0; i < array_traits<U>::extent; ++i)
            encode_value(v[i], dst + i * image_stride<E>(), c);
    }
    else if constexpr (is_dyn_member_v<U>)
    {
        constexpr kind k = dyn_member<U>::k;
        if constexpr (k == kind::string_)
        {
            if (v.empty())
                put_slot(dst, 0, 0);
            else
            {
                const std::uint32_t off = c.alloc(v.size(), 1);
                std::memcpy(c.base + off, v.data(), v.size());
                put_slot(dst, off, static_cast<std::uint32_t>(v.size()));
            }
        }
        else if constexpr (k == kind::vector_)
        {
            using E = typename dyn_member<U>::elem;
            const auto& seq = sequence_of(v);
            const std::size_t n = sequence_size(seq);
            if (n == 0)
                put_slot(dst, 0, 0);
            else
            {
                const std::uint32_t off = c.alloc(n * image_stride<E>(), image_align<E>());
                put_slot(dst, off, static_cast<std::uint32_t>(n));
                std::size_t i = 0;
                for_each_in_wire_order<U>(seq, c.canonical_unordered, [&](const auto& e) {
                    encode_value(static_cast<const E&>(e), c.base + off + i++ * image_stride<E>(),
                                 c);
                });
            }
        }
        else if constexpr (k == kind::optional_)
        {
            using E = typename dyn_member<U>::elem;
            if (!v)
                put_slot(dst, 0, 0);
            else
            {
                const std::uint32_t off = c.alloc(image_size<E>(), image_align<E>());
                put_slot(dst, off, 1);
                encode_value(*v, c.base + off, c);
            }
        }
        else if constexpr (k == kind::map_ || k == kind::set_)
        { // 1.6: the entries in canonical order — the container's order when it is sorted, else sorted here
            constexpr entry_layout el = container_entry_layout<U>();
            if (v.empty())
            {
                put_slot(dst, 0, 0);
                return;
            }
            const std::uint32_t off = c.alloc(v.size() * std::size_t{el.size}, el.align);
            put_slot(dst, off, static_cast<std::uint32_t>(v.size()));
            std::size_t i = 0;
            using entry_t = typename U::value_type;
            if constexpr (dyn_member<U>::ordered)
                for (const entry_t& e : v)
                    encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
            else if (
                !c.canonical_unordered) // serialize_options: the container's own order, no sort
                for (const entry_t& e : v)
                    encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
            else
                for_each_in_canonical_order<U>(v, [&](const entry_t& e) {
                    encode_entry<U>(e, c.base + off + i++ * std::size_t{el.size}, c);
                });
        }
        else
        {
            if (v.valueless_by_exception())
                put_slot(dst, 0, 0);
            else
                std::visit(
                    [&]<class A>(const A& a) {
                        const std::uint32_t off = c.alloc(image_size<A>(), image_align<A>());
                        put_slot(dst, off, static_cast<std::uint32_t>(v.index()) + 1);
                        encode_value(a, c.base + off, c);
                    },
                    v);
        }
    }
    else
    { // dynamic class: the caller zeroed the fixed part
        for_each_part<U>(
            [&](auto bi) {
                constexpr std::size_t B = decltype(bi)::value;
                using Bt = base_t<U, B>;
                encode_value((const Bt&)v, dst + base_image_offset<U, B>(), c);
            },
            [&](auto mi) {
                constexpr std::size_t I = decltype(mi)::value;
                if constexpr (!member_skipped<U, I>())
                    encode_value(get_part<I>(v), dst + member_image_offset<U, I>(), c);
            });
    }
}
// Encodes a dynamic root value into `image` (exactly measure_image(v) bytes). Allocates nothing but the canonical
// sort's order table of an unordered container above 64 entries (may_sort_v; review B1).
// G2-2 (2.1.1): the whole measured image is zeroed first, so alignment gaps between tail blocks and the padding
// inside element blocks are 0 whatever the sink's memory held before (a reused capture_file buffer, a stack
// buffer, an es::ipc slot, caller memory behind a span_sink): identical values give identical bytes on every sink.
template<class T>
void encode_image(const T& v, std::byte* const image, const std::size_t len,
                  const bool canonical_unordered = true)
{
    std::memset(image, 0, len);
    image_cursor c{image, image_size<T>(), len, canonical_unordered};
    encode_value(v, image, c);
}

// ---- decoding an image (2.1.3: review G2-6, G2-12) -------------------------------------------------------------
// The slot rules and the byte budget are fmt::image_budget (format.hpp), shared with the dictionary-driven walker
// in src/decoder.cpp. `invalid` records a value the C++ type cannot hold (a valueless variant without a
// std::monostate alternative): decoding continues and the caller reports error::invalid_value. `bools` counts
// repaired bool bytes (reported as warning::noncanonical_bool).
struct decode_ctx : fmt::image_budget
{
    bool invalid = false;
    std::uint32_t bools = 0;
    using fmt::image_budget::image_budget;
};

// G2-12: copies a flat value of type U from `src` and repairs every bool byte that is not 0 or 1 to 1 (read as
// true). memcpy of 0x02 into a bool is an invalid value representation: undefined on the first read
// (-fsanitize=bool traps). 2.1.16 (decision D): an enum leaf outside its range (an enum without a fixed
// underlying type) is set to 0 and counted in `invalid`; the caller reports error::invalid_value.
struct flat_copy
{
    std::uint32_t bools = 0;   // repaired bool bytes
    std::uint32_t invalid = 0; // enum leaves outside their range
};
template<class U> inline flat_copy copy_flat(void* const dst, const std::byte* const src) noexcept
{
    std::memcpy(dst, src, sizeof(U));
    flat_copy r;
    if constexpr (bools_v<U>.size() != 0)
        for (const std::uint32_t b : bools_v<U>)
            if (std::to_integer<unsigned>(src[b]) > 1)
            {
                static_cast<std::byte*>(dst)[b] = std::byte{1};
                ++r.bools;
            }
    if constexpr (validate_read_enums)
        if constexpr (enum_ranges_v<U>.size() != 0)
            for (const enum_range& e : enum_ranges_v<U>)
                if (!enum_leaf_in_range(e, src + e.offset))
                {
                    std::memset(static_cast<std::byte*>(dst) + e.offset, 0, e.size);
                    ++r.invalid;
                }
    return r;
}
// True when every bool byte of a flat U at `src` is 0 or 1 and (ES_SERREG_VALIDATE_READ=1) every bounded enum
// leaf is in range (what a zero-copy view needs).
template<class U> inline bool bools_canonical(const std::byte* const src) noexcept
{
    if constexpr (bools_v<U>.size() != 0)
        for (const std::uint32_t b : bools_v<U>)
            if (std::to_integer<unsigned>(src[b]) > 1)
                return false;
    if constexpr (validate_read_enums)
        if constexpr (enum_ranges_v<U>.size() != 0)
            for (const enum_range& e : enum_ranges_v<U>)
                if (!enum_leaf_in_range(e, src + e.offset))
                    return false;
    return true;
}
// Index of std::monostate among a variant's alternatives, or variant_npos.
template<class V> consteval std::size_t monostate_index()
{
    return []<std::size_t... I>(std::index_sequence<I...>) {
        std::size_t r = std::variant_npos;
        ((r == std::variant_npos && std::is_same_v<std::variant_alternative_t<I, V>, std::monostate>
              ? (r = I, 0)
              : 0),
         ...);
        return r;
    }(std::make_index_sequence<std::variant_size_v<V>>{});
}

template<class M> bool decode_value(M& out, decode_ctx& c, const std::size_t at, const int depth)
{
    using U = mapped_t<M>;
    const std::span<const std::byte> image = c.image;
    if (depth > max_image_depth)
        return false;
    if constexpr (!is_dynamic<U>())
    {
        if (at > image.size() || image.size() - at < sizeof(U))
            return false;
        const flat_copy fc = copy_flat<U>(&out, image.data() + at);
        c.bools += fc.bools;
        if (fc.invalid)
            c.invalid = true; // 2.1.16: an enum leaf outside its range, reported as invalid_value
        return true;
    }
    else if constexpr (array_traits<U>::is_array)
    {
        using E = typename array_traits<U>::elem;
        for (std::size_t i = 0; i < array_traits<U>::extent; ++i)
            if (!decode_value(out[i], c, at + i * image_stride<E>(), depth + 1))
                return false;
        return true;
    }
    else if constexpr (is_dyn_member_v<U>)
    {
        constexpr kind k = dyn_member<U>::k;
        fmt::slot s;
        if (!fmt::read_slot(image, at, s))
            return false;
        if constexpr (k == kind::string_)
        {
            if (!c.take(s, s.n))
                return false;
            out.assign(s.n ? reinterpret_cast<const char*>(image.data() + s.off) : "", s.n);
            return true;
        }
        else if constexpr (k == kind::vector_)
        {
            using E = typename dyn_member<U>::elem;
            // (an element with no image bytes is rejected at compile time: check_supported, 2.1.12 review D-05)
            if (!c.take_n(s, image_stride<E>()))
                return false;
            auto& seq = sequence_of(out);
            if constexpr (requires { seq.resize(s.n); })
            { // vector, deque, list, forward_list, a stack's container: size it, then fill in order
                seq.resize(s.n);
                std::size_t i = 0;
                for (auto& e : seq)
                    if (!decode_value(e, c, s.off + i++ * image_stride<E>(), depth + 1))
                        return false;
            }
            else
            { // a multimap: entries are decoded into a temporary and inserted; duplicate keys are data
                seq.clear();
                for (std::uint32_t i = 0; i < s.n; ++i)
                {
                    E e{};
                    if (!decode_value(e, c, s.off + std::size_t{i} * image_stride<E>(), depth + 1))
                        return false;
                    seq.insert(seq.end(), std::move(e));
                }
            }
            return true;
        }
        else if constexpr (k == kind::optional_)
        {
            using E = typename dyn_member<U>::elem;
            if (s.n > 1 || !c.take(s, image_size<E>()))
                return false;
            if (s.n == 0)
            {
                out.reset();
                return true;
            }
            out.emplace();
            return decode_value(*out, c, s.off, depth + 1);
        }
        else if constexpr (k == kind::map_ || k == kind::set_)
        { // 1.6: every entry is decoded into temporaries and inserted; a key that is already there (a duplicate
            // on the wire: not canonical, or not a map) is reported as invalid_value and the first value is kept
            constexpr entry_layout el = container_entry_layout<U>();
            if (!c.take_n(s, el.size))
                return false;
            out.clear();
            if constexpr (requires { out.reserve(std::size_t{}); })
                out.reserve(s.n); // a hash container: one rehash, not log n of them
            for (std::uint32_t i = 0; i < s.n; ++i)
            {
                const std::size_t entry_at = s.off + std::size_t{i} * el.size;
                typename dyn_member<U>::key key{};
                if (!decode_value(key, c, entry_at, depth + 1))
                    return false;
                const std::size_t before = out.size();
                // the hint is exact for a container in canonical order (O(1) per entry), harmless otherwise
                if constexpr (k == kind::map_)
                {
                    typename dyn_member<U>::elem value{};
                    if (!decode_value(value, c, entry_at + el.value_offset, depth + 1))
                        return false;
                    out.emplace_hint(out.end(), std::move(key), std::move(value));
                }
                else
                    out.emplace_hint(out.end(), std::move(key));
                if (out.size() == before)
                    c.invalid = true;
            }
            return true;
        }
        else
        {
            if (s.n > dyn_member<U>::count || !c.take(s, 0))
                return false; // take() with 0 bytes: the n == 0 ⇔ off == 0 rule and off inside the image
            if (s.n == 0)
            { // valueless on the wire (the writer's variant was valueless_by_exception)
                constexpr std::size_t m = monostate_index<U>();
                if constexpr (m != std::variant_npos)
                    out.template emplace<m>();
                else // no alternative means "no value": `out` is left as it was; error::invalid_value
                    c.invalid = true;
                return true;
            }
            bool ok = false;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((s.n == I + 1 ? (ok = c.take(s, image_size<std::variant_alternative_t<I, U>>()) &&
                                       decode_value(out.template emplace<I>(), c, s.off, depth + 1),
                                  void())
                               : void()),
                 ...);
            }(std::make_index_sequence<dyn_member<U>::count>{});
            return ok;
        }
    }
    else
    {
        if (at > image.size() || image.size() - at < image_size<U>())
            return false;
        bool ok = true;
        for_each_part<U>(
            [&](auto bi) {
                constexpr std::size_t B = decltype(bi)::value;
                using Bt = base_t<U, B>;
                ok = ok && decode_value((Bt&)out, c, at + base_image_offset<U, B>(), depth + 1);
            },
            [&](auto mi) {
                constexpr std::size_t I = decltype(mi)::value;
                if constexpr (!member_skipped<U, I>())
                    ok = ok && decode_value(get_part<I>(out), c, at + member_image_offset<U, I>(),
                                            depth + 1);
            });
        return ok;
    }
}
// Decodes a dynamic root value from its image (the bytes after the u32 len, u32 0 prefix): length_mismatch when
// a slot breaks the rules or the budget, invalid_value when a variant could not hold its (valueless) value.
template<class T> status decode_image(T& out, const std::span<const std::byte> image)
{
    decode_ctx c(image, image_size<mapped_t<T>>());
    if (!decode_value(out, c, 0, 0))
        return std::unexpected(error::length_mismatch);
    if (c.bools)
        report_warning(warning::noncanonical_bool, schema_hash_memo<mapped_t<T>>, c.bools);
    if (c.invalid)
        return std::unexpected(error::invalid_value);
    return {};
}

} // namespace detail

// ---- public constants --------------------------------------------------------------------------------
template<class T>
inline constexpr std::uint64_t schema_hash_v = detail::schema_hash_of<detail::mapped_t<T>>();
template<class T>
inline constexpr std::uint64_t layout_hash_v = detail::layout_hash_of<detail::mapped_t<T>>();
template<class T>
inline constexpr std::string_view canonical_name_v =
    std::define_static_string(detail::canonical_name<detail::mapped_t<T>>());
// The exact strings the two hashes are computed from (fingerprint64(schema_string_v<T>) == schema_hash_v<T>).
// Nothing is emitted into the binary unless a program reads them at run time.
template<class T>
inline constexpr std::string_view schema_string_v =
    std::define_static_string(detail::canonical_string<detail::mapped_t<T>>(true));
template<class T>
inline constexpr std::string_view layout_string_v =
    std::define_static_string(detail::canonical_string<detail::mapped_t<T>>(false));

template<class T>
concept flat_type = std::is_trivially_copyable_v<detail::mapped_t<T>> &&
                    (std::is_class_v<detail::mapped_t<T>> || std::is_enum_v<detail::mapped_t<T>>);
// v2: a class type encoded as a canonical image (plan v2.0). Not flat, not a dyn member type itself. Any other
// non-trivially-copyable class is admitted here so that check_supported() (FR-S7) names the rule it breaks
// instead of an overload-resolution failure.
// 1.6: a container (std::vector, std::optional, std::variant, std::map, std::set, …) is a dynamic type of its own
// when it is an argument — the transparent wrapper type of plans-for-containers.md; a std::string stays the
// `string` argument category.
template<class T>
concept dynamic_type = std::is_class_v<detail::mapped_t<T>> && !flat_type<T> &&
                       (!detail::is_dyn_member_v<detail::mapped_t<T>> ||
                        detail::is_container_arg_v<detail::mapped_t<T>>) &&
                       !detail::is_string_view_v<detail::mapped_t<T>>;

// descriptor_v<T> is completed in blob.hpp (it needs the blob bytes); the pieces live here.
namespace detail
{
template<class T> consteval type_desc make_type_desc_base()
{
    using U = mapped_t<T>;
    type_desc d{};
    d.name = canonical_name_v<U>;
    d.schema_hash = schema_hash_v<U>;
    d.layout_hash = layout_hash_v<U>;
    d.size = image_size<U>(); // sizeof for flat types, the fixed part for dynamic ones
    d.align = image_align<U>();
    d.since = annotation_value<since>(std::meta::dealias(^^U));
    d.deprecated = annotation_value<deprecated>(std::meta::dealias(^^U));
    d.is_flat = !is_dynamic<U>();
    if constexpr (std::is_enum_v<U>)
    {
        d.k = kind::enum_;
        d.underlying = enum_underlying_kind<U>(); // 2.1.16 (decision D)
        d.enum_fixed =
            has_fixed_underlying_type_v<U>; // 1.6: written to the ENTRY (enum_flag_fixed)
        d.enumerators = enumerators_v<U>;
    }
    else if constexpr (is_container_arg_v<U>)
    { // 1.6: a container argument — one unnamed field, the container itself, at 0 (struct_flag_transparent)
        d.k = kind::struct_;
        d.fields = wrapper_fields_v<U>;
        d.transparent = true;
    }
    else if constexpr (is_dynamic<U>())
    { // v2: the encoder writes every byte of the image, so there is no scrub table
        d.k = kind::struct_;
        d.fields = fields_v<U>;
    }
    else
    {
        d.k = kind::struct_;
        d.fields = fields_v<U>;
        d.padding_gaps = gaps_v<U>;
        d.bool_offsets = bools_v<U>;
    }
    return d;
}
} // namespace detail

} // namespace es::serreg
