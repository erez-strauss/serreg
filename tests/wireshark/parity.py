#!/usr/bin/env python3
"""tests/wireshark/parity.py — the generated Wireshark dissector decodes what srdump decodes (2.1.9).

For each check, srdump --wireshark writes a dissector, tshark decodes a capture with it (-T pdml), and every value
field of every frame (literals, scalar and string arguments, struct members, enumerators, strings, vector and
array elements — in order) is compared with srdump --json on the same capture; rendered print messages are
compared too. Checks:
  fixture <capture.ser>   the capture's FILE reader path (packet 1 = header + dictionary, then one frame each)
  backpatched             orders.ser rewritten with its dictionary relocated to the end (FILE flag bit 2)
  user0                   a pcap with link type USER0: a dictionary frame, then the frames of orders.ser
  udp / tcp               the frames of orders.ser as UDP datagrams / TCP segments (one frame split in two)
  mismatch                a dissector generated from orders.ser reading dynamic.ser: unknown entries and calls
                          are reported (expert infos), nothing is decoded with a wrong layout, no Lua error

With --lua L instead of --tshark (2.1.11), the dissector runs under the plain interpreter L (lua5.2, lua5.4, …)
through tests/wireshark/wslua_stub.lua, which implements the part of Wireshark's Lua API the dissector uses and
prints PDML; only the checks on FILE captures (fixture, backpatched, prints, elf) apply. Wireshark 4.6 and later
embed Lua 5.4, earlier versions 5.2; this runs both without either Wireshark.

usage: parity.py --srdump S (--tshark T [--text2pcap P] | --lua L) --fixtures DIR CHECK [CHECK ...]
Exit 0 all checks pass, 1 a mismatch, 77 skipped (tshark cannot run Lua here). Standard library only.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

STRUCTURAL = ("serreg.frame.", "serreg.file.", "serreg.dict.", "serreg.record.", "serreg.call.")
SKIP_FIELDS = {"serreg.type", "serreg.message"}


def fail(msg):
    print("FAIL: " + msg)
    return False


class Env:
    def __init__(self, a):
        self.srdump, self.tshark, self.text2pcap, self.fixtures = a.srdump, a.tshark, a.text2pcap, a.fixtures
        self.lua = a.lua
        self.stub = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wslua_stub.lua")
        self.work = tempfile.mkdtemp(prefix="serreg-ws-", dir="/var/tmp" if os.path.isdir("/var/tmp") else None)
        os.chmod(self.work, 0o755)
        self.prefix = []
        if not self.lua and os.geteuid() == 0:  # Wireshark does not run Lua scripts for root
            runuser = shutil.which("runuser")
            if not runuser:
                print("SKIP: running as root and no runuser: tshark would not load the Lua dissector")
                sys.exit(77)
            self.prefix = [runuser, "-u", "nobody", "--"]

    def path(self, name):
        return os.path.join(self.work, name)

    def put(self, name, data):
        p = self.path(name)
        with open(p, "wb") as f:
            f.write(data)
        os.chmod(p, 0o644)
        return p

    def generate(self, capture, name):
        lua = self.path(name)
        r = subprocess.run([self.srdump, "--wireshark", lua, capture], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("srdump --wireshark failed: " + r.stderr)
        os.chmod(lua, 0o644)
        return lua

    def tshark_pdml(self, lua, capture, extra=()):
        if self.lua:  # 2.1.11: the dissector under a plain interpreter, through the API stub
            if extra:
                raise RuntimeError("--lua runs FILE captures only (no preferences)")
            r = subprocess.run([self.lua, self.stub, lua, capture], capture_output=True, text=True)
            if r.returncode != 0 or r.stderr.strip():
                raise RuntimeError("%s wslua_stub.lua failed (%d): %s" % (self.lua, r.returncode, r.stderr))
            return ET.fromstring(r.stdout)
        cmd = self.prefix + [self.tshark, "-n", "-X", "lua_script:" + lua, "-r", capture, "-T", "pdml", *extra]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("tshark failed (%d): %s" % (r.returncode, r.stderr))
        noise = [ln for ln in r.stderr.splitlines() if ln.strip() and "Running as user" not in ln]
        if noise:
            raise RuntimeError("tshark stderr: " + "\n".join(noise))
        return ET.fromstring(r.stdout)

    def srdump_records(self, capture):
        r = subprocess.run([self.srdump, "--json", capture], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("srdump --json failed: " + r.stderr)
        frames = {}
        messages = {}
        for line in r.stdout.splitlines():
            if not line.startswith('{"frame"'):
                continue
            o = json.loads(line)
            if "message" in o:
                messages[o["frame"]] = o["message"].rstrip("\n")
                continue
            frames.setdefault(o["frame"], []).append(o)
        return frames, messages


# ---- expected leaves from srdump --json ------------------------------------------------------------------------
def leaves_json(v, out):
    if v is None:
        return
    if isinstance(v, dict):
        keys = list(v.keys())
        # an enumerator; srdump --json writes integers beyond 2^53 as strings (2.1.12: a 64-bit enum value)
        if keys == ["name", "value"] and (isinstance(v["value"], int) or
                                          (isinstance(v["value"], str) and v["value"].lstrip("-").isdigit())):
            out.append(str(v["value"]))
        elif keys == ["alt", "type", "value"]:  # a variant alternative
            leaves_json(v["value"], out)
        elif keys == ["lit", "id"]:
            out.append(v["lit"])
        else:
            for k in keys:
                leaves_json(v[k], out)
    elif isinstance(v, list):
        for e in v:
            leaves_json(e, out)
    elif isinstance(v, bool):
        out.append("true" if v else "false")
    else:
        out.append(str(v))


def expected_frame(records):
    out = []
    for rec in records:
        leaves_json(rec["value"], out)
    return out


# ---- actual leaves from tshark -T pdml -------------------------------------------------------------------------
EXPERT_FIELDS = {"serreg.unknown_call", "serreg.unknown_type", "serreg.dict.unknown_entries",
                 "serreg.dict.layout_mismatch", "serreg.dict.program_differs", "serreg.abi_differs",
                 "serreg.malformed", "serreg.noncanonical_bool", "serreg.record_count"}


def serreg_leaves(packet):
    """(value leaves in order, rendered messages, expert info texts, Lua/malformed errors) of one packet."""
    out, msgs, experts, errors = [], [], [], []
    for f in packet.iter("field"):
        name = f.get("name", "")
        if name == "_ws.expert":
            experts.append(f.get("showname", ""))
        elif name in ("_ws.malformed", "_ws.lua.error") or "Lua Error" in f.get("showname", "") or \
                "Dissector bug" in f.get("showname", ""):
            errors.append(f.get("showname", name))  # _ws.lua.text / _ws.lua.fake are ordinary text items
        elif name == "serreg.message":
            msgs.append(f.get("show", ""))
        elif name.startswith("serreg.") and not name.startswith(STRUCTURAL) and name not in SKIP_FIELDS \
                and name not in EXPERT_FIELDS:
            out.append(f.get("show", ""))
    return out, msgs, experts, errors


def norm(s):
    s = s.split("\x00")[0].split("\\000")[0].split("\\x00")[0]
    low = s.lower()
    if low in ("true", "false"):
        return low
    return s


def same(a, b):
    na, nb = norm(a), norm(b)
    if na == nb:
        return True
    try:  # a char or byte field is shown in hex by tshark (0x5a), in decimal by srdump --json (90)
        return int(na, 0) == int(nb, 0)
    except ValueError:
        pass
    try:
        fa, fb = float(na), float(nb)
        return math.isclose(fa, fb, rel_tol=1e-6, abs_tol=1e-9)
    except ValueError:
        return False


def compare(env, lua, capture, packet_to_frame, source=None, extra=()):
    """packet_to_frame: packet number (1-based) → frame index, or None to skip the packet."""
    frames, messages = env.srdump_records(source or capture)
    root = env.tshark_pdml(lua, capture, extra)
    ok = True
    seen = set()
    for n, pkt in enumerate(root.iter("packet"), start=1):
        fi = packet_to_frame(n)
        leaves, msgs, experts, errors = serreg_leaves(pkt)
        if errors:
            ok = fail("packet %d: %s" % (n, errors))
        if fi is None:
            continue
        seen.add(fi)
        want = expected_frame(frames.get(fi, []))
        if len(want) != len(leaves) or not all(same(a, b) for a, b in zip(want, leaves)):
            ok = fail("frame %d: srdump %r\n                tshark %r" % (fi, want, leaves))
        if fi in messages:
            if msgs != [messages[fi]]:
                ok = fail("frame %d message: srdump %r, tshark %r" % (fi, messages[fi], msgs))
    missing = set(frames) - seen
    if missing:
        ok = fail("frames not seen by tshark: %s" % sorted(missing))
    return ok


# ---- captures derived from orders.ser --------------------------------------------------------------------------
def split_file(data):
    minor = struct.unpack_from("<H", data, 6)[0]
    flags, dict_off, dict_len = struct.unpack_from("<I", data, 8)[0], *struct.unpack_from("<QQ", data, 24)
    assert dict_off == 48
    first = (48 + dict_len + 7) & ~7
    if minor >= 7:
        first += 16  # 1.7: the CHAIN (next_dictionary, first_checkpoint) follows the opening dictionary
    rri = data[48:48 + dict_len]
    frames, p = [], first
    while p + 16 <= len(data):
        flen = struct.unpack_from("<I", data, p)[0]
        if flen < 16 or flen % 8:
            raise RuntimeError("split_file: bad frame_len %d at %d" % (flen, p))
        frames.append(data[p:p + flen])  # 1.7: meta frames (writer info, checkpoints) stay in the list — they
        p += flen                        # are frames, numbered like the others, with no value leaves
    return data[:48], rri, frames


def backpatched(data):
    # a 1.6 file whose dictionary was rewritten at the end (FILE flag bit 2); 1.7 writers never rewrite, so the
    # input must be a 1.6 file (the corpus keeps orders-1.6.ser for this and the 1.6-compatibility tests)
    assert struct.unpack_from("<H", data, 6)[0] == 6, "backpatched: needs a 1.6 capture"
    header, rri, frames = split_file(data)
    body = header + rri + b"\x00" * ((-len(rri)) % 8) + b"".join(frames)
    flags = struct.unpack_from("<I", header, 8)[0] | 4
    out = bytearray(body + rri)
    struct.pack_into("<I", out, 8, flags)
    struct.pack_into("<Q", out, 24, len(body))
    return bytes(out)


def dict_frame(rri):
    payload = rri + b"\x00" * ((-len(rri)) % 8)
    return struct.pack("<IHHQ", 16 + len(payload), 2, 0, 0x4553535244494354) + payload


def pcap(packets, linktype):
    out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, linktype)
    for i, p in enumerate(packets):
        out += struct.pack("<IIII", i, 0, len(p), len(p)) + p
    return out


def text2pcap(env, packets, proto_args, name):
    lines = []
    for p in packets:
        for off in range(0, len(p), 16):
            lines.append("%06x " % off + " ".join("%02x" % b for b in p[off:off + 16]))
        lines.append("")
    src = env.put(name + ".txt", "\n".join(lines).encode())
    out = env.path(name + ".pcap")
    r = subprocess.run([env.text2pcap, "-q", *proto_args, src, out], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("text2pcap: " + r.stderr)
    os.chmod(out, 0o644)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--srdump", required=True)
    ap.add_argument("--tshark")
    ap.add_argument("--lua")
    ap.add_argument("--text2pcap")
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("checks", nargs="+")
    a = ap.parse_args()
    if bool(a.tshark) == bool(a.lua):
        ap.error("give exactly one of --tshark and --lua")
    env = Env(a)
    ok = True
    try:
        orders_src = os.path.join(a.fixtures, "orders.ser")
        orders = env.put("orders.ser", open(orders_src, "rb").read())
        for check in a.checks:
            if check.startswith("fixture:"):
                cap = env.put(os.path.basename(check[8:]), open(check[8:], "rb").read())
                lua = env.generate(cap, "fixture.lua")
                good = compare(env, lua, cap, lambda n: None if n == 1 else n - 2)
            elif check == "backpatched":
                old = os.path.join(a.fixtures, "..", "corpus", "capture", "orders-1.6.ser")
                cap = env.put("backpatched.ser", backpatched(open(old, "rb").read()))
                lua = env.generate(cap, "backpatched.lua")
                n_frames = len(split_file(open(old, "rb").read())[2])
                good = compare(env, lua, cap, lambda n: None if n == 1 or n > n_frames + 1 else n - 2)
            elif check == "user0":
                _, rri, frames = split_file(open(orders, "rb").read())
                cap = env.put("user0.pcap", pcap([dict_frame(rri)] + frames, 147))
                lua = env.generate(orders, "user0.lua")
                good = compare(env, lua, cap, lambda n: None if n == 1 else n - 2, source=orders)
            elif check == "udp":
                _, _, frames = split_file(open(orders, "rb").read())
                cap = text2pcap(env, frames, ["-u", "40000,9999"], "udp")
                lua = env.generate(orders, "udp.lua")
                good = compare(env, lua, cap, lambda n: n - 1, source=orders, extra=("-o", "serreg.udp_port:9999"))
            elif check == "udp-heuristic":
                # no port setting: the UDP heuristic recognises frames by their call hash (2.1.10)
                _, _, frames = split_file(open(orders, "rb").read())
                cap = text2pcap(env, frames, ["-u", "40001,39999"], "udph")
                lua = env.generate(orders, "udph.lua")
                good = compare(env, lua, cap, lambda n: n - 1, source=orders)
            elif check.startswith("prints:"):
                # print frames covering std::format replacement fields: the messages must equal srdump's
                cap = env.path("print_specs.ser")
                r = subprocess.run([check[7:], cap], capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError("print capture writer failed: " + r.stderr)
                os.chmod(cap, 0o644)
                lua = env.generate(cap, "prints.lua")
                good = compare(env, lua, cap, lambda n: None if n == 1 else n - 2)
            elif check in ("tcp", "tcp-heuristic"):
                _, _, frames = split_file(open(orders, "rb").read())
                segs = [frames[0][:20], frames[0][20:] + frames[1]] + frames[2:]  # frame 0 split across segments
                heuristic = check == "tcp-heuristic"  # 2.1.10: no port setting, recognised by the call hash
                cap = text2pcap(env, segs, ["-T", "40002,39998" if heuristic else "40000,9998"], check)
                lua = env.generate(orders, check + ".lua")
                # packet 1 carries a partial frame (no values); packet 2 completes frame 0 and holds frame 1
                frames_json, _ = env.srdump_records(orders)
                opts = ("-o", "tcp.desegment_tcp_streams:TRUE") + (() if heuristic else ("-o", "serreg.tcp_port:9998"))
                root = env.tshark_pdml(lua, cap, opts)
                got = [serreg_leaves(p)[0] for p in root.iter("packet")]
                want = [[], expected_frame(frames_json.get(0, [])) + expected_frame(frames_json.get(1, []))] + \
                       [expected_frame(frames_json.get(i, [])) for i in range(2, len(frames))]
                good = len(got) == len(want) and all(
                    len(g) == len(w) and all(same(x, y) for x, y in zip(w, g)) for g, w in zip(got, want))
                if not good:
                    fail("%s: want %r\n     got %r" % (check, want, got))
            elif check.startswith("elf:"):
                # elf:<binary>:<capture> — the dissector comes from the program's es_serreg_dict section
                binary, capture = check[4:].split(":", 1)
                cap = env.put(os.path.basename(capture), open(capture, "rb").read())
                lua = env.path("elf.lua")
                r = subprocess.run([env.srdump, "--wireshark", lua, "--elf", binary], capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError("srdump --wireshark --elf: " + r.stderr)
                os.chmod(lua, 0o644)
                good = compare(env, lua, cap, lambda n: None if n == 1 else n - 2)
            elif check == "fuzz":
                # 3000 corrupted frames (byte flips, truncation, garbage lengths) in one USER0 pcap: every packet
                # must dissect without a Lua error or a dissector bug (malformed expert infos are expected)
                import random
                rnd = random.Random(2190)
                pool = split_file(open(orders, "rb").read())[2]
                dyn = open(os.path.join(a.fixtures, "dynamic.ser"), "rb").read()
                pool += split_file(dyn)[2]
                pkts = []
                for _ in range(3000):
                    f = bytearray(rnd.choice(pool))
                    for _ in range(rnd.randint(1, 4)):
                        i = rnd.randrange(len(f))
                        f[i] = rnd.randrange(256) if rnd.random() < 0.7 else f[i] ^ 0x80
                    if rnd.random() < 0.2:
                        f = f[:rnd.randrange(len(f))]
                    pkts.append(bytes(f))
                cap = env.put("fuzz.pcap", pcap(pkts, 147))
                both = env.put("dynamic.ser", dyn)
                lua = env.path("fuzz.lua")
                r = subprocess.run([env.srdump, "--wireshark", lua, "--dict", os.path.join(a.fixtures, "orders.rri"),
                                    both], capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError("srdump --wireshark: " + r.stderr)
                os.chmod(lua, 0o644)
                root = env.tshark_pdml(lua, cap)
                good = True
                count = malformed = 0
                for n, p in enumerate(root.iter("packet"), start=1):
                    count += 1
                    _, _, experts, errors = serreg_leaves(p)
                    malformed += 1 if errors or any("Malformed" in e for e in experts) else 0
                    real = [e for e in errors if "Malformed" not in e and e != "_ws.malformed"]
                    if real:
                        good = fail("fuzz packet %d: %s" % (n, real[:3]))
                        break
                if count != len(pkts):
                    good = fail("fuzz: tshark showed %d of %d packets" % (count, len(pkts)))
                print("     fuzz: %d of %d corrupted packets reported as malformed, none crashed" % (malformed, count))
            elif check == "mismatch":
                dyn = env.put("dynamic.ser", open(os.path.join(a.fixtures, "dynamic.ser"), "rb").read())
                lua = env.generate(orders, "mismatch.lua")
                root = env.tshark_pdml(lua, dyn)
                pkts = list(root.iter("packet"))
                good = True
                counts = {f.get("name"): f.get("show") for f in pkts[0].iter("field")
                          if f.get("name") in ("serreg.dict.known", "serreg.dict.unknown")}
                # dynamic.ser's 20 entries (1.6: 9 types, 4 literals, 7 calls): corpus::Order and corpus::Side are
                # orders.ser's too, the other 18 are not
                if counts != {"serreg.dict.known": "2", "serreg.dict.unknown": "18"}:
                    good = fail("mismatch: dictionary counts %r, want 2 known, 18 unknown" % counts)
                for n, p in enumerate(pkts[1:], start=2):
                    leaves, _, experts, errors = serreg_leaves(p)
                    names = [f.get("name") for f in p.iter("field")]
                    if n == 2 and not leaves and not errors and not experts:
                        continue  # 1.7: packet 2 is the writer-info frame — no call, nothing unknown about it
                    if errors or leaves or not any("unknown to this dissector" in e.lower() for e in experts):
                        good = fail("mismatch packet %d: leaves %r experts %r errors %r" % (n, leaves, experts, errors))
            else:
                raise RuntimeError("unknown check " + check)
            print(("ok   " if good else "FAIL ") + check)
            ok = ok and good
    except RuntimeError as e:
        print("FAIL: %s" % e)
        ok = False
    finally:
        shutil.rmtree(env.work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
