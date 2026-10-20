# `src/file_io.cpp`

The POSIX file helpers of the tools layer (spec §6.9, FR-D5): `tools::read_file()`, `tools::write_file()`,
`tools::write_all()`, and the library's single write loop `internal::write_all()` declared in `src/internal.hpp`.
They live apart from `src/elf_reader.cpp` (review addendum A4): they have nothing to do with the executable
format. Every input is capped at `decode_limits::input_bytes` (1 GiB by default, gk 1.15; a process-wide limit,
with `internal::max_input_bytes` = 1 GiB as its ceiling). It has no reflection
dependency.

## Depends on / used by

Includes: `internal.hpp` (`max_input_bytes`, `input_limit()`, the `internal::write_all` declaration), `es/serreg/tools.hpp`,
`<cerrno>`, `<fcntl.h>`, `<sys/stat.h>`, `<unistd.h>`.

Used by: `src/elf_reader.cpp` (`extract_section_from_elf`, `extract_embedded_dictionary(path)` → `read_file`),
`src/dictionary.cpp` (`load_dictionary_file` → `read_file`), `src/cli.cpp` (captures, `--config`, `--diff`
operands through `read_file`; `--rri-out` through `write_file`), `src/registry.cpp` (`tools::write_all` for
`export_image(fd)` and the `ES_SERREG_DUMP_*` output), and every test that reads a file. (`src/reader.cpp`'s
windowed reader and `mapped_file` do their own I/O; every capture path of the tools (A6) goes
through the windowed reader, so `read_file` is left with RRI, ELF and config inputs, `--diff` operands, the
span-based readers' callers, and a dump of a pipe.)

## Structure

- **`slurp(fd)`** (`file_io.cpp:20-56`, anonymous namespace): read a descriptor to EOF in 1 MiB steps,
  `EINTR`-safe, with the input cap and an exact-fit probe.
- **`tools::read_file(path)`** (`file_io.cpp:60-71`): `"-"` → `slurp(STDIN_FILENO)`; otherwise
  `open(O_RDONLY | O_CLOEXEC)`, `ENOENT` → `not_found`, other open failures → `io_error`.
- **`tools::write_all(fd, span)`** (`file_io.cpp:73-76`): forwards to `internal::write_all`.
- **`tools::write_file(path, span)`** (`file_io.cpp:78-91`): `O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC`, mode 0644;
  a failing `close()` is `io_error`.
- **`internal::write_all(fd, data, size)`** (`file_io.cpp:98-117`): the write loop.

## How it works

### `slurp` and the input cap

```cpp
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
```

(`file_io.cpp:42-52`.) `read_file` works for regular files, pipes and `"-"` (stdin) alike by reading to EOF
rather than `fstat`ing a size. The buffer grows by 1 MiB whenever less than 64 KiB is free, never beyond
`internal::input_limit()` (gk 1.15: a hostile pipe must not exhaust memory) — that is
`get_decode_limits().input_bytes`, capped at `max_input_bytes` (1 GiB), read on each step so `set_decode_limits`
takes effect for the next read; an input that exactly fills the cap is probed
for one more byte to distinguish "exactly at the cap" (accepted) from "more" (`unsupported`). The vector is shrunk to
the bytes read before it is returned.

### The one write loop

```cpp
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
```

(`file_io.cpp:98-117`.) gk 1.9 / cc F20: the tools layer's one write loop. `EINTR` is retried, a zero-length
write is an error (no busy loop), `EAGAIN` is not retried — blocking descriptors only. `tools::write_all(fd, span)`
forwards to it; `write_file` opens, writes, closes, and closes on the error path too. (The header-only `fd_sink`
in `sink.hpp` carries its own equivalent, `detail::write_fd`, because headers cannot depend on `libserreg`'s
`.cpp` files.)

## Maintaining it

- **`write_all` is the tools layer's only write loop.** New output paths in `src/` must call it, not `::write`
  directly. Keep `internal.hpp`'s declaration and `tools::write_all` in sync, both `noexcept`.
- **The input cap and the `"-"` convention are CLI contracts** (`--stdin`, `use_stdin`, `tools.hpp`'s
  `read_file` comment); the windowed reader and `mapped_file` (`reader.cpp`) are the paths for captures of any
  size. `input_limit()` can only be lowered below 1 GiB by `set_decode_limits`, never raised above it.
- **No format knowledge here.** Parsing belongs to `elf_reader.cpp`, `dictionary.cpp` and `reader.cpp`.
- **Tests:** every test that reads or writes a file: `test_capture.cpp` (FR-D6 over `/proc/self/exe`, the
  `ES_SERREG_DUMP_REGISTRY_FD` pipe), `runtime_selftest` (reads `/proc/self/exe`), the `srdump_*` and `golden_*`
  ctests (captures, `.rri` files, `--rri-out`), `premain_env_dump`.
- **Allocation:** `slurp` allocates up to `input_limit()` (1 GiB by default). `internal::write_all` is `noexcept` and allocation-free because
  `export_image(fd)` and the exit-time dump use it.
- **Threading:** all functions are reentrant; no shared state.
