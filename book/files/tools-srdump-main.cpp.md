# `tools/srdump/main.cpp`

The `srdump` executable. Its whole body is one statement — `return es::serreg::cli::run(argc, argv);` —
because everything the tool does is a library service: argument parsing, `--config`, `--verify`, `--json`,
`--elf`, `--diff`, `--version` live in `es::serreg::cli` (`src/cli.cpp`) and the work behind them in
`es::serreg::tools` (spec §6.9, FR-D0: "everything the CLI can do is callable from the library"). The file
defines no options, prints nothing, and holds no logic of its own.

## Depends on / used by

Includes: `es/serreg/tools.hpp` (for `cli::run`). Links: `es::serreg_tools` (`libserreg_tools`, and through it `libserreg`;
decision G).

Used by: every `srdump_*`, `golden_srdump_*` and
`srdump_elf_*` ctest, `scripts/run-all-tests.sh` (`srdump --verify` on `tests/fixtures/golden/orders.ser`),
and users at the shell.

## Structure

```cpp
#include <es/serreg/tools.hpp>
int main(int argc, char** argv)
{
    return es::serreg::cli::run(argc, argv);
}
```

(`main.cpp:1-5`.) One include, one function, one return.

## How it works

`cli::run` constructs a default `cli::app` (`name = "srdump"`, `version = tools::library_version` = `"2.0.0"`,
no `extra_usage`, no `on_extra`), calls `cli::parse` (the `--config` pass, then the flags) and
`cli::execute`, and returns its exit code — 0 ok, 1 verify problems or a tool/decode error, 2 usage. Because
`main` adds nothing, an application that links `es::serreg_tools` and calls `cli::run(argc, argv, {.name = "myapp",
…})` gets exactly `srdump`'s behaviour under its own name (`examples/embedded_cli.cpp`), and a test that
calls `cli::run` in-process (`test_tools.cpp`, "cc F24 … cli::run knows --version") tests the same code the
binary runs.

## Maintaining it

- **Keep it at one line** (CLAUDE.md, AGENTS.md: "`tools/srdump/ main.cpp (one line)`"). A new flag or
  behaviour goes into `src/cli.cpp` (`parse_args`, `apply`, `usage_options`, `execute`) or `src/tools*`,
  never here — otherwise the library and the binary diverge and the in-process tests stop covering the tool.
- **The version string is not here**: `srdump --version` prints `tools::library_version` from `tools.hpp`
  and `fmt::format_major`/`format_minor` from `format.hpp`; `srdump_version` checks it against
  `PROJECT_VERSION` in CMake.
- **Wire format / allocation / threading:** nothing to maintain; see `src/cli.cpp`.
