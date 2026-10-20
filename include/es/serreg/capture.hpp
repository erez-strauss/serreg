// es/serreg/capture.hpp — the FILE container and stream dictionary frames (spec §6.5, FR-W1, FR-W7; format 1.7).
//
//   capture_file f;
//   if (!f.open("orders.ser")) …               // header + opening dictionary + CHAIN (+ writer-info frame)
//   serialize(f, "new order"_lit, order);      // frames (f is a claiming sink over its own buffer)
//   if (!f.close()) …                          // flush; nothing is rewritten (1.7: no back-patching)
//
// 1.7 (2.2.1): a capture is self-describing up to its last complete frame, at any instant. When the registry
// moved since the last dictionary written (a dlopen), the next frame is preceded by a dictionary frame holding the
// *delta* (as streams always did), and the previous dictionary's `next` pointer — the opening block's CHAIN or the
// 8 bytes at the end of the previous dictionary frame — is patched with one aligned 8-byte pwrite, so a reader
// collects the whole dictionary by following the chain without a scan. Optional, per file, zero bytes per frame
// when off (capture_options): a CRC32C trailer on every frame, checkpoint frames (their own forward chain, for
// seeking into a multi-TB file), a writer-info frame (the options and versions the file was written with), and a
// sync policy.
//
//   dictionary_tracker dt;  dt.update(fd_sink);   // streams/pipes: emits a dictionary frame the first time and
//                                                 // whenever registry::version() changed since the last one (a
//                                                 // delta since 1.7); one relaxed atomic load when nothing changed
// capture_file is not thread-safe (one writer).
#pragma once
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/serialize.hpp>
#include <es/serreg/sink.hpp>
#include <es/serreg/version.hpp>

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace es::serreg
{

// 1.7: when the writer flushes to the device (fdatasync). `dictionary` (the default) makes a dictionary delta
// durable before the frames that need it; `checkpoint` also syncs at every checkpoint; `frame` after every frame
// (slow: for tests and for writers that must not lose a message); `none` leaves it to the OS and close().
enum class sync_policy : std::uint8_t
{
    none,
    dictionary,
    checkpoint,
    frame
};

// 1.7: per-file options. Every default is "as 1.6": no CRC, no checkpoints; the writer-info frame is on because it
// costs one frame per file and is what `srdump --info` reports.
struct capture_options
{
    bool crc = false; // every data frame carries a CRC32C trailer (frame flag crc)
    std::uint64_t checkpoint_bytes =
        0; // a checkpoint frame every N bytes of frames (0: none by size)
    std::chrono::nanoseconds checkpoint_every{
        0}; // … or every T (0: none by time; checked every 64 frames)
    bool checkpoint_dictionary =
        false;               // each checkpoint carries the full dictionary (seek without the chain)
    bool writer_info = true; // the writer-info frame after the opening dictionary
    sync_policy sync = sync_policy::dictionary;
    std::string_view program{}; // recorded in the writer-info frame (argv[0], a service name)
    bool info_host =
        true; // hostname, pid and start time in the writer-info frame (off: reproducible files)
    bool info_thread =
        false; // the opening thread's id in the writer-info frame (a logger's thread_policy::once)
    std::string_view
        log_clock{}; // the loggers' clock, recorded for readers: "system" | "steady" | "tsc" | "user"
    std::uint64_t (*clock)() noexcept =
        nullptr; // wall clock for checkpoints and the info frame; nullptr: system
};

// Compile-time policy of a capture file (an NTTP, so the choices are `if constexpr`):
//   dictionary_updates — false for a writer whose registry cannot move (no dlopen): the per-frame version compare
//                        (one relaxed load) disappears, and so does the chain machinery.
struct capture_policy
{
    bool dictionary_updates = true;
};

namespace detail
{
inline std::vector<std::byte> file_header(const std::uint64_t program_hash,
                                          const std::uint64_t dict_offset,
                                          const std::uint64_t dict_len,
                                          const std::uint32_t extra_flags)
{
    std::vector<std::byte> h(fmt::file_header_size);
    fmt::writer w{h.data()};
    w.put_bytes(fmt::file_magic.data(), 4);
    w.put(fmt::format_major);
    w.put(fmt::format_minor);
    w.put(fmt::file_flag_le | fmt::file_flag_dict | extra_flags);
    w.put(fmt::hash_algo_xxh3_64);
    w.put(program_hash);
    w.put(dict_offset);
    w.put(dict_len);
    w.put(fmt::host_abi_tag); // format 1.3: the writer's ABI (FR-W4)
    return h;
}

// A dictionary frame (1.7 layout): header | RRI | pad | u64 next (0). `rri` is a full image or a delta.
inline std::vector<std::byte> dictionary_frame_bytes(const std::span<const std::byte> rri)
{
    const std::size_t total =
        fmt::frame_header_size + fmt::align8(rri.size()) + fmt::dict_frame_link_size;
    std::vector<std::byte> buf(total);
    fmt::writer w{buf.data()};
    w.put(static_cast<std::uint32_t>(total));
    w.put(fmt::frame_flag_dict);
    w.put(std::uint16_t{0});
    w.put(fmt::dict_frame_sentinel);
    w.put_bytes(rri.data(), rri.size());
    return buf; // the pad and the link are the zero bytes the vector was created with
}

// A meta frame with a sentinel call hash and a payload (checkpoint, writer info); padded to 8.
inline std::vector<std::byte> meta_frame_bytes(const std::uint16_t flags,
                                               const std::uint64_t sentinel,
                                               const std::span<const std::byte> payload)
{
    const std::size_t total = fmt::frame_header_size + fmt::align8(payload.size());
    std::vector<std::byte> buf(total);
    fmt::writer w{buf.data()};
    w.put(static_cast<std::uint32_t>(total));
    w.put(flags);
    w.put(std::uint16_t{0});
    w.put(sentinel);
    w.put_bytes(payload.data(), payload.size());
    return buf;
}

// The writer-info payload: u16 version, u16 n_items, ITEM × n (u16 key, u16 len, bytes, pad to 2).
// "ptr8 long8 wchar4 char-signed max-align16 ldbl16 le": the long form of the ABI tag for the writer-info frame
// (the same text srdump prints for a dictionary's tag).
inline std::string abi_long_text(const std::uint64_t tag)
{
    const fmt::abi_tag t = fmt::unpack_abi(tag);
    if (t.version != 1)
        return "unknown(version " + std::to_string(t.version) + ")";
    return "ptr" + std::to_string(t.ptr_size) + " long" + std::to_string(t.long_size) + " wchar" +
           std::to_string(t.wchar_size) + (t.char_signed ? " char-signed" : " char-unsigned") +
           " max-align" + std::to_string(t.max_align) + " ldbl" +
           std::to_string(t.long_double_size) + (t.endian ? " be" : " le");
}

struct info_builder
{
    std::vector<std::byte> v = std::vector<std::byte>(4);
    std::uint16_t n = 0;
    void add(const fmt::info_key key, const std::string_view text)
    {
        if (text.size() > 0xFFFF)
            return;
        const auto put16 = [&](const std::uint16_t x) {
            const auto a = std::bit_cast<std::array<std::byte, 2>>(x);
            v.insert(v.end(), a.begin(), a.end());
        };
        put16(static_cast<std::uint16_t>(key));
        put16(static_cast<std::uint16_t>(text.size()));
        for (const char c : text)
            v.push_back(static_cast<std::byte>(c));
        if (text.size() % 2)
            v.push_back(std::byte{0});
        ++n;
    }
    std::vector<std::byte> finish()
    {
        const auto ver = std::bit_cast<std::array<std::byte, 2>>(std::uint16_t{1});
        const auto cnt = std::bit_cast<std::array<std::byte, 2>>(n);
        v[0] = ver[0];
        v[1] = ver[1];
        v[2] = cnt[0];
        v[3] = cnt[1];
        return std::move(v);
    }
};
inline std::string hostname_text()
{
    char buf[256]{};
    if (::gethostname(buf, sizeof buf - 1) != 0)
        return {};
    return buf;
}
inline std::uint64_t system_wall_clock_ns() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
} // namespace detail

// A dictionary frame on any sink: the full image, or (1.7) the delta since `since_version`.
template<sink S>
result<std::size_t> write_dictionary_frame(S& out, const std::uint64_t since_version = 0)
{
    const auto rri = registry::instance().export_image_since(since_version);
    const auto buf = detail::dictionary_frame_bytes(rri);
    if (!out.write(buf.data(), buf.size()))
        return std::unexpected(detail::sink_failure(out));
    return buf.size();
}

// Remembers the registry version at the last dictionary frame written to a stream (FR-W7 for non-seekable
// sinks). Call update() from the writer's idle/heartbeat path; returns true when a frame was written. The first
// frame is the full image, later ones are deltas (1.7).
struct dictionary_tracker
{
    std::uint64_t last_version = 0; // 0 = never written
    template<sink S> result<bool> update(S& out)
    {
        const std::uint64_t v = registry::instance().version();
        if (last_version != 0 && v == last_version)
            return false;
        if (const auto r = write_dictionary_frame(out, last_version); !r)
            return std::unexpected(r.error());
        last_version = v;
        return true;
    }
};

namespace detail
{
// 2.2.1: a scoped capture file declares its dictionary_scope (sink.hpp scoped_sink); void = the whole registry
template<class Scope> struct scope_decl
{
    using dictionary_scope = Scope;
};
template<> struct scope_decl<void>
{
};
} // namespace detail

// Seekable file container. A claiming sink: frames are assembled in an internal buffer and flushed on commit.
// `Scope` is a dictionary scope tag (void: the whole registry is the file's dictionary; a tag: only the calls
// serialised into files of this type, closed over their types — see registry.hpp registrar_scoped) and `P` the
// compile-time policy (capture_policy). Both are part of the sink *type*, so the scope's registrations and the
// policy's `if constexpr` branches cost nothing per message.
template<class Scope = void, capture_policy P = capture_policy{}>
class basic_capture_file : public detail::scope_decl<Scope>
{
public:
    static constexpr bool scoped = !std::is_void_v<Scope>;
    basic_capture_file() = default;
    // Convenience constructor: failure is sticky — is_open() is false and state() says why (gk 1.8).
    explicit basic_capture_file(const std::string_view path, const capture_options& o = {})
    {
        static_cast<void>(open(path, o));
    }
    ~basic_capture_file()
    {
        if (f_ && !close())
            detail::report_line("es::serreg: capture_file: close failed ({})",
                                to_string(st_.error()));
    }
    basic_capture_file(const basic_capture_file&) = delete;
    basic_capture_file& operator=(const basic_capture_file&) = delete;

    [[nodiscard]] result<void> open(const std::string_view path, const capture_options& o = {})
    {
        if (f_)
        {
            if (const auto c = close(); !c)
                return c;
        }
        st_ = {};
        opt_ = o;
        pos_ = 0;
        data_frames_ = 0;
        data_bytes_ = 0;
        bytes_since_ckpt_ = 0;
        frames_since_clock_ = 0;
        last_ckpt_offset_ = 0;
        last_dict_offset_ = 0;
        const std::string p(path);
        f_ = std::fopen(p.c_str(), "wb");
        if (!f_)
            return fail(error::io_error);
        const registry& reg = registry::instance();
        dict_version_ = reg.version();
        const auto rri = export_dictionary(0);
        std::uint32_t flags = 0;
        if (opt_.crc)
            flags |= fmt::file_flag_crc_all;
        if (checkpoints_enabled())
            flags |= fmt::file_flag_checkpoints;
        if (opt_.writer_info)
            flags |= fmt::file_flag_info;
        const auto hdr =
            detail::file_header(reg.program_hash(), fmt::file_header_size, rri.size(), flags);
        if (!fwrite_all(hdr.data(), hdr.size()) || !fwrite_all(rri.data(), rri.size()) || !pad8())
            return fail(error::io_error);
        // the CHAIN: u64 next_dictionary, u64 first_checkpoint — zero until patched
        chain_offset_ = pos_;
        last_dict_offset_ = fmt::file_header_size;
        next_link_offset_ = chain_offset_;
        const std::byte zeros[fmt::chain_size]{};
        if (!fwrite_all(zeros, sizeof zeros))
            return fail(error::io_error);
        if (opt_.writer_info && !write_info())
            return fail(error::io_error);
        if (!sync_if(sync_policy::dictionary))
            return fail(error::io_error);
        start_ns_ = now();
        last_ckpt_ns_ = start_ns_;
        return {};
    }

    // sink + claiming_sink
    bool write(const std::byte* const p, const std::size_t n) noexcept
    {
        if (!f_ || !st_)
        {
            if (st_)
                st_ = std::unexpected(error::io_error); // not open
            return false;
        }
        if (!before_frame())
            return false;
        return fwrite_all(p, n) && after_frame(n);
    }
    std::byte* claim(const std::size_t n)
    { // may allocate (first frame / larger frame): honestly not noexcept
        if (!f_ || !st_)
        {
            if (st_)
                st_ = std::unexpected(error::io_error);
            return nullptr;
        }
        if (!before_frame())
            return nullptr;
        if (buf_.size() < n)
        {
            buf_.resize(n);
        } // grows once to the largest frame seen; not on the hot path afterwards
        return buf_.data();
    }
    // 2.1.12 (review C-09): the result reaches serialize(), so a frame the disk refused is not reported as
    // written (it used to surface only at the next call or at close()).
    bool commit(const std::size_t n) noexcept
    {
        return fwrite_all(buf_.data(), n) && after_frame(n);
    }
    // 1.7: serialize() asks; every frame of a {.crc = true} file carries the trailer
    bool frame_crc() const noexcept
    {
        return opt_.crc;
    }
    status state() const noexcept
    {
        return st_;
    }

    // Flushes and closes. 1.7: nothing is rewritten — dictionary deltas were written inline as they happened; a
    // file with checkpoints gets a final one so the chain ends at the last frame.
    [[nodiscard]] result<void> close()
    {
        if (!f_)
            return st_;
        if (st_ && checkpoints_enabled() && data_frames_ != 0)
            static_cast<void>(write_checkpoint());
        if (st_ && std::fflush(f_) != 0)
            st_ = std::unexpected(error::io_error);
        if (st_ && opt_.sync != sync_policy::none && ::fdatasync(::fileno(f_)) != 0)
            st_ = std::unexpected(error::io_error);
        const int rc = std::fclose(f_);
        f_ = nullptr;
        if (rc != 0 && st_)
            st_ = std::unexpected(error::io_error);
        return st_;
    }

    // 1.7: a checkpoint now (also written by the size/time policy and at close()).
    [[nodiscard]] result<void> checkpoint()
    {
        if (!f_ || !st_)
            return std::unexpected(st_ ? error::io_error : st_.error());
        if (!write_checkpoint())
            return std::unexpected(st_.error());
        return {};
    }

    std::size_t bytes_written() const noexcept
    {
        return pos_;
    }
    std::uint64_t data_frames() const noexcept
    {
        return data_frames_;
    }
    bool is_open() const noexcept
    {
        return f_ != nullptr;
    }
    const capture_options& options() const noexcept
    {
        return opt_;
    }

private:
    std::uint64_t now() const noexcept
    {
        return opt_.clock ? opt_.clock() : detail::system_wall_clock_ns();
    }
    bool checkpoints_enabled() const noexcept
    {
        return opt_.checkpoint_bytes != 0 || opt_.checkpoint_every.count() != 0;
    }
    // Before a data frame: a dictionary delta if the registry moved (one relaxed load otherwise).
    bool before_frame()
    {
        if constexpr (P.dictionary_updates)
        {
            const std::uint64_t v = registry::instance().version();
            if (v != dict_version_) [[unlikely]]
                return write_delta(v);
        }
        return true;
    }
    bool after_frame(const std::size_t n)
    {
        ++data_frames_;
        data_bytes_ += n;
        if (checkpoints_enabled())
        {
            bytes_since_ckpt_ += n;
            bool due = opt_.checkpoint_bytes != 0 && bytes_since_ckpt_ >= opt_.checkpoint_bytes;
            if (!due && opt_.checkpoint_every.count() != 0 && ++frames_since_clock_ >= 64)
            {
                frames_since_clock_ = 0;
                const std::uint64_t t = now();
                due =
                    t - last_ckpt_ns_ >= static_cast<std::uint64_t>(opt_.checkpoint_every.count());
            }
            if (due && !write_checkpoint())
                return false;
        }
        return sync_if(sync_policy::frame);
    }
    // The file's dictionary (full, or the scope's) — everything, or the delta since a registry version.
    std::vector<std::byte> export_dictionary(const std::uint64_t since) const
    {
        if constexpr (scoped)
            return registry::instance().export_image_scope(detail::scope_hash_v<Scope>, since);
        else
            return registry::instance().export_image_since(since);
    }
    bool write_delta(const std::uint64_t version)
    {
        const auto rri = export_dictionary(dict_version_);
        if (rri.size() <= fmt::rri_header_size)
        { // 2.2.1: the registry moved, but nothing of this file's dictionary did (another scope's types)
            dict_version_ = version;
            return true;
        }
        const auto buf = detail::dictionary_frame_bytes(rri);
        const std::uint64_t at = pos_;
        if (!fwrite_all(buf.data(), buf.size()))
            return false;
        if (!sync_if(sync_policy::dictionary) || !patch(next_link_offset_, at))
            return false;
        next_link_offset_ = at + buf.size() - fmt::dict_frame_link_size;
        last_dict_offset_ = at;
        dict_version_ = version;
        return true;
    }
    bool write_checkpoint()
    {
        std::vector<std::byte> payload(fmt::checkpoint_fixed_size);
        std::vector<std::byte> rri;
        if (opt_.checkpoint_dictionary)
            rri = export_dictionary(0);
        fmt::writer w{payload.data()};
        w.put(std::uint16_t{1});
        w.put(static_cast<std::uint16_t>(
            opt_.checkpoint_dictionary ? fmt::checkpoint_flag_full_dictionary : 0));
        w.put(static_cast<std::uint32_t>(rri.size()));
        w.put(last_ckpt_offset_); // prev
        w.put(std::uint64_t{0});  // next (patched by the next checkpoint)
        w.put(data_frames_);      // data frames before this checkpoint
        w.put(data_bytes_);       // their bytes
        w.put(now());
        w.put(last_dict_offset_);
        w.put(dict_version_);
        payload.insert(payload.end(), rri.begin(), rri.end());
        const auto buf =
            detail::meta_frame_bytes(fmt::frame_flag_checkpoint, fmt::checkpoint_sentinel, payload);
        const std::uint64_t at = pos_;
        if (!fwrite_all(buf.data(), buf.size()))
            return false;
        // link it: the previous checkpoint's `next` (payload offset 16 → frame offset 32), or the CHAIN's second word
        const std::uint64_t link =
            last_ckpt_offset_ ? last_ckpt_offset_ + fmt::frame_header_size + 16 : chain_offset_ + 8;
        if (!sync_if(sync_policy::checkpoint) || !patch(link, at))
            return false;
        last_ckpt_offset_ = at;
        bytes_since_ckpt_ = 0;
        last_ckpt_ns_ = now();
        return true;
    }
    bool write_info()
    {
        detail::info_builder b;
        b.add(fmt::info_library_version, library_version);
        b.add(fmt::info_format,
              std::to_string(fmt::format_major) + "." + std::to_string(fmt::format_minor));
        if (!opt_.program.empty())
            b.add(fmt::info_program, opt_.program);
        if (opt_.info_host)
        {
            b.add(fmt::info_hostname, detail::hostname_text());
            b.add(fmt::info_pid, std::to_string(::getpid()));
            b.add(fmt::info_start_wall_ns, std::to_string(now()));
        }
        if (opt_.info_thread)
            b.add(fmt::info_thread, std::to_string(::gettid()));
        if (!opt_.log_clock.empty())
            b.add(fmt::info_clock, opt_.log_clock);
        b.add(fmt::info_abi, detail::abi_long_text(fmt::host_abi_tag));
        b.add(fmt::info_crc, opt_.crc ? "1" : "0");
        b.add(fmt::info_checkpoint_bytes, std::to_string(opt_.checkpoint_bytes));
        b.add(fmt::info_checkpoint_ns, std::to_string(opt_.checkpoint_every.count()));
        if constexpr (scoped)
            b.add(fmt::info_dictionary, "scoped:" + std::string(detail::scope_name_v<Scope>));
        else
            b.add(fmt::info_dictionary, "full");
        b.add(fmt::info_sync, opt_.sync == sync_policy::none         ? "none"
                              : opt_.sync == sync_policy::dictionary ? "dictionary"
                              : opt_.sync == sync_policy::checkpoint ? "checkpoint"
                                                                     : "frame");
        const auto buf =
            detail::meta_frame_bytes(fmt::frame_flag_info, fmt::info_sentinel, b.finish());
        return fwrite_all(buf.data(), buf.size());
    }
    // One aligned 8-byte write at `at` (a chain link): the data must be flushed first so the patch lands after it.
    bool patch(const std::uint64_t at, const std::uint64_t value)
    {
        if (std::fflush(f_) != 0)
        {
            st_ = std::unexpected(error::io_error);
            return false;
        }
        if (::pwrite(::fileno(f_), &value, 8, static_cast<off_t>(at)) != 8)
        {
            st_ = std::unexpected(error::io_error);
            return false;
        }
        return true;
    }
    bool sync_if(const sync_policy level)
    {
        if (static_cast<int>(opt_.sync) < static_cast<int>(level))
            return true;
        if (std::fflush(f_) != 0 || ::fdatasync(::fileno(f_)) != 0)
        {
            st_ = std::unexpected(error::io_error);
            return false;
        }
        return true;
    }
    bool pad8()
    {
        if (pos_ % 8 == 0)
            return true;
        const std::byte z[8]{};
        return fwrite_all(z, 8 - pos_ % 8);
    }
    bool fwrite_all(const void* const p, const std::size_t n) noexcept
    {
        if (n && std::fwrite(p, 1, n, f_) != n)
        {
            st_ = std::unexpected(error::io_error);
            return false;
        }
        pos_ += n;
        return true;
    }
    result<void> fail(const error e) noexcept
    {
        st_ = std::unexpected(e);
        if (f_)
        {
            std::fclose(f_);
            f_ = nullptr;
        }
        return std::unexpected(e);
    }
    std::FILE* f_ = nullptr;
    std::size_t pos_ = 0;
    capture_options opt_{};
    std::uint64_t dict_version_ = 0;     // the registry version the last dictionary written covers
    std::uint64_t chain_offset_ = 0;     // the CHAIN after the opening dictionary
    std::uint64_t next_link_offset_ = 0; // where the next dictionary's offset is patched in
    std::uint64_t last_dict_offset_ =
        0; // the latest dictionary (opening block or frame), for checkpoints
    std::uint64_t last_ckpt_offset_ = 0; // 0 = none yet
    std::uint64_t data_frames_ = 0, data_bytes_ = 0;
    std::uint64_t bytes_since_ckpt_ = 0;
    std::uint32_t frames_since_clock_ = 0;
    std::uint64_t start_ns_ = 0, last_ckpt_ns_ = 0;
    status st_; // sticky: ok until the first failure (open, write, close)
    std::vector<std::byte> buf_;
};
using capture_file = basic_capture_file<>;
template<class Scope> using scoped_capture_file = basic_capture_file<Scope>;

} // namespace es::serreg
