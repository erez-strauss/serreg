// es/ipc/region.hpp — memory for the es::ipc rings (v2.0.5): a named file on tmpfs or hugetlbfs mapped
// MAP_SHARED (between unrelated processes), or an anonymous shared mapping (threads, fork children).
//
//   auto r = es::ipc::region::create("orders", 1 << 20);                              // /dev/shm/orders
//   auto h = es::ipc::region::create("orders", 1 << 20, {.pages = es::ipc::pages::huge_2m, .lock = true});
//   auto o = es::ipc::region::open("orders");          // another process: /dev/shm, then every hugetlbfs mount
//   auto a = es::ipc::region::anonymous(1 << 20, {.pages = es::ipc::pages::huge_2m});
//
// Why not shm_open(): on Linux it is open("/dev/shm/<name>") and nothing more, and it cannot reach hugetlbfs,
// which shared huge pages need. The one thing it guarantees — memory that is RAM, not a disk file with writeback
// — is kept here by fstatfs(): a region is accepted only on tmpfs (normal pages) or hugetlbfs (huge pages of the
// requested size). No transparent huge pages: huge pages are explicit, from the pool the administrator reserved
// (vm.nr_hugepages / hugepagesz=1G hugepages=N), and a missing reservation is an error, never a silent fallback.
//
// Every mapping is MAP_POPULATE (no page faults on first touch); `.lock = true` also mlock()s it. The creator
// owns the name and unlinks it when its region is closed or destroyed (unless keep()). Move-only.
#pragma once

#include "common.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

namespace es::ipc
{

enum class pages : std::uint8_t
{
    normal,  // system page size, tmpfs (/dev/shm) for named regions
    huge_2m, // 2 MiB huge pages, hugetlbfs mount with pagesize=2M for named regions
    huge_1g, // 1 GiB huge pages, hugetlbfs mount with pagesize=1G
};

constexpr std::size_t page_bytes(const pages p) noexcept
{
    return p == pages::huge_2m   ? std::size_t{2} << 20
           : p == pages::huge_1g ? std::size_t{1} << 30
                                 : 0;
}
constexpr std::string_view to_string(const pages p) noexcept
{
    return p == pages::huge_2m ? "huge_2m" : p == pages::huge_1g ? "huge_1g" : "normal";
}

struct region_options
{
    ipc::pages pages = ipc::pages::normal;
    bool lock = false; // mlock() the mapping (needs RLIMIT_MEMLOCK or CAP_IPC_LOCK)
    std::string_view
        dir{}; // directory to use instead of the default for `pages`; must be tmpfs or hugetlbfs
    unsigned mode = 0600; // file mode of a created named region
    // open(): accept a file owned by another user, or writable by its group or by others (2.1.12, review I-08).
    // Off by default: /dev/shm is world-writable, so anyone can create a file under the name a reader expects.
    bool foreign_owner = false;
};

class region
{
public:
    static constexpr std::size_t max_path = 256;
    static constexpr long tmpfs_magic = 0x01021994;     // TMPFS_MAGIC
    static constexpr long hugetlbfs_magic = 0x958458f6; // HUGETLBFS_MAGIC

    // A new named region of at least `bytes` (rounded up to the page size). errc::file_exists if the name is
    // taken; errc::not_enough_memory when the huge-page pool cannot back it; errc::no_such_file_or_directory
    // when no hugetlbfs mount of that page size exists; errc::permission_denied when the directory is not
    // writable by this user (a hugetlbfs mount is root:root 0755 unless mounted with uid=/gid=/mode=);
    // errc::invalid_argument for a bad name or a directory that is not tmpfs/hugetlbfs of the requested size.
    static std::expected<region, std::errc> create(const std::string_view name,
                                                   const std::size_t bytes,
                                                   const region_options& o = {}) noexcept
    {
        region r;
        if (bytes == 0)
            return std::unexpected(std::errc::invalid_argument);
        if (const auto e = r.resolve(name, o); e != std::errc{})
            return std::unexpected(e);
        const int fd = ::open(r.path_, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, o.mode);
        if (fd < 0)
            return std::unexpected(static_cast<std::errc>(errno));
        r.owner_ = true; // from here on, failure unlinks
        r.creator_ = ::getpid();
        if (const auto e = check_fs(fd, o.pages, r.page_); e != std::errc{})
            return fail(fd, std::move(r), e);
        const std::size_t n = detail::align_up(bytes, r.page_);
        if (::ftruncate(fd, static_cast<off_t>(n)) != 0)
            return fail(fd, std::move(r), static_cast<std::errc>(errno));
        // 2.1.12 (review I-05): on a size-limited tmpfs (Docker's 64 MiB /dev/shm) ftruncate and even MAP_POPULATE
        // succeed and the first touch of a page beyond the limit is a SIGBUS; allocating the blocks now turns that
        // into an error here. (hugetlbfs reserves its pages at mmap(), which reports ENOMEM itself.)
        if (r.page_ == system_page())
            if (const int e = ::posix_fallocate(fd, 0, static_cast<off_t>(n)); e != 0)
                return fail(fd, std::move(r), static_cast<std::errc>(e));
        return r.map(fd, n, o.lock);
    }

    // An existing named region, mapped at its full size. Without o.dir it looks in /dev/shm and then in every
    // hugetlbfs mount, so the opener need not know which page size the creator chose. errc::no_such_file_or_
    // directory when absent; errc::resource_unavailable_try_again when it exists but is not sized yet.
    static std::expected<region, std::errc> open(const std::string_view name,
                                                 const region_options& o = {}) noexcept
    {
        if (!o.dir.empty())
            return open_in(name, o.dir, o);
        auto r = open_in(name, "/dev/shm", o);
        if (r || r.error() != std::errc::no_such_file_or_directory)
            return r;
        std::errc last = std::errc::no_such_file_or_directory;
        for_each_hugetlbfs([&](const char* const mnt, std::size_t) {
            auto h = open_in(name, mnt, o);
            if (h || h.error() != std::errc::no_such_file_or_directory)
            {
                r = std::move(h);
                last = std::errc{};
                return false; // stop
            }
            return true;
        });
        return last == std::errc{} ? std::move(r) : std::unexpected(last);
    }

    // An unnamed MAP_SHARED | MAP_ANONYMOUS mapping (+ MAP_HUGETLB for huge pages): for threads, and inherited by
    // fork() children. errc::not_enough_memory when the huge-page pool cannot back it.
    static std::expected<region, std::errc> anonymous(const std::size_t bytes,
                                                      const region_options& o = {}) noexcept
    {
        region r;
        if (bytes == 0)
            return std::unexpected(std::errc::invalid_argument);
        int flags = MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE;
        r.page_ = system_page();
        if (o.pages != pages::normal)
        {
            r.page_ = page_bytes(o.pages);
            flags |= MAP_HUGETLB | ((o.pages == pages::huge_2m ? 21 : 30) << MAP_HUGE_SHIFT);
        }
        const std::size_t n = detail::align_up(bytes, r.page_);
        void* const p = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == MAP_FAILED)
            return std::unexpected(errno == EINVAL && o.pages != pages::normal
                                       ? std::errc::not_enough_memory
                                       : static_cast<std::errc>(errno));
        r.p_ = p;
        r.n_ = n;
        if (o.lock && ::mlock(p, n) != 0)
            return std::unexpected(static_cast<std::errc>(errno)); // r's destructor unmaps
        return r;
    }

    // Unlinks a named region (the same search as open()). True if a file was removed.
    static bool remove(const std::string_view name, const region_options& o = {}) noexcept
    {
        char path[max_path];
        const auto rm = [&](const std::string_view dir) {
            return make_path(path, dir, name) && ::unlink(path) == 0;
        };
        if (!o.dir.empty())
            return rm(o.dir);
        if (rm("/dev/shm"))
            return true;
        bool removed = false;
        for_each_hugetlbfs([&](const char* const mnt, std::size_t) {
            removed = rm(mnt);
            return !removed;
        });
        return removed;
    }

    region(region&& o) noexcept
        : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)), page_(o.page_),
          owner_(std::exchange(o.owner_, false)), creator_(o.creator_)
    {
        std::memcpy(path_, o.path_, sizeof path_);
    }
    region& operator=(region&& o) noexcept
    {
        if (this != &o)
        {
            close();
            p_ = std::exchange(o.p_, nullptr);
            n_ = std::exchange(o.n_, 0);
            page_ = o.page_;
            owner_ = std::exchange(o.owner_, false);
            creator_ = o.creator_;
            std::memcpy(path_, o.path_, sizeof path_);
        }
        return *this;
    }
    region(const region&) = delete;
    region& operator=(const region&) = delete;
    ~region()
    {
        close();
    }

    void* data() const noexcept
    {
        return p_;
    }
    std::size_t size() const noexcept
    {
        return n_;
    }
    std::size_t page_size() const noexcept
    {
        return page_;
    }
    bool huge() const noexcept
    {
        return page_ > system_page();
    }
    std::string_view path() const noexcept
    {
        return path_;
    } // empty for anonymous regions
    bool owner() const noexcept
    {
        return owner_;
    }
    void keep() noexcept
    {
        owner_ = false;
    } // the file outlives this mapping

    void close() noexcept
    {
        if (p_)
            ::munmap(p_, n_);
        // 2.1.12 (review I-06): only in the process that created the file — a fork()ed child that exits normally
        // must not unlink the parent's region
        if (owner_ && path_[0] && creator_ == ::getpid())
            ::unlink(path_);
        p_ = nullptr;
        n_ = 0;
        owner_ = false;
    }

    static std::size_t system_page() noexcept
    {
        return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    }

    // Calls f(mount_point, huge_page_bytes) for every hugetlbfs mount in /proc/mounts until f returns false.
    template<class F> static void for_each_hugetlbfs(F&& f) noexcept
    {
        std::FILE* const m = std::fopen("/proc/mounts", "re");
        if (!m)
            return;
        char line[1024];
        while (std::fgets(line, sizeof line, m))
        {
            char* save = nullptr;
            ::strtok_r(line, " ", &save); // device
            char* const mnt = ::strtok_r(nullptr, " ", &save);
            const char* const type = ::strtok_r(nullptr, " ", &save);
            if (!mnt || !type || std::strcmp(type, "hugetlbfs") != 0)
                continue;
            unescape(mnt);
            struct statfs s = {};
            if (::statfs(mnt, &s) != 0)
                continue;
            if (!f(static_cast<const char*>(mnt), static_cast<std::size_t>(s.f_bsize)))
                break;
        }
        std::fclose(m);
    }

private:
    region() noexcept = default;

    static std::expected<region, std::errc> fail(const int fd, region&& r,
                                                 const std::errc e) noexcept
    {
        ::close(fd);
        r.close(); // unlinks: r.owner_ is set
        return std::unexpected(e);
    }
    static std::expected<region, std::errc> open_in(const std::string_view name,
                                                    const std::string_view dir,
                                                    const region_options& o) noexcept
    {
        region r;
        if (!make_path(r.path_, dir, name))
            return std::unexpected(std::errc::invalid_argument);
        const int fd = ::open(r.path_, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return std::unexpected(static_cast<std::errc>(errno));
        struct statfs s = {};
        struct stat st = {};
        if (::fstatfs(fd, &s) != 0 || ::fstat(fd, &st) != 0)
            return fail(fd, std::move(r), static_cast<std::errc>(errno));
        if (s.f_type != tmpfs_magic && s.f_type != hugetlbfs_magic)
            return fail(fd, std::move(r), std::errc::invalid_argument);
        if (!o.foreign_owner &&
            (st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0))
            return fail(fd, std::move(r), std::errc::permission_denied); // 2.1.12 (review I-08)
        if (st.st_size <= 0)
            return fail(fd, std::move(r),
                        std::errc::resource_unavailable_try_again); // created, not sized yet
        r.page_ = s.f_type == hugetlbfs_magic ? static_cast<std::size_t>(s.f_bsize) : system_page();
        return r.map(fd, static_cast<std::size_t>(st.st_size), o.lock);
    }
    // The directory for `name` under options `o` (explicit dir, /dev/shm, or the hugetlbfs mount of that size).
    std::errc resolve(const std::string_view name, const region_options& o) noexcept
    {
        if (!o.dir.empty())
            return make_path(path_, o.dir, name) ? std::errc{} : std::errc::invalid_argument;
        if (o.pages == pages::normal)
            return make_path(path_, "/dev/shm", name) ? std::errc{} : std::errc::invalid_argument;
        // The first mount of that page size this process can write to; if none is writable, the first one (the
        // open() then reports permission_denied, which says what to fix: the mount's owner or mode).
        std::errc e = std::errc::no_such_file_or_directory;
        bool writable = false;
        for_each_hugetlbfs([&](const char* const mnt, const std::size_t pg) {
            if (pg != page_bytes(o.pages))
                return true;
            const bool w = ::access(mnt, W_OK) == 0;
            if (e == std::errc::no_such_file_or_directory || (w && !writable))
            {
                e = make_path(path_, mnt, name) ? std::errc{} : std::errc::invalid_argument;
                writable = w;
            }
            return !writable; // keep looking only while no writable mount was found
        });
        return e;
    }
    // tmpfs for normal pages; hugetlbfs of exactly the requested size for huge pages. Sets `page`.
    static std::errc check_fs(const int fd, const pages want, std::size_t& page) noexcept
    {
        struct statfs s = {};
        if (::fstatfs(fd, &s) != 0)
            return static_cast<std::errc>(errno);
        if (want == pages::normal)
        {
            page = system_page();
            return s.f_type == tmpfs_magic ? std::errc{} : std::errc::invalid_argument;
        }
        page = static_cast<std::size_t>(s.f_bsize);
        return s.f_type == hugetlbfs_magic && page == page_bytes(want)
                   ? std::errc{}
                   : std::errc::invalid_argument;
    }
    std::expected<region, std::errc> map(const int fd, const std::size_t n,
                                         const bool lock) noexcept
    {
        void* const p =
            ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        const int e = errno;
        ::close(fd);
        if (p ==
            MAP_FAILED) // hugetlbfs reserves the pages at mmap(): ENOMEM means the pool is too small
        {
            close();
            return std::unexpected(static_cast<std::errc>(e));
        }
        p_ = p;
        n_ = n;
        if (lock && ::mlock(p, n) != 0)
        {
            const int le = errno;
            close();
            return std::unexpected(static_cast<std::errc>(le));
        }
        return std::move(*this);
    }
    // dir + '/' + name. A leading '/' on name is accepted; any other '/', ".", ".." or an empty name is not.
    static bool make_path(char (&out)[max_path], std::string_view dir,
                          std::string_view name) noexcept
    {
        if (!name.empty() && name.front() == '/')
            name.remove_prefix(1);
        while (dir.size() > 1 && dir.back() == '/')
            dir.remove_suffix(1);
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string_view::npos || dir.empty() ||
            dir.size() + 1 + name.size() >= max_path)
            return false;
        std::memcpy(out, dir.data(), dir.size());
        out[dir.size()] = '/';
        std::memcpy(out + dir.size() + 1, name.data(), name.size());
        out[dir.size() + 1 + name.size()] = '\0';
        return true;
    }
    // /proc/mounts escapes space, tab, newline and backslash as \ooo
    static void unescape(char* s) noexcept
    {
        char* d = s;
        for (; *s; ++d)
            if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
                s[3] >= '0' && s[3] <= '7')
            {
                *d = static_cast<char>((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
                s += 4;
            }
            else
                *d = *s++;
        *d = '\0';
    }

    void* p_ = nullptr;
    std::size_t n_ = 0;
    std::size_t page_ = 0;
    bool owner_ = false;
    pid_t creator_ = 0; // the process that created the file (2.1.12)
    char path_[max_path] = {};
};

} // namespace es::ipc
