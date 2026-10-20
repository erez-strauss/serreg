// ce-example0 — slide 19 of the Core C++ 2026 talk, "C++26 reflection in one slide": no library, just <meta>.
// GCC 16: -std=c++26 -freflection. clang-p2996: -std=c++26 -freflection-latest -stdlib=libc++.
//   ^^T                 a reflection: a value of type std::meta::info
//   [: m :]             a splice: back from the value to code
//   template for        an expansion statement, unrolled once per member
#include <cstdint>
#include <meta>
#include <print>

template<class T> void show(const T& v)
{
    template for (constexpr std::meta::info m : std::define_static_array(
                      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())))
        std::println("{} @{} = {}", std::meta::identifier_of(m), std::meta::offset_of(m).bytes, v.[:m:]);
}

struct Quote
{
    std::uint64_t id;
    double px;
    std::uint32_t qty;
    char sym[8];
};

int main()
{
    constexpr auto name = std::define_static_string(std::meta::identifier_of(^^Quote)); // consteval → a constant
    constexpr std::size_t members =
        std::meta::nonstatic_data_members_of(^^Quote, std::meta::access_context::unchecked()).size();
    std::println("{}: {} members, {} bytes", name, members, sizeof(Quote));
    show(Quote{42, 105.25, 100, "AAPL"});
}
