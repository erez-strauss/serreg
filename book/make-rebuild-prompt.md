# `scripts/make-rebuild-prompt.py` — the single-file rebuild prompt

*(DECISIONS D-2.)* Assembles `ser-reg-rebuild-v<version>.md`: one self-contained Markdown file from
which an agent or a person can build the library again from an empty directory and check the result bit for bit
against the original — without the original's source. The hand-written part, the instructions, is
`docs/ser-reg-rebuild-prompt.md`; the script appends the content those instructions refer to. Together they are
the project's one rebuild prompt. `scripts/make-release.sh` runs the script after packing the project tarball and writes the
generated file next to it. Python 3, standard library only. (This page is kept at the top of `book/` because
`files/` pages map to library sources — see `scripts/check-book.py`; `files/build-system.md` lists the script with
the other scripts.)

## Usage

```
scripts/make-rebuild-prompt.py [--out DIR] [--quiet]
```
`DIR` defaults to the parent of the repository (where `make-release.sh` puts the tarballs). The script prints the
output path, its line count and its SHA-256. It exits non-zero (with a message) when a part is missing or when
`CMakeLists.txt`'s version and the version `docs/DECISIONS.md` refers to (`**Refers to:** library **X.Y.Z**`)
disagree — a prompt never mixes two releases.

## What the generated file contains

```python
DOCS = [
    "docs/DECISIONS.md",
    "docs/ser-reg-requirements.md",
    "docs/ser-reg-architecture.md",
    "docs/ser-reg-testing.md",
    "docs/ser-reg-stories.md",
]
```
(`make-rebuild-prompt.py:20-26`.) In order:

1. **The prompt** — `docs/ser-reg-rebuild-prompt.md` with `<version>` and `<format>` substituted (the format is
   read from `format_minor` in `format.hpp`, e.g. `1.7`).
2. **The five flattened documents** above, each as one part.
3. **The normative sources**, each in a fence of its language: `include/es/serreg/format.hpp` (the byte
   layouts), `tests/golden/corpus.hpp`, `tests/golden/hashes.cpp`, `tests/golden/blobs.cpp` (the golden values the
   rebuild must reproduce), `python/serreg/format.py`, `CMakePresets.json`, `cmake/warnings.cmake`,
   `public-files-list.txt` (`make-rebuild-prompt.py:27-36`).
4. **The leading comment of every public header** in `include/es/serreg/`, `include/es/serreg/detail/` and
   `include/es/ipc/` — the API in the headers' own words (`leading_comment`, `make-rebuild-prompt.py:63-73`).
5. **Checksums** — SHA-256 and size of every file under `tests/fixtures/` and of the five
   `ce-examples/ce-exampleN/main.cpp`: the rebuilt tree must reproduce these files exactly.

`fence()` (`make-rebuild-prompt.py:86-90`) picks a fence longer than any run of backticks in the embedded text,
so a document that itself contains fenced code cannot end its part early.

## Maintaining it

- A document or source that the rebuild needs goes into `DOCS` or `SOURCES`, and the prompt
  (`docs/ser-reg-rebuild-prompt.md`) says why; nothing else in the tree is assumed to exist.
- The generated file's header line says "parts 2–12" while the current lists produce parts 2–9 (five documents,
  sources, header comments, checksums); the sentence is static text in `main()`.
- The checksum list changes with every fixture refreeze and every ce-example edit; that is intended — it is how a
  rebuild proves it matches.
- The output name carries the version (`ser-reg-rebuild-v<version>.md`); running the script twice for the same
  version rewrites that file (unlike `make-release.sh`, it does not refuse an existing output).
