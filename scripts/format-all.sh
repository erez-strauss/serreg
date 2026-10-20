#!/bin/sh
# Formats every tracked C++ file in place with the repository .clang-format.
# The reference formatter is clang-format 22 (2.1.9: the maintainer's box and CI); older majors format a few
# constructs differently, so the script warns when it runs one (`pip install clang-format==22.1.8` provides it).
CF=${CLANG_FORMAT:-$(command -v clang-format || command -v clang-format-22)}
[ -n "$CF" ] || { echo "clang-format not found" >&2; exit 1; }
major=$("$CF" --version | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p')
if [ -n "$major" ] && [ "$major" -lt 22 ]; then
  echo "warning: $CF is clang-format $major; the reference is 22 and results may differ" >&2
fi
cd "$(dirname "$0")/.." && find include src tools tests examples -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 "$CF" -i --style=file
