# 2. Getting started

## 2.1 Requirements

- GCC 16 or newer with `-std=c++26 -freflection` (the only compiler that builds the library today; Bloomberg's
  clang-p2996 compiles the golden file for the cross-check). Clang 22+ without P2996 cannot build it.
- CMake ≥ 4.0, Ninja. Linux x86-64 (ELF64, little-endian). binutils ≥ 2.36 (`[[gnu::retain]]`).
- No dependencies. Tests use Catch2 v3 (fetched by CMake, or `-DES_SERREG_CATCH2_DIR=<checkout>` offline).

## 2.2 Build, test, install

```sh
export CXX=g++                       # or /opt/gcc-16/bin/g++-16; must be GCC 16+
cmake --preset gcc16-release && cmake --build --preset gcc16-release -j
ctest --preset gcc16-release --output-on-failure
cmake --install build/gcc16-release --prefix /opt/serreg
```

If `g++-16` is not the system compiler, run binaries with `LD_LIBRARY_PATH=<gcc16>/lib64` (or link with
`-Wl,-rpath`). `scripts/run-all-tests.sh --quick` is the smoke run (debug preset, format check, `srdump`
checks) and `scripts/run-all-tests.sh` the full protocol with every log captured under `logs/`.

Presets: `gcc16-debug`, `gcc16-release`, `gcc16-static` (static `libserreg`, no dlopen tests), `gcc16-static-none`
(static, no image embedding: `ES_SERREG_IMAGE_FORMAT=none`), `gcc16-ubsan-trap` (UBSan without a runtime library),
`gcc16-asan-ubsan`, `gcc16-tsan`, `opt-gcc16-debug` (pins `/opt/gcc-16`), `clang-p2996`.

## 2.3 A first program

```cpp
#include <es/serreg/serreg.hpp>
#include <cstring>
#include <print>

namespace ex {
enum class Side : std::uint8_t { buy = 1, sell = 2 };
struct Order { std::uint64_t id; std::int64_t px; std::uint32_t qty; Side side; char sym[8]; };
}

int main()
{
    using namespace es::serreg;
    using namespace es::serreg::literals;

    capture_file file{"orders.ser"};                 // FILE header + dictionary (+ CHAIN, writer info) written now
    if (!file.is_open()) return 1;                   // file.state().error() says why
    ex::Order o{42, 1050000, 100, ex::Side::buy, "AAPL"};
    if (!serialize(file, "new order"_, o)) return 1; // one 48-byte frame; the literal costs 0 bytes
    if (!file.close()) return 1;                     // flush; nothing is rewritten (format 1.7)

    // read it back, typed: the walker skips the dictionary and the meta frames and hands over each data frame
    ex::Order back{};
    const auto walk = tools::for_each_frame("orders.ser", [&](const dyn::dictionary&, const tools::frame_ref& f) {
        return !deserialize(f.bytes, "new order"_, back);   // false (stop) once the frame decoded
    });
    if (!walk) return 1;
    std::println("{} {}", back.id, std::string_view(back.sym, strnlen(back.sym, sizeof back.sym)));
}
```

Build it against an installed prefix:

```cmake
find_package(serreg 2.2 CONFIG REQUIRED)      # -DCMAKE_PREFIX_PATH=/opt/serreg; 2.2.x (the minor is the compatibility unit)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE es::serreg) # brings -std=c++26 -freflection -fno-gnu-unique, includes, libserreg
```

The package has three libraries (decision G): `es::serreg` (the runtime), `es::serreg_tools` (the
srdump command line, the Wireshark generator and the dictionary diff, for programs that embed them) and `es::ipc`
(the shared-memory rings of chapter 11, header-only C++23). `es::serreg` does not bring `es::ipc`: link it too when
you use the rings. `find_package(serreg … COMPONENTS ipc|serreg|tools)` loads only the named ones
(`package-contents.md`).

Then look at what you wrote without any of your headers:

```sh
srdump orders.ser                # dictionary (types, literals, calls) + every frame decoded
srdump orders.ser --verify       # "ok: 1 frames, 2 records, 48 bytes"
srdump --elf ./app --toml        # the schema compiled into the binary
file -m /opt/serreg/share/serreg/serreg.magic orders.ser
```

Notice what you did **not** do: no registration call, no schema file, no generated code. `ex::Order` and
`ex::Side` were described by reflection when `serialize` was instantiated, and pushed into the registry before
`main()` began.

## 2.4 What can be serialized

| as an argument of `serialize()` | how it is encoded |
|---|---|
| flat class/enum (`flat_type`): arithmetic, `bool`, char types, `std::byte`, enums, C arrays, `std::array`, nested flat classes, private members, non-virtual bases | native bytes, padding zeroed |
| `as_flat<T>` mapped types (top-level only) | the mapped flat type |
| literal `"text"_` / `"text"_lit` / `lit<"text">{}` / `serialize<"text">(…)` | 0 bytes; id in the call signature |
| scalars (`int`, `double`, `bool`, `char`, `std::byte`, …) | their bytes padded to 8 |
| runtime strings (`const char*`, `char[N]`, `std::string_view`, `std::string`) | `u32 len, bytes` |
| dynamic class (`std::string` / `std::vector` / `std::optional` / `std::variant` members at any depth) | `u32 len, u32 0, canonical image` |
| … with `std::map` / `std::unordered_map` / `std::flat_map` / `std::set` / `std::unordered_set` / `std::flat_set` members | the same; the wire shapes `map` and `set`, entries sorted by key (format 1.6) |
| a container itself (`std::vector<int>`, `std::map<std::string, double>`, `std::optional<Order>`, …) | `u32 len, u32 0, canonical image` — the record of its transparent type, named after the container (`std::vector<i32>`) (format 1.6) |
| … with `std::deque` / `std::list` / `std::forward_list` / `std::stack` members or arguments | the `vector` shape: the same slot and block as a `std::vector` of the element (a stack bottom first); as an argument, under the container's own name (`std::deque<i32>`) |
| … with `std::pair` / `std::tuple` members or arguments | positional dynamic classes: parts `first`/`second` or `_0`, `_1`, … at canonical offsets, always encoded element-wise (`dyn<std::pair<i32,f64>>`) |
| … with `std::multimap` / `std::unordered_multimap` members or arguments | a `vector` of `std::pair<K,V>` sorted by key, duplicate keys kept (schema kindspec `multimap<K,V>`) |

Rejected at compile time with a message naming the rule (FR-S7): pointers, references, unions, polymorphic
types, bit-fields, `std::string_view` members, `std::vector<bool>`, smart pointers ("ownership is not data"),
`std::tuple<>`, and **any other standard-library class** — the vocabulary above is closed, so `std::priority_queue`,
`std::bitset` or `std::chrono::duration` are refused with the list of what is accepted and a pointer to
`as_flat<T>` / `es::serreg::type_properties<T>`; unnamed types. Annotations: `[[=es::serreg::skip]]` (member not serialized, scrubbed as padding),
`[[=es::serreg::since(N)]]`, `[[=es::serreg::deprecated(N)]]` (schema string only).

## 2.5 Where to go next

Chapter 3 covers the writer API in full (sinks, options, print), chapter 4 the five ways to read, chapter 5 the
registry and its runtime hooks, chapter 7 the bytes.
