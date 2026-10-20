#!/usr/bin/env python3
"""check-includes.py — the include rules of AGENTS.md §4 (2.1.13):

  1. every header (.hpp/.h) starts its code with `#pragma once`;
  2. package headers come before system headers: in every source and header, each `#include <es/...>` or
     `#include "..."` precedes every other `#include <...>` (the standard library, POSIX, Catch2, ...);
  3. every header includes what it needs to compile on its own — checked by the build: tests/headers compiles one
     generated translation unit per header, containing only that header (twice), in the object libraries
     serreg_headers_{ipc,public,internal,tests}.

Usage: scripts/check-includes.py            exit 0 when the tree follows rules 1 and 2, 1 otherwise (ctest
                                            headers_include_rules runs it).
Conditional includes (inside #if) are checked like the others; a file may opt out of rule 2 for one line with a
trailing `// include-order: after <reason>` comment (used where a system header must be seen first).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIRS = ["include", "src", "tools", "tests", "examples"]
INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"](.*)$')


def is_package(kind, name):
    return kind == '"' or name.startswith("es/")


def check(path):
    problems = []
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if path.suffix in (".hpp", ".h"):
        code = [l for l in text if l.strip() and not l.strip().startswith("//")]
        if not code or code[0].strip() != "#pragma once":
            problems.append("first code line is not #pragma once")
    seen_system = None
    for no, line in enumerate(text, 1):
        m = INC.match(line)
        if not m:
            continue
        kind, name, rest = m.groups()
        if is_package(kind, name):
            if seen_system and "include-order: after" not in rest:
                problems.append(f"line {no}: package header {name} after system header {seen_system}")
        elif seen_system is None:
            seen_system = name
    return problems


def main():
    bad = 0
    for d in DIRS:
        for p in sorted((ROOT / d).rglob("*")):
            if p.suffix not in (".hpp", ".h", ".cpp") or "build" in p.parts:
                continue
            for problem in check(p):
                bad += 1
                print(f"{p.relative_to(ROOT)}: {problem}")
    print(f"include rules: {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
