"""python3 -m serreg — srdump's main modes in pure Python.

    python3 -m serreg <file.ser> [--json | --text] [--dict x.rri ...]   dump: the dictionary, then the frames
    python3 -m serreg <file.ser> --verify [--json]                        counts and problems; exit 1 on problems
    python3 -m serreg <file.ser> --info [--json]                          1.7: header, dictionary chain, checkpoints
    python3 -m serreg <file.ser> --level LVL [--json]                     log frames at or above LVL, one line each
    python3 -m serreg --rri <image.rri> [--json]                          a dictionary on its own

Exit codes: 0 ok, 1 problems (verify) or a capture that cannot be read, 2 bad usage.
"""
from __future__ import annotations

import argparse
import json
import mmap
import sys

from . import format as F
from .dictionary import Dictionary, load_rri
from .reader import LEVEL_BY_NAME, Reader, capture_info, json_dumps, json_lines, text_lines


def _open(path: str) -> bytes | mmap.mmap:
    if path == "-":
        return sys.stdin.buffer.read()
    with open(path, "rb") as fh:
        try:
            return mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        except (ValueError, OSError):  # an empty file, or no mmap: read it whole
            fh.seek(0)
            return fh.read()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m serreg", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?", help="a .ser FILE or a bare frame stream ('-' for standard input)")
    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument("--json", action="store_true", help="JSON output (a dictionary object, then one object per line)")
    fmt.add_argument("--text", action="store_true", help="text output (the default)")
    ap.add_argument("--verify", action="store_true", help="check frames and records; exit 1 on problems")
    ap.add_argument("--info", action="store_true", help="1.7: header, dictionary chain, checkpoints, writer info")
    ap.add_argument("--level", metavar="LVL", choices=list(F.LEVEL_NAMES),
                    help="only log frames at or above trace|debug|info|warn|error|fatal")
    ap.add_argument("--dict", metavar="X.RRI", action="append", default=[], help="extra dictionary (repeatable)")
    ap.add_argument("--rri", metavar="IMAGE.RRI", help="print a dictionary image and exit")
    ap.add_argument("--dict-only", action="store_true", help="print only the capture's dictionary")
    ap.add_argument("--hex", action="store_true", help="with --json: add each record's payload bytes")
    a = ap.parse_args(argv)
    out = sys.stdout
    json_mode = a.json

    extra: list[Dictionary] = []
    for path in a.dict + ([a.rri] if a.rri else []):
        d, err = load_rri(path)
        if err:
            print(f"{path}: {err}", file=sys.stderr)
            return 1
        extra.append(d)
    if a.rri:
        merged = Dictionary()
        for d in extra:
            merged.merge(d)
        out.write(json.dumps(merged.to_json_obj(), indent=2) + "\n" if json_mode else merged.to_text())
        return 0
    if not a.capture:
        ap.error("a capture is required")
    try:
        data = _open(a.capture)
    except OSError as e:
        print(f"{a.capture}: {e.strerror or e}", file=sys.stderr)
        return 1

    if a.info:
        ci = capture_info(data)
        if isinstance(ci, str):
            print(f"{a.capture}: {ci}", file=sys.stderr)
            return 1
        out.write(json_dumps(ci.to_json_obj()) + "\n" if json_mode else ci.text())
        return 0

    r = Reader(data, extra)
    if r.error:
        print(f"{a.capture}: {r.error}", file=sys.stderr)
        return 1
    if a.verify:
        rep = r.verify()
        out.write(json_dumps(rep.to_json_obj()) + "\n" if json_mode else rep.text())
        return 0 if rep.ok else 1

    min_level = LEVEL_BY_NAME[a.level] if a.level else None
    if min_level is None:  # the dictionary first, as of the end of the capture (dictionary frames may be anywhere)
        pre = Reader(data, extra, decode=False)
        for _ in pre.frames():
            pass
        out.write(json.dumps(pre.dictionary.to_json_obj(), indent=2) + "\n" if json_mode else
                  pre.dictionary.to_text() + "\n")
        if a.dict_only:
            return 0
    if json_mode:
        for o in json_lines(r, min_level, a.hex):
            out.write(json_dumps(o) + "\n")
    else:
        for line in text_lines(r, min_level):
            out.write(line + "\n")
    return 1 if r.problems or r.problems_omitted else 0


if __name__ == "__main__":
    sys.exit(main())
