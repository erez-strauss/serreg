// tests/unit/test_format17.cpp — format 1.7 (2.2.1, DECISIONS P-*): the dictionary chain and inline deltas
// (no back-patching), the CRC32C trailer, checkpoint frames, the writer-info frame, tools::info, --resync, and
// the reader's acceptance of 1.6 files beside 1.7 ones.
#include "corpus.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace es::serreg;
using namespace es::serreg::literals;

namespace
{
std::uint64_t fixed_clock() noexcept
{
    return 1'600'000'000'000'000'000ull;
}
std::uint64_t u64_at(const std::vector<std::byte>& b, const std::size_t at)
{
    std::uint64_t v{};
    std::memcpy(&v, b.data() + at, 8);
    return v;
}
std::uint32_t u32_at(const std::vector<std::byte>& b, const std::size_t at)
{
    std::uint32_t v{};
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
std::uint16_t u16_at(const std::vector<std::byte>& b, const std::size_t at)
{
    std::uint16_t v{};
    std::memcpy(&v, b.data() + at, 2);
    return v;
}
std::string tmp_path(const std::string& name)
{
    return std::string(ES_SERREG_TEST_TMPDIR) + "/" + name;
}
// A literal that no call site of this binary registers: pushed by hand between two frames, it moves the
// registry version the way a dlopen does (test_conflict.cpp pushes nodes the same way).
using late_lit = decltype("late arrival (1.7 delta)"_lit);
constinit reg_node late_node{nullptr,
                             detail::literal_blob_bytes_v<late_lit::str>,
                             late_lit::id,
                             {},
                             node_kind::literal,
                             nullptr,
                             nullptr,
                             0,
                             false};
} // namespace

TEST_CASE(
    "P 1.7 a capture file is written as 1.7: header, CHAIN, writer-info frame, no rewrite at close",
    "[format17][capture]")
{
    const std::string path = tmp_path("f17_plain.ser");
    capture_options co;
    co.clock = &fixed_clock;
    co.program = "test_format17";
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit));
        REQUIRE(f.close());
    }
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    const auto& b = *bytes;
    CHECK(u16_at(b, 4) == 1);
    CHECK(u16_at(b, 6) == 7);
    const std::uint32_t flags = u32_at(b, 8);
    CHECK((flags & fmt::file_flag_dict) != 0);
    CHECK((flags & fmt::file_flag_info) != 0);
    CHECK((flags & fmt::file_flag_backpatched) == 0);
    CHECK((flags & fmt::file_flag_crc_all) == 0);
    const std::uint64_t dict_off = u64_at(b, 24), dict_len = u64_at(b, 32);
    CHECK(dict_off == fmt::file_header_size);
    // the CHAIN after the opening dictionary: no delta, no checkpoint → two zero words
    const std::size_t chain = fmt::align8(static_cast<std::size_t>(dict_off + dict_len));
    CHECK(u64_at(b, chain) == 0);
    CHECK(u64_at(b, chain + 8) == 0);
    // then the writer-info frame
    const std::size_t info_at = chain + fmt::chain_size;
    CHECK((u16_at(b, info_at + 4) & fmt::frame_flag_info) != 0);
    CHECK(u64_at(b, info_at + 8) == fmt::info_sentinel);

    const auto rep = tools::verify(b);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 2); // data frames: meta frames are not counted
    CHECK(rep->records == 4);

    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    CHECK(ci->minor == 7);
    CHECK(ci->dictionary_offsets.size() == 1);
    CHECK(ci->checkpoints.empty());
    CHECK(ci->writer_item(fmt::info_library_version) == library_version);
    CHECK(ci->writer_item(fmt::info_format) == "1.7");
    CHECK(ci->writer_item(fmt::info_program) == "test_format17");
    CHECK(ci->writer_item(fmt::info_crc) == "0");
    CHECK(!ci->writer_item(fmt::info_hostname).empty());
    CHECK(ci->dict.find_type(schema_hash_v<corpus::Order>) != nullptr);
    const std::string text = tools::to_text(*ci);
    CHECK(text.find("format 1.7") != std::string::npos);
    CHECK(text.find("writer      program = test_format17") != std::string::npos);
    CHECK(tools::to_json(*ci).find("\"format\":\"1.7\"") != std::string::npos);
    // the windowed reader and the typed readers see the same two data frames
    std::size_t n = 0;
    const auto w = tools::for_each_frame(
        path, [&](const dyn::dictionary&, const tools::frame_ref&) { return ++n, true; });
    REQUIRE(w.has_value());
    CHECK(n == 2);
    ::unlink(path.c_str());
}

TEST_CASE(
    "P 1.7 a registry change between frames writes a dictionary delta inline and links the chain",
    "[format17][capture][chain]")
{
    const std::string path = tmp_path("f17_delta.ser");
    capture_options co;
    co.clock = &fixed_clock;
    co.info_host = false;
    const std::uint64_t v0 = registry::instance().version();
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        registry::instance().push(&late_node); // a dlopen's effect: the version moves
        REQUIRE(registry::instance().version() != v0);
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit)); // preceded by the delta
        REQUIRE(serialize(f, corpus::Side::buy));
        REQUIRE(f.close());
    }
    const auto bytes = tools::read_file(path);
    REQUIRE(bytes.has_value());
    const auto& b = *bytes;
    const std::uint64_t dict_off = u64_at(b, 24), dict_len = u64_at(b, 32);
    const std::size_t chain = fmt::align8(static_cast<std::size_t>(dict_off + dict_len));
    const std::uint64_t delta_at = u64_at(b, chain);
    REQUIRE(delta_at != 0);
    CHECK(delta_at < b.size());
    // the linked frame is a dictionary frame whose RRI is a delta holding the late literal and ending in a zero link
    CHECK((u16_at(b, static_cast<std::size_t>(delta_at) + 4) & fmt::frame_flag_dict) != 0);
    const std::uint32_t dlen = u32_at(b, static_cast<std::size_t>(delta_at));
    CHECK(u64_at(b, static_cast<std::size_t>(delta_at) + dlen - 8) == 0);
    const std::uint32_t rri_flags = u32_at(b, static_cast<std::size_t>(delta_at) + 16 + 8);
    CHECK((rri_flags & fmt::rri_flag_delta) != 0);
    CHECK(u32_at(b, static_cast<std::size_t>(delta_at) + 16 + 16) == 1); // one entry: the literal
    // the header still points at the opening dictionary; nothing was rewritten (no back-patched flag)
    CHECK(dict_off == fmt::file_header_size);
    CHECK((u32_at(b, 8) & fmt::file_flag_backpatched) == 0);

    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    REQUIRE(ci->dictionary_offsets.size() == 2);
    CHECK(ci->dictionary_offsets[1] == delta_at);
    REQUIRE(ci->dict.find_literal(late_lit::id) != nullptr);
    CHECK(ci->dict.find_literal(late_lit::id)->text == late_lit{}.text());
    // the sequential readers meet the delta inline
    const auto rep = tools::verify(path);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 3);
    const auto w = tools::for_each_frame(
        path, [](const dyn::dictionary&, const tools::frame_ref&) { return true; });
    REQUIRE(w.has_value());
    REQUIRE(w->dict.find_literal(late_lit::id) != nullptr);
    CHECK(w->dict.find_literal(late_lit::id)->text == late_lit{}.text());

    // 2.2.6: --verbose lists the dictionary blocks — the opening block and the delta, with what each added — and
    // marks the delta in the frame dump; the file (windowed) and the span walkers say the same
    tools::dump_options vo;
    vo.verbose = true;
    std::string by_path, by_span;
    REQUIRE(tools::dump(path, nullptr, vo, by_path, tools::block_options{}));
    REQUIRE(tools::dump(b, nullptr, vo, by_span));
    CHECK(by_path == by_span);
    CHECK(by_path.contains("dictionary blocks  2\n"));
    CHECK(by_path.contains("  opening block  @48  +"));
    // frame 0 is the writer-info frame, 1 the first order, 2 the delta, 3 the cancel
    const std::string delta_line = "  frame 2  dictionary frame  @" + std::to_string(delta_at) +
                                   "  +0 types, +1 literals, +0 calls\n";
    CHECK(by_path.contains(delta_line));
    CHECK(by_path.find(delta_line) !=
          by_path.rfind(delta_line));          // once in the table, once as the marker
    CHECK(by_path.contains("frame 3  call ")); // the data frame after the delta
    vo.dict_only = true;
    std::string dict_only;
    REQUIRE(tools::dump(path, nullptr, vo, dict_only, tools::block_options{}));
    CHECK(dict_only.contains("dictionary blocks  2\n"));
    CHECK_FALSE(dict_only.contains("frame 3  call "));
    vo.fmt = tools::format::json;
    std::string js;
    REQUIRE(tools::dump(path, nullptr, vo, js, tools::block_options{}));
    CHECK(js.contains("{\"frame\":null,\"offset\":48,\"dictionary\":{\"block\":\"opening\""));
    CHECK(
        js.contains("\"dictionary\":{\"block\":\"frame\",\"types\":0,\"literals\":1,\"calls\":0}"));
    // without --verbose nothing of this appears (the goldens stay as they are)
    std::string plain;
    REQUIRE(tools::dump(path, nullptr, tools::dump_options{}, plain, tools::block_options{}));
    CHECK_FALSE(plain.contains("dictionary blocks"));

    // 2.2.6: a dictionary link that points past the end of the file is a warning of tools::info, not silence
    {
        auto broken = b;
        const std::uint64_t beyond = b.size() + 100;
        std::memcpy(broken.data() + chain, &beyond, 8);
        const std::string bp = tmp_path("f17_delta_broken.ser");
        REQUIRE(tools::write_file(bp, broken));
        const auto bi = tools::info(bp);
        REQUIRE(bi.has_value());
        CHECK(bi->dictionary_offsets.size() == 1); // the chain ends at the bad link
        REQUIRE(bi->warnings.size() == 1);
        CHECK(bi->warnings[0].contains("dictionary chain: link @" + std::to_string(chain)));
        CHECK(bi->warnings[0].contains("points past the end of the file"));
        CHECK(tools::to_text(*bi).contains("warning     dictionary chain"));
        CHECK(tools::to_json(*bi).contains("\"warnings\":[\"dictionary chain"));
        // and a link that goes backwards (a loop in a corrupt file)
        const std::uint64_t back = 8;
        std::memcpy(broken.data() + chain, &back, 8);
        REQUIRE(tools::write_file(bp, broken));
        const auto li = tools::info(bp);
        REQUIRE(li.has_value());
        REQUIRE(li->warnings.size() == 1);
        CHECK(li->warnings[0].contains("does not go forward"));
        ::unlink(bp.c_str());
    }
    CHECK(ci->warnings.empty()); // the intact file has none
    ::unlink(path.c_str());
}

TEST_CASE(
    "P 1.7 CRC32C trailers: written per frame, checked by every reader, a corrupt frame is skipped",
    "[format17][crc]")
{
    STATIC_REQUIRE(crc32c(std::string_view("123456789")) ==
                   0xE3069283u); // the standard check value
    const std::string path = tmp_path("f17_crc.ser");
    capture_options co;
    co.crc = true;
    co.clock = &fixed_clock;
    co.info_host = false;
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(serialize(f, corpus::sample_cancel(), "why"_lit));
        REQUIRE(serialize(f, corpus::Side::buy));
        REQUIRE(f.close());
    }
    auto bytes = *tools::read_file(path);
    CHECK((u32_at(bytes, 8) & fmt::file_flag_crc_all) != 0);
    // every data frame: flag bit 2 set, frame_len includes the 8-byte trailer, the trailer verifies
    std::size_t n = 0, first_data = 0;
    const auto w = tools::for_each_frame(std::span<const std::byte>(bytes),
                                         [&](const dyn::dictionary&, const tools::frame_ref& fr) {
                                             std::uint16_t fl{};
                                             std::memcpy(&fl, fr.bytes.data() + 4, 2);
                                             CHECK((fl & fmt::frame_flag_crc) != 0);
                                             if (!first_data)
                                                 first_data = fr.offset;
                                             ++n;
                                             return true;
                                         });
    REQUIRE(w.has_value());
    CHECK(n == 3);
    CHECK(w->problems.empty());
    const auto h = read_frame_header(std::span<const std::byte>(bytes).subspan(first_data));
    REQUIRE(h.has_value());
    CHECK(h->crc());
    CHECK(h->frame_len ==
          frame_size<decltype("evt"_lit), corpus::Order>(serialize_options{.crc = true}));
    CHECK(h->payload_end() == h->frame_len - fmt::crc_trailer_size);
    corpus::Order o{};
    CHECK(deserialize(std::span<const std::byte>(bytes).subspan(first_data), "evt"_lit, o)
              .has_value());
    CHECK(o.id == corpus::sample_order().id);
    // flip one payload byte of the first data frame: the span reader, the windowed reader and the typed reader
    // all report crc_mismatch, and the two walkers skip that frame and go on
    bytes[first_data + fmt::frame_header_size + 2] ^= std::byte{0x40};
    const auto rep = tools::verify(bytes);
    REQUIRE(rep.has_value());
    CHECK_FALSE(rep->ok());
    CHECK(rep->frames == 2);
    REQUIRE(!rep->problems.empty());
    CHECK(rep->problems[0].find("crc mismatch") != std::string::npos);
    REQUIRE(tools::write_file(path, bytes));
    const auto repf = tools::verify(path);
    REQUIRE(repf.has_value());
    CHECK_FALSE(repf->ok());
    CHECK(repf->frames == 2);
    CHECK(
        deserialize(std::span<const std::byte>(bytes).subspan(first_data), "evt"_lit, o).error() ==
        error::crc_mismatch);
    CHECK(read_frame_header(std::span<const std::byte>(bytes).subspan(first_data)).error() ==
          error::crc_mismatch);
    ::unlink(path.c_str());
}

TEST_CASE("P 1.7 checkpoint frames: a forward chain by size, a final one at close, tools::info "
          "follows it",
          "[format17][checkpoint]")
{
    const std::string path = tmp_path("f17_ckpt.ser");
    capture_options co;
    co.checkpoint_bytes = 200;
    co.clock = &fixed_clock;
    co.info_host = false;
    std::size_t frames = 0;
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        for (int i = 0; i < 12; ++i, ++frames)
            REQUIRE(serialize(f, "evt"_lit, corpus::sample_order())); // 64 bytes each
        REQUIRE(f.close());
    }
    const auto bytes = *tools::read_file(path);
    CHECK((u32_at(bytes, 8) & fmt::file_flag_checkpoints) != 0);
    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    REQUIRE(ci->checkpoints.size() >= 3); // every ≥ 200 bytes (4 frames) plus the final one
    const std::uint64_t dict_off = u64_at(bytes, 24), dict_len = u64_at(bytes, 32);
    const std::size_t chain = fmt::align8(static_cast<std::size_t>(dict_off + dict_len));
    CHECK(u64_at(bytes, chain + 8) == ci->checkpoints.front().offset); // the CHAIN's second word
    for (std::size_t i = 0; i < ci->checkpoints.size(); ++i)
    {
        const auto& c = ci->checkpoints[i];
        CHECK(c.prev == (i ? ci->checkpoints[i - 1].offset : 0));
        CHECK(c.next == (i + 1 < ci->checkpoints.size() ? ci->checkpoints[i + 1].offset : 0));
        CHECK(c.wall_clock_ns == fixed_clock());
        CHECK(c.dictionary_offset == fmt::file_header_size);
        CHECK(!c.full_dictionary);
        if (i)
            CHECK(c.data_frames_before > ci->checkpoints[i - 1].data_frames_before);
    }
    CHECK(ci->checkpoints.back().data_frames_before ==
          frames); // the final checkpoint covers every frame
    CHECK(ci->checkpoints.back().bytes_before ==
          frames * frame_size<decltype("evt"_lit), corpus::Order>());
    const auto rep = tools::verify(path);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == frames);
    // a dump shows the data frames only (the meta frames count in the numbering: --info lists them)
    std::string text;
    REQUIRE(tools::dump(bytes, nullptr, tools::dump_options{}, text));
    CHECK(text.find("checkpoint") == std::string::npos);
    CHECK(text.find("info frame") == std::string::npos);
    CHECK(text.find("frame 1  call") != std::string::npos); // frame 0 is the writer-info frame
    ::unlink(path.c_str());
}

TEST_CASE("P 1.7 --resync: after a corrupt frame_len the windowed reader continues at the next "
          "checkpoint",
          "[format17][resync]")
{
    const std::string path = tmp_path("f17_resync.ser");
    capture_options co;
    co.checkpoint_bytes = 200;
    co.clock = &fixed_clock;
    co.info_host = false;
    {
        capture_file f(path, co);
        REQUIRE(f.is_open());
        for (int i = 0; i < 12; ++i)
            REQUIRE(serialize(f, "evt"_lit, corpus::sample_order()));
        REQUIRE(f.close());
    }
    auto bytes = *tools::read_file(path);
    const auto ci = tools::info(path);
    REQUIRE(ci.has_value());
    REQUIRE(ci->checkpoints.size() >= 2);
    // corrupt the frame_len of the second data frame (the first frame after the info frame is data frame 0)
    std::size_t second = 0, k = 0;
    REQUIRE(
        tools::for_each_frame(std::span<const std::byte>(bytes), [&](const dyn::dictionary&,
                                                                     const tools::frame_ref& fr) {
            if (k++ == 1)
                second = fr.offset;
            return second == 0;
        }).has_value());
    REQUIRE(second != 0);
    const std::uint32_t bad = 0x7FFFFFF8u;
    std::memcpy(bytes.data() + second, &bad, 4);
    REQUIRE(tools::write_file(path, bytes));
    // without --resync the walk stops there
    tools::block_options bo;
    const auto stop = tools::verify(path, nullptr, bo);
    REQUIRE(stop.has_value());
    CHECK_FALSE(stop->ok());
    CHECK(stop->frames == 1);
    // with --resync it skips to the first checkpoint after the damage and reads the rest
    bo.resync = true;
    const auto go = tools::verify(path, nullptr, bo);
    REQUIRE(go.has_value());
    CHECK_FALSE(go->ok()); // the skipped range is a problem
    CHECK(go->frames > 1);
    CHECK(go->frames < 12);
    bool noted = false;
    for (const auto& p : go->problems)
        noted = noted || p.find("skipped") != std::string::npos;
    CHECK(noted);
    ::unlink(path.c_str());
}

TEST_CASE("P 1.7 the readers still read 1.6 files, and refuse 1.5 and 1.8", "[format17][compat]")
{
    const std::string dir = std::string(ES_SERREG_TEST_SOURCE_DIR) + "/../fixtures/corpus/capture/";
    const auto old = tools::read_file(dir + "orders-1.6.ser");
    REQUIRE(old.has_value());
    CHECK(u16_at(*old, 6) == 6);
    const auto rep = tools::verify(*old);
    REQUIRE(rep.has_value());
    CHECK(rep->ok());
    CHECK(rep->frames == 11);
    const auto ci = tools::info(dir + "orders-1.6.ser");
    REQUIRE(ci.has_value());
    CHECK(ci->minor == 6);
    CHECK(ci->checkpoints.empty());
    CHECK(!ci->has_writer_info());
    CHECK(ci->dict.types.size() == 3);
    for (const unsigned minor : {5u, 8u})
    {
        std::vector<std::byte> other(*old);
        other[6] = static_cast<std::byte>(minor);
        CHECK(tools::verify(other).error() == error::bad_version);
    }
    // a 1.6 RRI is read; its minor is kept inside the dictionary frame payloads of 1.6 streams
    const auto rri = tools::load_dictionary_file(std::string(ES_SERREG_TEST_SOURCE_DIR) +
                                                 "/../fixtures/corpus/rri/orders-1.6.rri");
    REQUIRE(rri.has_value());
    CHECK(rri->types.size() == 3);
}

TEST_CASE("P 1.7 serialize_options{.crc} on a plain sink, and frame_size accounts for the trailer",
          "[format17][crc][sink]")
{
    std::vector<std::byte> buf;
    vector_sink out{buf};
    const auto n =
        serialize_with(serialize_options{.crc = true}, out, "evt"_lit, corpus::sample_order());
    REQUIRE(n.has_value());
    CHECK(*n == frame_size<decltype("evt"_lit), corpus::Order>() + fmt::crc_trailer_size);
    CHECK(*n == frame_size<decltype("evt"_lit), corpus::Order>(serialize_options{.crc = true}));
    const auto h = read_frame_header(buf);
    REQUIRE(h.has_value());
    CHECK(h->crc());
    corpus::Order o{};
    CHECK(deserialize(buf, "evt"_lit, o).has_value());
    // a runtime-sized frame (a string argument) gets the trailer too
    buf.clear();
    const std::string s(5000, 'x'); // beyond the small stack buffer: the heap path
    REQUIRE(serialize_with(serialize_options{.crc = true}, out, s, 42));
    const auto h2 = read_frame_header(buf);
    REQUIRE(h2.has_value());
    CHECK(h2->crc());
    std::string back;
    int i = 0;
    CHECK(deserialize(buf, back, i).has_value());
    CHECK(back == s);
    CHECK(i == 42);
}
