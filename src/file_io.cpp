// src/file_io.cpp — POSIX file helpers of the tools (tools::read_file / write_file / write_all and the internal
// write_all used by export and the env dump). Split out of elf_reader.cpp in 2.1.8 (review addendum A4): they
// have nothing to do with the executable format. Inputs are capped at decode_limits::input_bytes (gk 1.15; 2.1.16).
#include "internal.hpp"

#include <es/serreg/tools.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace es::serreg::tools
{
namespace
{

// Reads all of `fd` into a vector (regular files, pipes and stdin alike); error::unsupported above the cap.
result<std::vector<std::byte>> slurp(const int fd)
{
    std::vector<std::byte> out;
    std::size_t used = 0;
    for (;;)
    {
        if (out.size() - used < 65536)
        {
            if (out.size() >= internal::input_limit())
                return std::unexpected(error::unsupported);
            out.resize(std::min(out.size() + (std::size_t{1} << 20), internal::input_limit()));
        }
        const ssize_t n = ::read(fd, out.data() + used, out.size() - used);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(error::io_error);
        }
        if (n == 0)
            break;
        used += static_cast<std::size_t>(n);
        if (used == internal::input_limit())
        { // exactly full: anything more is over the cap
            std::byte probe;
            ssize_t m;
            do
                m = ::read(fd, &probe, 1);
            while (m < 0 && errno == EINTR);
            if (m != 0)
                return std::unexpected(m < 0 ? error::io_error : error::unsupported);
            break;
        }
    }
    out.resize(used);
    return out;
}

} // namespace

result<std::vector<std::byte>> read_file(const std::string_view path)
{
    if (path == "-")
        return slurp(STDIN_FILENO);
    const std::string p(path);
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(errno == ENOENT ? error::not_found : error::io_error);
    auto r = slurp(fd);
    ::close(fd);
    return r;
}

result<void> write_all(const int fd, const std::span<const std::byte> data) noexcept
{
    return internal::write_all(fd, data.data(), data.size());
}

result<void> write_file(const std::string_view path, const std::span<const std::byte> data)
{
    const std::string p(path);
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(error::io_error);
    const auto w = write_all(fd, data);
    if (!w)
    {
        ::close(fd);
        return w;
    }
    return ::close(fd) == 0 ? result<void>{} : std::unexpected(error::io_error);
}

} // namespace es::serreg::tools

namespace es::serreg::internal
{

result<void> write_all(const int fd, const void* const data, const std::size_t size) noexcept
{
    const auto* p = static_cast<const std::byte*>(data);
    std::size_t done = 0;
    while (done < size)
    {
        const ssize_t n = ::write(fd, p + done, size - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return std::unexpected(error::io_error);
        }
        if (n == 0)
            return std::unexpected(
                error::io_error); // never spin on a descriptor that accepts nothing
        done += static_cast<std::size_t>(n);
    }
    return {};
}

} // namespace es::serreg::internal
