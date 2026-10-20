// tests/unit/test_capture.cpp — the FILE container and the tools layer (spec §6.5, §6.9, FR-W1/W2/W7, FR-D1/D2/D6):
// capture_file round-trip through tools::read_file / verify / dump, bare frame streams with a dictionary frame,
// the ELF-embedded registry copy, and ES_SERREG_DUMP_REGISTRY_FD (fork + pipe).
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{

std::string tmp_path(const char* const name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}

std::vector<std::byte> read_all(const int fd)
{
    std::vector<std::byte> out;
    std::byte buf[4096];
    for (;;)
    {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

} // namespace

TEST_CASE("FR-W1 capture_file: compact frames read back, verify and dump", "[capture][file]")
{
    const std::string path = tmp_path("compact.ser");
    {
        capture_file f(path);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit));
        REQUIRE(serialize(f, corpus::Padded{9, 8, 7}, corpus::Side::sell));
        REQUIRE(f.close());
        CHECK_FALSE(f.is_open());
    }
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() > fmt::file_header_size);
    CHECK(std::memcmp(bytes->data(), fmt::file_magic.data(), 4) == 0);

    const auto rep = tools::verify(*bytes);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 3);
    CHECK(rep->records == 6);
    CHECK(rep->bytes == frame_size<decltype("evt"_lit), corpus::Order>() +
                            frame_size<corpus::Cancel, decltype("why"_lit)>() +
                            frame_size<corpus::Padded, corpus::Side>());

    SECTION("text dump has field names and values")
    {
        std::string text;
        REQUIRE(tools::dump(*bytes, nullptr, tools::dump_options{}, text));
        for (const char* const needle :
             {"corpus::Order", "id: 42", "px: 1050000", "qty: 100", "side: corpus::Side::buy (1)",
              "sym: \"AAPL", "evt", "why", "corpus::Padded", "value: 8", "corpus::Cancel",
              "reason: 7"})
        {
            INFO(needle);
            CHECK(text.find(needle) != std::string::npos);
        }
    }
    SECTION("json dump")
    {
        std::string js;
        tools::dump_options o;
        o.fmt = tools::format::json;
        REQUIRE(tools::dump(*bytes, nullptr, o, js));
        CHECK(js.find("\"id\":\"42\"") !=
              std::string::npos); // gk 1.12: 64-bit integers are quoted in JSON
        CHECK(js.find("\"reason\":7") != std::string::npos);
        CHECK(js.find("corpus::Order") != std::string::npos);
    }
    SECTION("type filter and frame range")
    {
        std::string text;
        tools::dump_options o;
        o.type_name_filter = "corpus::Cancel";
        REQUIRE(tools::dump(*bytes, nullptr, o, text));
        CHECK(text.find("reason: 7") != std::string::npos);
        CHECK(text.find("px: 1050000") == std::string::npos);
    }
    SECTION("a damaged capture is reported, not crashed on")
    {
        std::vector<std::byte> bad = *bytes;
        bad.resize(bad.size() - 4); // last frame cut
        const auto r = tools::verify(bad);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->ok());
        CHECK(r->frames == 2);
    }
}

TEST_CASE("FR-W2 capture_file with full records", "[capture][file]")
{
    const std::string path = tmp_path("full.ser");
    {
        capture_file f(path);
        REQUIRE(f.is_open());
        const serialize_options full{.full_records = true};
        REQUIRE(serialize_with(full, f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize_with(full, f, corpus::Nested{{1, 2, 3, 4}, {5, 6, 7}, 8}));
    }
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    const auto rep = tools::verify(*bytes);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 2);
    CHECK(rep->records == 3);
    std::string text;
    REQUIRE(tools::dump(*bytes, nullptr, tools::dump_options{}, text));
    CHECK(text.find("corpus::Nested") != std::string::npos);
    CHECK(text.find("n: 8") != std::string::npos);
}

TEST_CASE("FR-W7 bare frame stream with a leading dictionary frame verifies", "[capture][stream]")
{
    std::vector<std::byte> stream;
    vector_sink out{stream};
    const auto dict = write_dictionary_frame(out);
    REQUIRE(dict.has_value());
    CHECK(*dict == stream.size());
    const auto h = read_frame_header(stream);
    REQUIRE(h.has_value());
    CHECK(h->dictionary());
    CHECK(h->call_hash == fmt::dict_frame_sentinel);
    REQUIRE(serialize(out, "evt"_lit, corpus::sample_order()));
    REQUIRE(serialize(out, corpus::sample_cancel()));
    const auto rep = tools::verify(stream);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames ==
          2); // 2.2.1: the dictionary frame is consumed, not counted (as the file reader did)
    CHECK(rep->records == 3);
    std::string js;
    tools::dump_options o;
    o.fmt = tools::format::json;
    REQUIRE(tools::dump(stream, nullptr, o, js));
    CHECK(js.find("\"id\":\"42\"") !=
          std::string::npos); // gk 1.12: 64-bit integers are quoted in JSON
    // Without any dictionary the same frames cannot be decoded ...
    const std::span<const std::byte> frames_only =
        std::span<const std::byte>(stream).subspan(*dict);
    const auto bare = tools::verify(frames_only);
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->ok());
    // ... unless the dictionary is supplied from the side (--elf / --dict path).
    const auto elf = es::serreg::test::embedded_dictionary_of_self();
    REQUIRE(elf.has_value());
    const auto with_elf = tools::verify(frames_only, &*elf);
    REQUIRE(with_elf.has_value());
    CHECK(with_elf->ok());
    CHECK(with_elf->records == 3);
}

TEST_CASE("FR-D6 extract_registry_from_elf(/proc/self/exe) matches the compile-time hashes",
          "[capture][elf]")
{
    const auto d = es::serreg::test::embedded_dictionary_of_self();
    REQUIRE(d.has_value());
    struct expect
    {
        const char* name;
        std::uint64_t schema, layout;
        std::uint32_t size;
    };
    const expect table[] = {
        {"corpus::Order", schema_hash_v<corpus::Order>, layout_hash_v<corpus::Order>,
         sizeof(corpus::Order)},
        {"corpus::Cancel", schema_hash_v<corpus::Cancel>, layout_hash_v<corpus::Cancel>,
         sizeof(corpus::Cancel)},
        {"corpus::Side", schema_hash_v<corpus::Side>, layout_hash_v<corpus::Side>,
         sizeof(corpus::Side)},
        {"corpus::Nested", schema_hash_v<corpus::Nested>, layout_hash_v<corpus::Nested>,
         sizeof(corpus::Nested)},
        {"corpus::Padded", schema_hash_v<corpus::Padded>, layout_hash_v<corpus::Padded>,
         sizeof(corpus::Padded)},
        {"corpus::Scalars", schema_hash_v<corpus::Scalars>, layout_hash_v<corpus::Scalars>,
         sizeof(corpus::Scalars)}, // component of Nested
        {"corpus::StampFlat", schema_hash_v<corpus::Stamp>, layout_hash_v<corpus::Stamp>,
         sizeof(corpus::StampFlat)}, // other TUs
        {"corpus::Vec<i32,3>", schema_hash_v<corpus::Vec3i>, layout_hash_v<corpus::Vec3i>,
         sizeof(corpus::Vec3i)},
        {"corpus::Local", schema_hash_v<Local>, layout_hash_v<Local>, sizeof(Local)},
    };
    for (const expect& e : table)
    {
        INFO(e.name);
        const dyn::type* const t = d->find_type_by_name(e.name);
        REQUIRE(t != nullptr);
        CHECK(t->schema_hash == e.schema);
        CHECK(t->layout_hash == e.layout);
        CHECK(t->size == e.size);
        CHECK(d->find_type(e.schema) == t);
    }
    const dyn::type* const order = d->find_type_by_name("corpus::Order");
    REQUIRE(order->fields.size() == 5);
    CHECK(order->fields[4].name == "sym");
    CHECK(order->fields[4].k == kind::array_);
    CHECK(order->fields[4].extent == 8);
    CHECK(d->find_literal(decltype("evt"_lit)::id) != nullptr);
    CHECK(d->find_call(call_hash_v<decltype("evt"_lit), corpus::Order>) != nullptr);
    // The ELF copy and the live registry describe the same set of types (the ELF may hold extra TU-local blobs).
    for (const registry_entry& n : registry::instance().snapshot().entries)
        if (n.nk == node_kind::type)
        {
            INFO(n.name);
            CHECK(d->find_type(n.hash) != nullptr);
        }
    CHECK_FALSE(tools::extract_registry_from_elf("/nonexistent/binary").has_value());
}

TEST_CASE("FR-R9 ES_SERREG_DUMP_REGISTRY_FD writes the RRI into a pipe at exit", "[capture][env]")
{
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        ::close(fds[0]);
        const std::string fd = std::to_string(fds[1]);
        ::setenv("ES_SERREG_DUMP_REGISTRY_FD", fd.c_str(), 1);
        ::setenv("ES_SERREG_DUMP_FORMAT", "rri", 1);
        registry::instance()
            .dump_if_requested(); // the same hook that runs from atexit; explicit here so the child can _exit
        ::close(fds[1]);
        ::_exit(0);
    }
    ::close(fds[1]);
    const std::vector<std::byte> img = read_all(fds[0]);
    ::close(fds[0]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(
        img ==
        registry::instance().export_image()); // byte-identical to the in-process export (§10.2 #6)
    dyn::dictionary d;
    REQUIRE(d.add_rri(img));
    CHECK(d.find_type_by_name("corpus::Order") != nullptr);
}
