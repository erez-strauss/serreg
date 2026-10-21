#!/usr/bin/env python3
"""scripts/check-book.py — keeps book/ in sync with the sources (AGENTS.md §5b).

Checks:
  1. pages   every library/tool source file (include/es/**, src/*, tools/srdump/*) has a book/files/*.md page,
             and every page names a file that exists (build-system.md, examples.md, tests.md are topic pages);
  2. range   every `name.ext:N` / `name.ext:N-M` citation in book/ points inside that file;
  3. code    a citation written right after a code block — "```\n(`file.cpp:N`)" — points at the line where the
             block's first code line actually is (±3 lines), so a quoted snippet and its line number agree;
  4. stale   no book page mentions a symbol listed in STALE (names removed from the code);
  5. tests   (2.1.3) a quoted test name — "FR-…", "G2-…", "gk …", "cc …", "2.x …", "format …" — is (part of) a
             TEST_CASE name in tests/; "…" in the quote stands for omitted words.

usage: scripts/check-book.py [--fix] [--quiet]
  --fix     rewrite drifted citations of kind 3 when the block's first line occurs exactly once in the file
            (a range N-M keeps its length); anything else is reported for a human.
Exit status: 0 when clean, 1 when a problem remains. Run by scripts/run-all-tests.sh (stage "book sync").
Standard library only.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOPIC_PAGES = {"build-system.md", "examples.md", "tests.md"}
SOURCE_DIRS = ["include/es/serreg", "include/es/ipc", "include/es/ipc_old", "src", "tools/srdump"]
SEARCH_DIRS = ["include", "src", "tools", "examples", "tests"]
# Names that were removed from the code; the book must not describe them as current.
STALE = ["fnv1a64", "hash_algo_fnv1a64", "golden-1.0", "golden-1.1", "compat10_", "compat11_",
         "format_canary_rri_1_", "dictionary::format_minor", "format_minor{fmt::format_minor}",
         "block_fits", "orders-1.2.ser", "dynamic-1.2.ser", "orders-1.4.ser", "dynamic-1.4.ser",
         "orders-1.4.rri", "orders-1.5.ser", "dynamic-1.5.ser", "orders-1.5.rri"]
TEST_QUOTE = re.compile(r'"((?:FR-|NFR-|G2-|gk |cc |2\.\d|v\d|format )[^"]{6,})"')
REF = re.compile(r"([A-Za-z_][A-Za-z0-9_.-]*\.(?:hpp|cpp|h|cmake|sh|py)):(\d+)(?:[-–](\d+))?")
# a back-quoted identifier in prose: `name`, `ns::name`, `name()`, `--flag`, `name<T>` (the template part is dropped)
PROSE_TOKEN = re.compile(r"`([A-Za-z_:~\-][A-Za-z0-9_:\-]*(?:\(\))?)(?:<[^`]*>)?`")


def source_files():
    out = {}
    for d in SEARCH_DIRS:
        for dp, _, fs in os.walk(os.path.join(REPO, d)):
            if "/fixtures" in dp:
                continue
            for f in fs:
                out.setdefault(f, []).append(os.path.join(dp, f))
    return out


def test_names():
    names = []
    for dp, _, fs in os.walk(os.path.join(REPO, "tests")):
        for f in fs:
            if not f.endswith(".cpp"):
                continue
            with open(os.path.join(dp, f), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in re.finditer(r'TEST_CASE\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', text):
                names.append("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))))
    return names


def page_name(rel):
    return rel.replace("/", "-") + ".md"


def main():
    fix = "--fix" in sys.argv
    quiet = "--quiet" in sys.argv
    problems = []
    fixed = 0
    files = source_files()
    tests = test_names()
    cache = {}

    def lines_of(path):
        if path not in cache:
            with open(path, encoding="utf-8", errors="replace") as fh:
                cache[path] = fh.read().split("\n")
        return cache[path]

    # 1. pages
    pages_dir = os.path.join(REPO, "book", "files")
    pages = set(os.listdir(pages_dir))
    wanted = set()
    for d in SOURCE_DIRS:
        for dp, _, fs in os.walk(os.path.join(REPO, d)):
            for f in fs:
                if f.endswith((".hpp", ".cpp")):
                    rel = os.path.relpath(os.path.join(dp, f), REPO)
                    wanted.add(page_name(rel))
    for p in sorted(wanted - pages):
        problems.append(f"pages: no book page book/files/{p}")
    for p in sorted(pages - wanted - TOPIC_PAGES):
        problems.append(f"pages: book/files/{p} documents a file that does not exist")

    # 2.-4. citations and stale names
    for dp, _, fs in os.walk(os.path.join(REPO, "book")):
        for f in sorted(fs):
            if not f.endswith(".md"):
                continue
            path = os.path.join(dp, f)
            rel_page = os.path.relpath(path, REPO)
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
            lines = text.split("\n")
            changed = False
            for name in STALE:
                if name in text:
                    problems.append(f"stale: {rel_page} mentions removed name '{name}'")
            for i, line in enumerate(lines):
                for m in TEST_QUOTE.finditer(line):
                    parts = [p.strip(" .") for p in m.group(1).split("…") if p.strip(" .")]
                    if not any(all(p in n for p in parts) for n in tests):
                        problems.append(f"tests: {rel_page}:{i + 1}: no TEST_CASE matches \"{m.group(1)}\"")
                for m in list(REF.finditer(line)):
                    cands = files.get(os.path.basename(m.group(1)))
                    if not cands or len(cands) > 1:
                        continue  # unknown or ambiguous basename (e.g. CMakeLists): not checkable
                    src = cands[0]
                    src_lines = lines_of(src)
                    a = int(m.group(2))
                    b = int(m.group(3)) if m.group(3) else a
                    out_of_range = a < 1 or b > len(src_lines) or b < a
                    # a citation right after a closing fence refers to the block above it
                    j = i - 1
                    while j >= 0 and lines[j].strip() == "":
                        j -= 1
                    if j < 0 or not lines[j].startswith("```") or m.start() > 3:
                        if out_of_range:
                            problems.append(f"range: {rel_page}:{i + 1}: {m.group(0)} (file has {len(src_lines)} lines)")
                            continue
                        # 2.1.16 (question M): a prose citation — "`name` (file.hpp:N)" — is checked by the nearest
                        # back-quoted identifier before it on the same line: it must occur within the cited lines
                        # (±3 for a single line). Generic tokens (shorter than 4 characters, no letter) are skipped.
                        before = line[:m.start()]
                        toks = PROSE_TOKEN.findall(before[-120:])
                        if toks:
                            tok = toks[-1].split("(")[0].strip()
                            tok = tok.rsplit("::", 1)[-1]  # a qualified name is cited by its last component
                            if len(tok) >= 4 and re.search(r"[A-Za-z]", tok) and not tok.startswith("-"):
                                window = src_lines[max(0, a - 4):(b if m.group(3) else a) + 3]
                                if not any(tok in s for s in window):
                                    hits = [n + 1 for n, s in enumerate(src_lines) if tok in s]
                                    if fix and len(hits) == 1 and not m.group(3):
                                        new = f"{m.group(1)}:{hits[0]}"
                                        lines[i] = lines[i][:m.start()] + new + lines[i][m.end():]
                                        changed = True
                                        fixed += 1
                                        continue
                                    where = f"; found at {hits[:6]}" if hits else "; not found in the file"
                                    problems.append(f"prose: {rel_page}:{i + 1}: {m.group(0)} does not show `{tok}`{where}")
                        continue
                    k = j - 1
                    while k >= 0 and not lines[k].startswith("```"):
                        k -= 1
                    body = [x.strip() for x in lines[k + 1:j] if x.strip()]
                    if not body:
                        continue
                    # the book may condense a declaration onto one line: match the part before '{', at most 40 chars
                    first = body[0].split("{")[0].strip()[:40] or body[0][:40]
                    if first.startswith(("//", "...", "…", "#")) or len(first) < 8:
                        if out_of_range:
                            problems.append(f"range: {rel_page}:{i + 1}: {m.group(0)} (file has {len(src_lines)} lines)")
                        continue
                    # a single line must be within ±3; a range N-M may quote an excerpt from anywhere inside it
                    window = [] if out_of_range else src_lines[max(0, a - 4):(b if m.group(3) else a) + 3]
                    if any(first in s for s in window):
                        continue
                    hits = [n + 1 for n, s in enumerate(src_lines) if first in s]
                    fname = m.group(1)
                    if not hits:  # the code moved to another file: look everywhere
                        found = [(os.path.basename(pth), n + 1) for base, pths in files.items()
                                 for pth in pths if len(pths) == 1 and pth.endswith((".hpp", ".cpp"))
                                 for n, s in enumerate(lines_of(pth)) if first in s]
                        if len(found) == 1:
                            fname, hit = found[0]
                            hits = [hit]
                    if fix and len(hits) == 1:
                        na = hits[0]
                        nb = na + (b - a)
                        new = f"{fname}:{na}" + (f"-{nb}" if m.group(3) else "")
                        lines[i] = lines[i][:m.start()] + new + lines[i][m.end():]
                        changed = True
                        fixed += 1
                        continue
                    where = f"; found at {hits}" if hits else "; not found in the file"
                    problems.append(f"code: {rel_page}:{i + 1}: {m.group(0)} first line '{first[:60]}'{where}")
            if changed:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(lines))
    for p in problems:
        print(p)
    if not quiet or problems:
        print(f"book sync: {len(problems)} problem(s)" + (f", {fixed} citation(s) fixed" if fix else ""))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
