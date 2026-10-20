#!/usr/bin/env python3
"""tests/python/parity.py — the pure-Python reader (python/serreg) decodes what srdump decodes (2.2.1).

For every capture given, `srdump --json` and the Python reader are run and compared semantically: the dictionary
object (types, fields, enumerators, literals, calls and their constants), then frame by frame and record by
record every {"frame","record","type","value"} line, every rendered print message {"frame","message"} and every
log frame {"frame","log":{level, time, file, line, thread, sequence, message}}, with floating-point values
compared numerically. Then `--verify`'s frame/record/byte counts and verdict (srdump's JSON form against the
Python API, and the Python command line's `ok:` line against srdump's), for 1.7 FILEs `--info`'s dictionary
blocks, checkpoints and dictionary counts, and for captures with log frames `--level trace`.

A capture is given as PATH or PATH:DICT.rri (a bare frame stream whose dictionary comes from an RRI; it is
passed to srdump with --dict and to the reader alike). With --log-writer EXE, EXE writes <tmp>/log.ser first and
that capture is checked too.

usage: parity.py --srdump S --package python/ [--log-writer EXE] CAPTURE[:DICT.rri] ...
Exit 0 on parity, 1 on the first mismatch (printed), 77 when srdump cannot run here. Standard library only.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile


def fail(msg: str) -> bool:
    print("FAIL: " + msg)
    return False


# ---- semantic comparison ---------------------------------------------------------------------------------------------
def same(a, b, path: str = "") -> str | None:
    """None when a and b are the same JSON value (numbers within tolerance), else where they differ."""
    if isinstance(a, bool) or isinstance(b, bool):
        return None if a is b or a == b else f"{path}: {a!r} != {b!r}"
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(a, float) or isinstance(b, float):
            return None if math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-12) else f"{path}: {a!r} != {b!r}"
        return None if a == b else f"{path}: {a!r} != {b!r}"
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys():
            return f"{path}: keys {sorted(a)} != {sorted(b)}"
        for k in a:
            r = same(a[k], b[k], f"{path}.{k}")
            if r:
                return r
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f"{path}: {len(a)} items != {len(b)} items"
        for i, (x, y) in enumerate(zip(a, b)):
            r = same(x, y, f"{path}[{i}]")
            if r:
                return r
        return None
    if type(a) is not type(b):
        return f"{path}: {a!r} ({type(a).__name__}) != {b!r} ({type(b).__name__})"
    return None if a == b else f"{path}: {a!r} != {b!r}"


def parse_stream(text: str) -> list:
    """Every JSON value in srdump's / the reader's output, in order (the dictionary object, then one per line)."""
    dec = json.JSONDecoder()
    out, i, n = [], 0, len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        obj, i = dec.raw_decode(text, i)
        out.append(obj)
    return out


def describe(o) -> str:
    s = json.dumps(o, ensure_ascii=False)
    return s if len(s) <= 300 else s[:300] + "..."


# ---- the two sides ---------------------------------------------------------------------------------------------------
class Env:
    def __init__(self, a: argparse.Namespace) -> None:
        self.srdump = a.srdump
        self.package = os.path.abspath(a.package)
        self.work = tempfile.mkdtemp(prefix="serreg-py-")
        sys.dont_write_bytecode = True  # keep the source tree free of __pycache__
        sys.path.insert(0, self.package)
        import serreg  # noqa: E402  (the package under test)
        self.serreg = serreg
        self.env = dict(os.environ, PYTHONPATH=self.package, PYTHONDONTWRITEBYTECODE="1")

    def run_srdump(self, args: list[str]) -> tuple[int, str]:
        p = subprocess.run([self.srdump] + args, capture_output=True, text=True, errors="replace")
        return p.returncode, p.stdout

    def run_cli(self, args: list[str]) -> tuple[int, str]:
        p = subprocess.run([sys.executable, "-m", "serreg"] + args, capture_output=True, text=True,
                           errors="replace", env=self.env)
        return p.returncode, p.stdout

    def load(self, path: str, rri: str | None):
        with open(path, "rb") as fh:
            data = fh.read()
        extra = []
        if rri:
            d, err = self.serreg.load_rri(rri)
            if err:
                raise RuntimeError(f"{rri}: {err}")
            extra.append(d)
        return data, extra


def py_lines(env: Env, data: bytes, extra, min_level=None) -> list:
    """The reader's JSON objects, round-tripped through its own serializer so numbers are plain JSON values."""
    serreg = env.serreg
    r = serreg.Reader(data, extra)
    if r.error:
        raise RuntimeError(f"reader: {r.error}")
    return [json.loads(serreg.json_dumps(o)) for o in serreg.json_lines(r, min_level)]


def py_dictionary(env: Env, data: bytes, extra) -> dict:
    r = env.serreg.Reader(data, extra, decode=False)
    for _ in r.frames():
        pass
    return json.loads(json.dumps(r.dictionary.to_json_obj()))


# ---- checks ----------------------------------------------------------------------------------------------------------
def check_dump(env: Env, path: str, rri: str | None) -> bool:
    dict_args = ["--dict", rri] if rri else []
    rc, out = env.run_srdump(["--json"] + dict_args + [path])
    ref = parse_stream(out)
    if not ref or not isinstance(ref[0], dict) or "types" not in ref[0]:
        return fail(f"{path}: srdump --json printed no dictionary (rc {rc})")
    data, extra = env.load(path, rri)
    d = same(ref[0], py_dictionary(env, data, extra), "dictionary")
    if d:
        return fail(f"{path}: dictionary differs: {d}")
    mine = py_lines(env, data, extra)
    theirs = ref[1:]
    n_rec = sum(1 for o in theirs if "record" in o)
    n_msg = sum(1 for o in theirs if "message" in o)
    n_log = sum(1 for o in theirs if "log" in o)
    n_prob = sum(1 for o in theirs if "problem" in o)
    for i, (a, b) in enumerate(zip(theirs, mine)):
        d = same(a, b, f"line {i}")
        if d:
            return fail(f"{path}: {d}\n  srdump: {describe(a)}\n  python: {describe(b)}")
    if len(theirs) != len(mine):
        extra_side = "srdump" if len(theirs) > len(mine) else "python"
        o = (theirs if len(theirs) > len(mine) else mine)[min(len(theirs), len(mine))]
        return fail(f"{path}: {len(theirs)} srdump lines vs {len(mine)} python lines; first only in {extra_side}: "
                    f"{describe(o)}")
    frames = {o["frame"] for o in theirs if "frame" in o}
    kinds = sorted({o["type"] if isinstance(o.get("type"), str) else "?" for o in theirs if "record" in o})
    print(f"ok  dump    {path}: {len(frames)} frames, {n_rec} records, {n_msg} messages, {n_log} log lines, "
          f"{n_prob} problems; record types: {', '.join(kinds)}")
    return True


def check_verify(env: Env, path: str, rri: str | None) -> bool:
    dict_args = ["--dict", rri] if rri else []
    rc, out = env.run_srdump(["--verify", "--json"] + dict_args + [path])
    try:
        ref = json.loads(out)
    except json.JSONDecodeError:
        return fail(f"{path}: srdump --verify --json: unreadable output (rc {rc}): {out[:200]}")
    data, extra = env.load(path, rri)
    rep = env.serreg.Reader(data, extra).verify()
    mine = {"ok": rep.ok, "frames": rep.frames, "records": rep.records, "bytes": rep.bytes}
    want = {k: ref[k] for k in mine}
    d = same(want, mine, "verify")
    if d:
        return fail(f"{path}: verify differs: {d} (srdump problems: {ref.get('problems')}, python: {rep.problems})")
    if (rc == 0) != ref["ok"]:
        return fail(f"{path}: srdump --verify exit {rc} but ok={ref['ok']}")
    # the command line prints the same counts in srdump's text form
    rc2, text = env.run_cli(["--verify"] + dict_args + [path])
    m = re.search(r"^(ok|FAILED \(\d+ problems\)): (\d+) frames, (\d+) records, (\d+) bytes$", text, re.M)
    if not m:
        return fail(f"{path}: python --verify printed no summary line: {text[:200]}")
    cli = {"ok": m.group(1) == "ok", "frames": int(m.group(2)), "records": int(m.group(3)), "bytes": int(m.group(4))}
    d = same(want, cli, "cli verify")
    if d:
        return fail(f"{path}: python --verify output differs: {d}")
    if (rc2 == 0) != ref["ok"]:
        return fail(f"{path}: python --verify exit {rc2} but srdump says ok={ref['ok']}")
    print(f"ok  verify  {path}: {m.group(0)}")
    return True


def is_17_file(path: str) -> bool:
    with open(path, "rb") as fh:
        h = fh.read(8)
    return len(h) == 8 and h[:4] == b"ESSR" and int.from_bytes(h[6:8], "little") >= 7


def check_info(env: Env, path: str) -> bool:
    rc, out = env.run_srdump(["--info", "--json", path])
    try:
        ref = json.loads(out)
    except json.JSONDecodeError:
        return fail(f"{path}: srdump --info --json: unreadable output (rc {rc}): {out[:200]}")
    with open(path, "rb") as fh:
        data = fh.read()
    ci = env.serreg.capture_info(data)
    if isinstance(ci, str):
        return fail(f"{path}: python capture_info: {ci}")
    mine = json.loads(env.serreg.json_dumps(ci.to_json_obj()))
    for key in ("format", "program_hash", "size", "crc", "dictionary_offsets", "types", "literals", "calls",
                "checkpoints", "writer"):
        d = same(ref.get(key), mine.get(key), f"info.{key}")
        if d:
            return fail(f"{path}: --info differs: {d}")
    rc2, text = env.run_cli(["--info", "--json", path])
    d = same(ref, json.loads(text) if text.strip() else None, "cli info")
    if d:
        return fail(f"{path}: python --info --json differs: {d}")
    print(f"ok  info    {path}: {len(ref['dictionary_offsets'])} dictionary block(s), "
          f"{len(ref['checkpoints'])} checkpoint(s), {len(ref['writer'])} writer item(s)")
    return True


def check_level(env: Env, path: str, rri: str | None) -> bool:
    dict_args = ["--dict", rri] if rri else []
    rc, out = env.run_srdump(["--json", "--level", "trace"] + dict_args + [path])
    theirs = parse_stream(out)
    if not any("log" in o for o in theirs):
        return True  # no log frames: nothing to compare
    data, extra = env.load(path, rri)
    mine = py_lines(env, data, extra, min_level=0)
    d = same(theirs, mine, "level")
    if d:
        return fail(f"{path}: --level trace differs: {d}")
    rc2, text = env.run_cli(["--json", "--level", "trace"] + dict_args + [path])
    d = same(theirs, parse_stream(text), "cli level")
    if d:
        return fail(f"{path}: python --level trace differs: {d}")
    print(f"ok  level   {path}: {len(theirs)} log lines")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--srdump", required=True)
    ap.add_argument("--package", required=True, help="the directory holding the serreg package (python/)")
    ap.add_argument("--log-writer", help="python_log_capture: writes <tmp>/log.ser, checked as well")
    ap.add_argument("captures", nargs="+", metavar="CAPTURE[:DICT.rri]")
    a = ap.parse_args()
    env = Env(a)
    rc, out = env.run_srdump(["--version"])
    if rc != 0:
        print(f"SKIP: {a.srdump} --version failed ({rc}); cannot compare without the reference tool")
        return 77
    print(out.strip())
    items: list[tuple[str, str | None]] = []
    if a.log_writer:
        log = os.path.join(env.work, "log.ser")
        p = subprocess.run([a.log_writer, log], capture_output=True, text=True)
        if p.returncode != 0 or not os.path.exists(log):
            fail(f"{a.log_writer} failed ({p.returncode}): {p.stderr}")
            return 1
        items.append((log, None))
    for spec in a.captures:
        path, _, rri = spec.partition(":")
        items.append((path, rri or None))
    ok = True
    for path, rri in items:
        if not os.path.exists(path):
            ok = fail(f"{path}: no such file")
            continue
        ok = check_dump(env, path, rri) and ok
        ok = check_verify(env, path, rri) and ok
        ok = check_level(env, path, rri) and ok
        if is_17_file(path):
            ok = check_info(env, path) and ok
    shutil.rmtree(env.work, ignore_errors=True)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
