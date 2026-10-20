# `include/es/ipc/region.hpp`

Memory for the rings — es::ipc's variable-size rings and also the old slot
rings of `es::ipc_old`, which include it as `<es/ipc/region.hpp>`: a named file on tmpfs or hugetlbfs mapped `MAP_SHARED`, for unrelated
processes, or an anonymous shared mapping, for threads and `fork` children. Normal pages or explicit 2 MiB/1 GiB
huge pages, optional `mlock`. Chapter 11 §11.1 explains the design (why not `shm_open`, the tmpfs/hugetlbfs
check, no THP).

Includes: `<cerrno>`, `<cstddef>`, `<cstdint>`, `<cstdio>`, `<cstring>`, `<expected>`, `<string_view>`,
`<system_error>`, `<utility>`, `<fcntl.h>`, `<sys/mman.h>`, `<sys/stat.h>`, `<sys/vfs.h>`, `<unistd.h>`,
`common.hpp`. Defines `MAP_HUGE_SHIFT` (26) if the C library does not.

| name | meaning |
|---|---|
| `pages` | `normal`, `huge_2m`, `huge_1g`; `page_bytes(p)` gives 0 / 2 MiB / 1 GiB and `to_string(p)` its name |
| `region_options` | `pages` (normal); `lock` (false); `dir` (empty: `/dev/shm` for normal pages, the hugetlbfs mount of that page size for huge pages); `mode` (0600) |
| `region::create(name, bytes, o)` | `open(O_CREAT\|O_EXCL\|O_RDWR\|O_CLOEXEC\|O_NOFOLLOW)` → `fstatfs` check (tmpfs for normal pages; hugetlbfs with `f_bsize` equal to the requested page size) → `ftruncate(align_up(bytes, page))` → for normal pages `posix_fallocate` of the whole size → `mmap(MAP_SHARED\|MAP_POPULATE)` → optional `mlock`. Any failure after the file exists unlinks it |
| `region::open(name, o)` | with `o.dir`: that directory. Otherwise `/dev/shm`, then every hugetlbfs mount. The file must be on tmpfs or hugetlbfs; it must also be owned (I-08) by this user and not writable by group or others, else `permission_denied`, unless `o.foreign_owner` (`/dev/shm` is world-writable: anyone can plant a file under the expected name); size 0 gives `resource_unavailable_try_again` (created but not sized yet); `page_size()` comes from the filesystem |
| `region::anonymous(bytes, o)` | `mmap(MAP_SHARED\|MAP_ANONYMOUS\|MAP_POPULATE [\|MAP_HUGETLB\|MAP_HUGE_2MB/1GB])`; an empty pool gives `not_enough_memory` |
| `region::remove(name, o)` | unlink with the same search as `open` |
| `data()`, `size()`, `page_size()`, `huge()`, `path()`, `owner()`, `keep()`, `close()` | mapping, rounded size, actual page size, page size > system page, file path (empty if anonymous), whether this mapping owns the name, give that up, unmap (+ unlink if owner and in the creating process) |
| `system_page()` | `sysconf(_SC_PAGESIZE)` |
| `for_each_hugetlbfs(f)` | calls `f(mount_point, huge_page_bytes)` for each hugetlbfs line of `/proc/mounts` (octal escapes decoded, page size from `statfs`) until `f` returns false |
| `tmpfs_magic`, `hugetlbfs_magic` | `TMPFS_MAGIC`, `HUGETLBFS_MAGIC` |

Mount choice: with several hugetlbfs mounts of the requested page size, `create` uses the first one
the process can write to (`access(W_OK)`). If none is writable, it uses the first, and `open(O_CREAT)` reports
`permission_denied`. A hugetlbfs mount is `root:root 0755` unless it was mounted with `uid=`/`gid=`/`mode=`.

Two review fixes:

- **I-05, SIGBUS on a size-limited tmpfs.** On a `/dev/shm` with a size limit (Docker's default is 64 MiB),
  `ftruncate` and even `MAP_POPULATE` succeed for a region larger than the space left, and the first touch of a page
  beyond the limit is a `SIGBUS`. `create` now calls `posix_fallocate(fd, 0, n)` after `ftruncate` when the page
  size is the system page, so the blocks are allocated at once and a shortage is an error of `create` (typically
  `no_space_left_on_device`), after which the file is unlinked as for any other failure. hugetlbfs reserves its
  pages at `mmap`, which reports `ENOMEM` itself, so it needs no such call.
- **I-06, unlink across `fork`.** `create` records the creating process in `creator_` (`getpid()`), and `close()`
  (also run by the destructor) unlinks the name only when `owner_` is set and `creator_ == getpid()`. A `fork`ed
  child inherits the region object with `owner_` set; without the check, a child that exited normally would unlink
  the parent's region, and later `open` calls would fail with `no_such_file_or_directory`. `creator_` moves with the
  object (test "ipc region: only the creating process unlinks a named region").

The region tests (creation on tmpfs only, `mlock`, 2 MiB huge pages, I-06, I-08) are the "ipc region: …" cases of
`tests/unit/test_ipc.cpp` (`serreg_ipc_tests`); the ring tests use regions through the rings (named and
anonymous, across `fork`).

Errors (`std::errc`): `permission_denied` when the directory is not writable by this user; `invalid_argument`
for a bad name (inner `/`, `.`, `..`, empty, too long), size 0, or a
directory that is not tmpfs/hugetlbfs of the requested size. `file_exists` if the name is taken.
`no_such_file_or_directory` if no hugetlbfs mount has the requested page size, or `open` finds nothing.
`not_enough_memory` if the huge-page pool is too small. The `mlock` errors (`operation_not_permitted`,
`resource_unavailable_try_again`, `not_enough_memory`) are returned as they come. Nothing on this page is
hot-path code: it runs at setup.
