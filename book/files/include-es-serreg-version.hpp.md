# `include/es/serreg/version.hpp`

The library version as a string: `es::serreg::library_version`, the release this tree builds. It is a header of
its own (not a constant in `tools.hpp`) so that the runtime library can stamp it into the writer-info frame of a format 1.7 capture without depending on the tools layer.
No reflection dependency; it includes only `<string_view>`.

## Depends on / used by

Includes: `<string_view>`.

Included by: `serreg.hpp`, `capture.hpp` (the `library_version` item of the writer-info frame), `tools.hpp`
(`using es::serreg::library_version;` — `tools::library_version` stays valid for existing callers, and
`cli::app::version` defaults to it, which `srdump --version` prints).

## What it defines

```cpp
namespace es::serreg
{
inline constexpr std::string_view library_version = "2.2.6";
} // namespace es::serreg
```
(`version.hpp:6-9`.)

| name | kind | meaning |
|---|---|---|
| `library_version` | `inline constexpr std::string_view` | `"major.minor.patch"` of the release |

## Maintaining it

- **Changed only by `scripts/bump-version.sh X.Y.Z`** (the header comment says so), together with
  `CMakeLists.txt`'s `project(VERSION …)`, the SONAME and the package version; never by a blanket `sed`, which
  would also rewrite historical "since X" references across the tree.
- The string is part of every 1.7 capture (writer-info key `library_version`), so `srdump --info` shows which
  release wrote a file. The golden captures `tests/fixtures/golden/orders.ser`, `dynamic.ser` and `features.ser` carry
  a writer-info frame and therefore this string: after a version bump the `golden_fixture_*_byte_exact` tests fail
  until the fixtures are regenerated with `make_fixtures` and `make_dynamic_fixtures` (and their
  `.text`/`.json`/`.toml` expectations checked) — `bump-version.sh` does not do it.
- The wire format's version is not this string: it is `fmt::format_major`/`format_minor` in `format.hpp`.
