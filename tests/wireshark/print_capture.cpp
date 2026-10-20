// tests/wireshark/print_capture.cpp — writes a capture of print/println frames that exercise std::format's
// replacement fields (width, fill and alignment, sign, #, 0, precision, integer bases, floating-point types,
// strings, bool, char, argument ids), so wireshark_dissector_parity can compare the generated dissector's
// rendered messages with srdump's (2.1.10; a 64-bit enum since 2.1.11).
//
//   wireshark_print_capture <out.ser>
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <limits>
#include <print>
#include <string>

namespace ws_print
{
using namespace es::serreg;
using namespace es::serreg::literals;

enum class Color : std::uint8_t
{
    red = 1,
    green = 2,
};
// 2.1.11: a 64-bit enum — its value reaches Lua through UInt64:tonumber(), a float under Lua 5.4 (Wireshark 4.6+)
enum class Wide : std::uint64_t
{
    far = std::uint64_t{1} << 40,
};

class writer
{
public:
    explicit writer(std::string path) : path_{std::move(path)}
    {
    }

    int run() const
    {
        capture_file out(path_);
        if (!out.is_open())
        {
            std::println(stderr, "cannot open {}", path_);
            return 1;
        }
        const bool ok = integers(out) && floats(out) && others(out);
        return ok && out.close() ? 0 : 1;
    }

private:
    std::string path_;

    static bool integers(capture_file& out)
    {
        const std::int32_t n = -255;
        const std::uint32_t u = 48879;
        const std::int64_t big = std::numeric_limits<std::int64_t>::min();
        const std::uint64_t ubig = std::numeric_limits<std::uint64_t>::max();
        return println(out, "[{}] [{:5}] [{:<5}] [{:^7}] [{:*>8}]"_, n, n, n, n, n) &&
               println(out, "[{:+}] [{: }] [{:05}] [{:x}] [{:#x}] [{:X}]"_, u, u, u, u, u, u) &&
               println(out, "[{:#010x}] [{:o}] [{:#o}] [{:b}] [{:#b}] [{:d}]"_, u, u, u, u, u, u) &&
               println(out, "[{:x}] [{:#X}] [{:+08d}] [{:^+9}]"_, n, n, n, n) &&
               println(out, "[{}] [{:x}] [{}] [{:#x}]"_, big, big, ubig, ubig);
    }
    static bool floats(capture_file& out)
    {
        const double d = 1234.5678;
        const double small = -0.000123;
        const float f = 3.25f;
        return println(out, "[{}] [{:.3f}] [{:10.2f}] [{:<10.1f}] [{:e}] [{:.2e}] [{:E}]"_, d, d, d,
                       d, d, d, d) &&
               println(out, "[{:g}] [{:.3g}] [{:+.1f}] [{:08.2f}] [{:.4}] [{:G}]"_, d, d, d, d, d,
                       small) &&
               println(out, "[{}] [{}] [{:.1f}] [{:a}] [{}] [{}]"_, small, f, f, 0.5, 1e21,
                       100.0) &&
               // 2.1.12 (review T-04): floats that are not exact in binary print as floats, not doubles
               println(out, "[{}] [{:>12}] [{:.10f}] [{:e}] [{:a}]"_, 0.1f, 3.14f, 0.1f, 3.14f,
                       0.1f);
    }
    static bool others(capture_file& out)
    {
        const std::string s = "serreg";
        const bool yes = true;
        const char c = 'Z';
        return println(out, "[{}] [{:10}] [{:>10}] [{:.3}] [{:^9.2}] [{:-^12}]"_, s, s, s, s, s,
                       s) &&
               println(out, "[{}] [{:>6}] [{:d}] [{}] [{:d}] [{:>3}]"_, yes, yes, yes, c, c, c) &&
               println(out, "[{1}] [{0}] [{0:x}] [{0:d}] [{1:>4}]"_, 255, "ab"_) &&
               println(out, "[{}] [{:>8}] [{:<8}]"_, Color::green, Color::red, "lit"_) &&
               println(out, "[{}] [{}] [{}]"_, Wide::far, Wide{7},
                       Wide{std::uint64_t{1} << 60}) && // above 2^53 (2.1.12, T-02)
               print(out, "{{}} braces [{}]"_, 7);
    }
};
} // namespace ws_print

int main(const int argc, char** const argv)
{
    return ws_print::writer{argc > 1 ? argv[1] : "print_specs.ser"}.run();
}
