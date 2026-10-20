// tests/fuzz/fuzz_targets.cpp — fuzz entry points over the hostile-input surfaces of the library (FR-D5):
// the RRI/ENTRY parser, the capture walker + decoder + formatters, the ELF section finder and the --config
// parser. Every call must return; the sanitizer build turns any UB or leak into a failure.
//
// Built with -DES_SERREG_LIBFUZZER=ON (clang, -fsanitize=fuzzer) this file also defines LLVMFuzzerTestOneInput,
// forwarding to the target named by the compile definition ES_SERREG_FUZZ_TARGET (one executable per target).
#include "fuzz_targets.hpp"

#include "corpus.hpp" // the golden fixtures' types: the typed readers below use their exact signatures

#include <es/serreg/dictionary.hpp>
#include <es/serreg/serreg.hpp>
#include <es/serreg/tools.hpp>
#include <es/serreg/walk.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace es::serreg;

std::span<const std::byte> as_bytes(const std::uint8_t* const data, const std::size_t size)
{
    return {reinterpret_cast<const std::byte*>(data), size};
}

bool discard(std::string_view, void*)
{
    return true;
}

struct null_visitor final : tools::decoded_value_visitor
{
    void begin_struct(std::string_view, std::string_view) override
    {
    }
    void end_struct() override
    {
    }
    void begin_array(std::string_view, std::uint32_t) override
    {
    }
    void end_array() override
    {
    }
    void scalar(std::string_view, kind, std::span<const std::byte>) override
    {
    }
    void enumerator(std::string_view, std::string_view, std::string_view, std::int64_t) override
    {
    }
    void literal(std::string_view, std::uint64_t, std::string_view) override
    {
    }
};

// 2.1.6 (G2-6 follow-up): the typed image decoder (reflect.hpp) and the dictionary-driven walker (decoder.cpp) share
// one set of slot rules, so on every image of a corpus dynamic type they must agree: the walker succeeds exactly when
// the typed decode does not fail with length_mismatch (invalid_value — a valueless variant the C++ type cannot hold —
// is a well-formed image the walker shows as null). A disagreement aborts: the harness reports it as a crash.
template<class T> void agree_on(const record_view& r, const dyn::dictionary& d)
{
    const dyn::type* const t = d.find_type(schema_hash_v<T>);
    if (!t)
        return;
    T typed{};
    const status td = es::serreg::detail::decode_image(typed, r.payload);
    null_visitor v;
    const auto walked = tools::visit_record(r.payload, *t, d, v);
    const bool typed_ok = td || td.error() == error::invalid_value;
    if (typed_ok != walked.has_value())
    {
        std::println(stderr, "decoders disagree on {}: typed {}, walker {}", t->name,
                     td ? "ok" : to_string(td.error()), walked ? "ok" : to_string(walked.error()));
        std::abort();
    }
}
void decoders_agree(const std::span<const std::byte> frame)
{
    static const dyn::dictionary proc = [] {
        auto d = tools::load_dictionary_rri(registry::instance().export_image());
        return d ? *d : dyn::dictionary{};
    }();
    const auto fv = inspect(frame, &proc);
    if (!fv)
        return;
    for (const record_view& r : fv->records())
    {
        if (r.ak != fmt::arg_type)
            continue;
        if (r.hash == schema_hash_v<corpus::DynMsg>)
            agree_on<corpus::DynMsg>(r, proc);
        else if (r.hash == schema_hash_v<corpus::DynNested>)
            agree_on<corpus::DynNested>(r, proc);
        else if (r.hash == schema_hash_v<corpus::DynArr>)
            agree_on<corpus::DynArr>(r, proc);
    }
}

// G2-1 (2.1.1): the typed readers on hostile frames. The seed corpus holds frames of exactly these signatures
// (tests/golden/make_fixtures.cpp, make_dynamic_fixtures.cpp), so mutations that keep the call hash reach the
// record bounds checks of deserialize / deserialize_print / dispatch, not just the hash comparison. Every frame of
// the capture is also offered as-is and truncated to its first 24/32 bytes (a frame_len that lies).
void fuzz_typed(const std::span<const std::byte> in)
{
    using namespace es::serreg::literals;
    // The frame is copied so that it ends exactly where a PROT_NONE page begins: a read past its end faults even
    // without a sanitizer runtime (and is a heap-buffer-overflow under ASan for the vector fallback).
    constexpr std::size_t guard_cap = std::size_t{1} << 20;
    static std::byte* const guard_base = [] {
        const long pg = ::sysconf(_SC_PAGESIZE);
        void* const m = ::mmap(nullptr, guard_cap + static_cast<std::size_t>(pg),
                               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
            return static_cast<std::byte*>(nullptr);
        ::mprotect(static_cast<std::byte*>(m) + guard_cap, static_cast<std::size_t>(pg), PROT_NONE);
        return static_cast<std::byte*>(m);
    }();
    std::vector<std::byte> copy;
    std::span<const std::byte> frame;
    if (guard_base && in.size() <= guard_cap)
    {
        std::byte* const at = guard_base + (guard_cap - in.size());
        if (!in.empty())
            std::memcpy(at, in.data(), in.size());
        frame = {at, in.size()};
    }
    else
    {
        copy.assign(in.begin(), in.end());
        frame = copy;
    }
    corpus::Order o{};
    corpus::Cancel c{};
    corpus::Side sd{};
    std::string str;
    std::string_view sv;
    int i32 = 0;
    unsigned u32 = 0;
    double f64 = 0;
    corpus::DynMsg dm;
    corpus::DynNested dn;
    corpus::DynArr da;
    (void)deserialize(frame, "new order"_lit, o);
    (void)deserialize(frame, c, "why"_lit);
    (void)deserialize(frame, sd);
    (void)deserialize(frame, str, i32, f64, "evt"_);
    (void)deserialize(frame, sv, i32, f64, "evt"_);
    (void)deserialize(frame, dm);
    (void)deserialize(frame, dn, da);
    (void)deserialize_print(frame, "hello {} {} {}"_, str, i32, "worlds"_);
    (void)deserialize_println(frame, "px={:.2f} qty={} side={}"_, f64, u32, sd);
    (void)dispatch<corpus::Order, corpus::Cancel, corpus::DynMsg, corpus::DynNested, int>(
        frame, [](const auto&) {});
    decoders_agree(frame);
}

// 2.0.6: the block reader parses the same hostile bytes from a file; a memfd keeps that cheap per iteration.
void fuzz_block_reader(const std::uint8_t* const data, const std::size_t size)
{
    static const int fd = ::memfd_create("serreg_fuzz_capture", MFD_CLOEXEC);
    if (fd < 0 || ::ftruncate(fd, 0) != 0 ||
        (size && ::pwrite(fd, data, size, 0) != static_cast<ssize_t>(size)))
        return;
    const std::string path = "/proc/self/fd/" + std::to_string(fd);
    (void)tools::for_each_frame_in_file(
        path,
        [](const dyn::dictionary&, const tools::frame_ref& f) { return f.bytes.size() >= 16; },
        {.block_bytes = 4096});
}

} // namespace

extern "C" int fuzz_rri(const std::uint8_t* const data, const std::size_t size)
{
    dyn::dictionary d;
    if (d.add_rri(as_bytes(data, size)))
    {
        (void)dyn::to_rri(d); // re-encoding what was accepted must be safe too
        (void)d.to_json();
        (void)d.to_toml();
    }
    dyn::dictionary e;
    (void)e.add_entries(
        as_bytes(data, size)); // the same bytes as a bare ENTRY stream (ELF section content)
    return 0;
}

extern "C" int fuzz_capture(const std::uint8_t* const data, const std::size_t size)
{
    const auto bytes = as_bytes(data, size);
    (void)tools::verify(bytes);
    tools::dump_options text;
    text.hex = (size & 1) != 0;
    text.flatten = (size & 2) != 0;
    (void)tools::dump(bytes, nullptr, text, &discard, nullptr);
    tools::dump_options json;
    json.fmt = tools::format::json;
    (void)tools::dump(bytes, nullptr, json, &discard, nullptr);
    null_visitor
        v; // 2.0.6: the generic walk (inspect + visit_record per record) and the block reader
    (void)tools::walk_values<>(bytes, [](std::string_view, const auto&) {}, v);
    fuzz_block_reader(data, size);
    (void)tools::for_each_frame(bytes, [](const dyn::dictionary&, const tools::frame_ref& f) {
        fuzz_typed(f.bytes);
        return true;
    });
    // frames of a bare stream and of a damaged FILE that for_each_frame would not deliver: every 8-aligned
    // offset that reads as a frame header is offered to the typed readers directly (bounded to 64 per input)
    std::size_t tried = 0;
    for (std::size_t off = 0; off + 16 <= size && tried < 64; off += 8)
    {
        std::uint32_t len;
        std::memcpy(&len, data + off, 4);
        if (len < 16 || len % 8 || len > size - off)
            continue;
        ++tried;
        fuzz_typed(bytes.subspan(off, len));
        fuzz_typed(
            bytes.subspan(off, size - off)); // the span runs on: only frame_len may bound the reads
    }
    return 0;
}

extern "C" int fuzz_elf(const std::uint8_t* const data, const std::size_t size)
{
    const auto bytes = as_bytes(data, size);
    if (const auto sec = tools::extract_section_from_image(bytes, "es_serreg_dict"))
    {
        dyn::dictionary d;
        (void)d.add_entries(*sec);
    }
    (void)tools::extract_section_from_image(bytes, ".text");
    (void)tools::extract_embedded_dictionary(
        bytes); // A4 (2.1.8): magic dispatch, then the same reader
    return 0;
}

extern "C" int fuzz_config(const std::uint8_t* const data, const std::size_t size)
{
    std::string err;
    (void)cli::parse_config({reinterpret_cast<const char*>(data), size}, err);
    return 0;
}

#ifdef ES_SERREG_LIBFUZZER
#ifndef ES_SERREG_FUZZ_TARGET
#error "define ES_SERREG_FUZZ_TARGET to one of fuzz_rri, fuzz_capture, fuzz_elf, fuzz_config"
#endif
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    return ES_SERREG_FUZZ_TARGET(data, size);
}
#endif
