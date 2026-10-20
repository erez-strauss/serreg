// es/serreg/sink.hpp — sink concepts and adaptors (spec §6.4, FR-S2). A `claiming_sink` hands out an
// exact-size slot (ring buffers, disruptor sequences) so serialize() writes in place with no intermediate copy.
// A sink may additionally expose `status state() const`; serialize() then reports that error instead of
// error::sink_full (fd_sink does: io_error vs. a bounded buffer being full).
#pragma once
#include <es/serreg/error.hpp>

#include <unistd.h>

#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace es::serreg
{

template<class S>
concept sink = requires(S& s, const std::byte* p, std::size_t n) {
    { s.write(p, n) } -> std::same_as<bool>; // false ⇒ error::sink_full, or state().error()
};

// 2.2.1: a sink type that names a dictionary scope (`using dictionary_scope = Tag;`): every call signature
// serialised into it registers under that scope, and registry::export_image_scope() lists exactly those calls
// and their types — a log sink's dictionary (registry.hpp, log.hpp).
template<class S>
concept scoped_sink = sink<S> && requires { typename S::dictionary_scope; };

template<class S>
concept claiming_sink = sink<S> && requires(S& s, std::size_t n) {
    { s.claim(n) } -> std::same_as<std::byte*>; // nullptr ⇒ full
    { s.commit(n) }; // void, or (2.1.12, review C-09) bool: false ⇒ the frame was not written
};

namespace detail
{
// Commits a claimed frame; false when a bool-returning commit() refused it (2.1.12, review C-09).
template<claiming_sink S>
bool commit_claimed(S& out, const std::size_t n) noexcept(noexcept(out.commit(n)))
{
    if constexpr (std::is_same_v<decltype(out.commit(n)), bool>)
        return out.commit(n);
    else
    {
        out.commit(n);
        return true;
    }
}
} // namespace detail

// Writes into a caller-provided span; `size()` is the number of bytes used so far.
struct span_sink
{
    std::span<std::byte> buf;
    std::size_t used = 0;
    explicit span_sink(const std::span<std::byte> b) noexcept : buf(b)
    {
    }
    bool write(const std::byte* const p, const std::size_t n) noexcept
    {
        if (n > buf.size() - used)
            return false; // no wrap (gk 1.16)
        if (n)
            std::memcpy(buf.data() + used, p, n);
        used += n;
        return true;
    }
    std::byte* claim(const std::size_t n) noexcept
    {
        return n <= buf.size() - used ? buf.data() + used : nullptr;
    }
    void commit(const std::size_t n) noexcept
    {
        used += n;
    }
    std::span<const std::byte> bytes() const noexcept
    {
        return buf.first(used);
    }
};

// Appends to a std::vector<std::byte>. Allocates, so it is not noexcept: serialize() into it is not noexcept
// either (cc F6) and a failed allocation propagates as std::bad_alloc.
struct vector_sink
{
    std::vector<std::byte>& v;
    explicit vector_sink(std::vector<std::byte>& out) noexcept : v(out)
    {
    }
    bool write(const std::byte* const p, const std::size_t n)
    {
        v.insert(v.end(), p, p + n);
        return true;
    }
    std::byte* claim(const std::size_t n)
    {
        const std::size_t at = v.size();
        v.resize(at + n);
        return v.data() + at;
    }
    void commit(std::size_t) noexcept
    {
    }
};

namespace detail
{
// One write loop for every descriptor in the library: retries EINTR, treats a zero-length write as an error.
inline status write_fd(const int fd, const std::byte* p, std::size_t n) noexcept
{
    while (n)
    {
        const ssize_t r = ::write(fd, p, n);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(error::io_error);
        }
        if (r == 0)
            return std::unexpected(error::io_error);
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return {};
}

// The library's diagnostic line on stderr (2.1.9), formatted like std::println. std::println itself formats into a
// std::string and throws std::system_error when the write fails; these lines are written from destructors, from
// libserreg's load-time constructor and from inside dlclose(), in processes that may have closed stderr, so this
// formats into a stack buffer (std::format_to_n: no allocation) and writes it with write_fd (no exception). A line
// longer than 1023 bytes is truncated. Applications and tools use std::println.
template<class... Args>
void report_line(const std::format_string<Args...> fmt, Args&&... args) noexcept
{
    char buf[1024];
    const auto r =
        std::format_to_n(buf, std::ptrdiff_t{sizeof buf - 1}, fmt, std::forward<Args>(args)...);
    const auto n = static_cast<std::size_t>(r.out - buf); // format_to_n stops at the limit
    buf[n] = '\n';
    static_cast<void>(write_fd(2, reinterpret_cast<const std::byte*>(buf), n + 1));
}
} // namespace detail

// Writes to a blocking file descriptor (pipe, socket, file). Not seekable: dictionary updates go out as
// dictionary frames (capture.hpp). Failures are io_error (never sink_full) and are sticky until reset().
struct fd_sink
{
    int fd;
    explicit fd_sink(const int d) noexcept : fd(d)
    {
    }
    bool write(const std::byte* const p, const std::size_t n) noexcept
    {
        if (!st_)
            return false;
        st_ = detail::write_fd(fd, p, n);
        return st_.has_value();
    }
    status state() const noexcept
    {
        return st_;
    }
    void reset() noexcept
    {
        st_ = {};
    }

private:
    status st_;
};

} // namespace es::serreg
