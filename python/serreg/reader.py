"""serreg.reader — walks a capture (a FILE, or a bare frame stream with an external RRI), cuts frames into
records and decodes record payloads into plain Python values. Mirrors src/reader.cpp (the walker), src/decoder.cpp
(the dictionary-driven image decoder) and src/formatters.cpp (JSON and text renderings, print/log messages).

Nothing here raises on corrupt input: structural problems are collected in `Reader.problems` as text, the walk
stops where the C++ walker stops (a bad frame_len) and skips what it skips (unknown frame flags, a CRC mismatch);
a record whose payload does not decode carries `error` instead of `value`.

Decoded values
    scalar          int, float, bool; a char/char8 is the signed/unsigned byte value (as srdump's JSON)
    enum            Enum(type_name, name, value)
    struct          Struct(type_name, fields: list of (name, value))
    array / vector  Array(items, chars) — `chars` is the text when every element is a char
    string          Str(raw bytes)
    optional        the value, or Null
    variant         Variant(index, alt_text, value) or Null (valueless)
    map / set       Map(entries: list of (key, value)) / Set(items)
    literal record  Lit(id, text or None)
    transparent     Transparent(value) — a container passed as an argument; renderers show the value alone
"""
from __future__ import annotations

import datetime as _dt
import math
import re
import struct
from dataclasses import dataclass, field as dc_field
from typing import Iterator

from . import format as F
from .dictionary import Call, Dictionary, Field, Type, decode_text, hex64
from .format import scalar_value

# error names a Record.error / Reader.error / DecodeError can carry (the C++ es::serreg::error spellings)
ERROR_NAMES = ("truncated", "bad_magic", "bad_version", "endianness_mismatch", "hash_mismatch",
               "length_mismatch", "unknown_type", "unknown_literal", "unknown_call", "corrupt_dictionary",
               "unsupported", "invalid_argument", "crc_mismatch")


class DecodeError(Exception):
    """A record payload the dictionary-driven decoder refuses; str(e) is the C++ error name."""


# ---- decoded values ----------------------------------------------------------------------------------------------
@dataclass
class Enum:
    type_name: str
    name: str  # "" when the value has no enumerator (or the enum type is unknown)
    value: int


@dataclass
class Struct:
    type_name: str
    fields: list[tuple[str, object]] = dc_field(default_factory=list)
    descs: list[Field] = dc_field(default_factory=list)  # the FIELD of each member, for the renderers


@dataclass
class Array:
    items: list = dc_field(default_factory=list)
    chars: bytes | None = None  # every element was a char: the text (arrays of char render as strings)
    elem: Field | None = None


@dataclass
class Str:
    raw: bytes

    @property
    def text(self) -> str:
        return decode_text(self.raw)


@dataclass
class Variant:
    index: int
    alt_text: str  # "variant<i:type>"
    value: object
    alt: Field | None = None


@dataclass
class Map:
    entries: list[tuple[object, object]] = dc_field(default_factory=list)
    key: Field | None = None
    val: Field | None = None


@dataclass
class Set:
    items: list = dc_field(default_factory=list)
    elem: Field | None = None


@dataclass
class Lit:
    id: int
    text: str | None  # None when the literal is not in the dictionary


@dataclass
class Transparent:
    value: object
    desc: Field | None = None


class _NullType:
    def __repr__(self) -> str:
        return "Null"


Null = _NullType()


def float_text(v: float, single: bool = False) -> str:
    """std::to_chars(value): the fewest digits that read back as the same double (or, with `single`, the same
    float32), in "%f" or "%e" style whichever is shorter (ties go to fixed): 100 -> "100", 1e21 -> "1e+21"."""
    if v != v or v in (math.inf, -math.inf):
        return "nan" if v != v else ("inf" if v > 0 else "-inf")
    return _shortest(v, single)


def _shortest(v: float, single: bool) -> str:
    if v == 0:
        return "-0" if math.copysign(1.0, v) < 0 else "0"
    for p in range(0, 17):
        s = "%.*e" % (p, v)
        back = float(s)
        if single:
            if struct.pack("<f", back) == struct.pack("<f", v):
                break
        elif back == v:
            break
    mant, exp_s = s.split("e")
    exp = int(exp_s)
    neg = mant.startswith("-")
    digits = mant.lstrip("-").replace(".", "")
    digits = digits.rstrip("0") or "0"
    sci = digits[0] + ("." + digits[1:] if len(digits) > 1 else "") + "e%+03d" % exp
    if exp >= 0:
        if len(digits) <= exp + 1:
            fixed = digits + "0" * (exp + 1 - len(digits))
        else:
            fixed = digits[:exp + 1] + "." + digits[exp + 1:]
    else:
        fixed = "0." + "0" * (-exp - 1) + digits
    out = fixed if len(fixed) <= len(sci) else sci
    return "-" + out if neg else out


# ---- the dictionary-driven image decoder (src/decoder.cpp) ---------------------------------------------------------
class _Budget:
    """The slot rules of a canonical image: every non-empty block starts at or after the root's fixed part, lies
    inside the image, and all blocks together are at most image-size bytes (src/format.hpp image_budget)."""

    __slots__ = ("image", "fixed", "left", "steps")

    def __init__(self, image, fixed: int) -> None:
        self.image = image
        self.fixed = fixed
        self.left = len(image)
        self.steps = (2 * _MAX_DEPTH + 2) * (len(image) + 1) + 65536

    def take(self, off: int, n: int, nbytes: int) -> bool:
        if n == 0:
            return off == 0
        if off < self.fixed or off > len(self.image) or nbytes > len(self.image) - off or nbytes > self.left:
            return False
        self.left -= nbytes
        return True

    def step(self) -> None:
        if self.steps == 0:
            raise DecodeError("length_mismatch")
        self.steps -= 1


_MAX_DEPTH = 2 * F.MAX_NESTING


def _read_slot(b) -> tuple[int, int]:
    if len(b) < F.SLOT_SIZE:
        raise DecodeError("length_mismatch")
    return F.SLOT.unpack_from(b, 0)


def _enum_value(b, underlying: int) -> int:
    """An enum's value from 1, 2, 4 or 8 bytes, sign-extended when the underlying kind is signed."""
    if len(b) not in (1, 2, 4, 8):
        raise DecodeError("length_mismatch")
    return int.from_bytes(b, "little", signed=underlying in F.SIGNED_KINDS)


def _alt_text(a: Field, i: int, d: Dictionary) -> str:
    if a.kind in (F.K_STRUCT, F.K_ENUM):
        t = d.find_type(a.type_hash)
        return f"variant<{i}:{t.name if t else hex64(a.type_hash)}>"
    return f"variant<{i}:{F.kind_name(a.kind)}>"


class Decoder:
    """Decodes one record payload against a type. `warnings` collects non-canonical bool bytes (read as true)."""

    def __init__(self, d: Dictionary) -> None:
        self.d = d
        self.warnings: list[str] = []

    def record(self, payload, t: Type) -> object:
        """A record's value: a flat type's native bytes, or a dynamic type's canonical image."""
        c = _Budget(payload, t.size)
        return self._struct(payload, t, "", 0, c)

    def _value(self, b, f: Field, name: str, depth: int, c: _Budget) -> object:
        if depth > _MAX_DEPTH:
            raise DecodeError("unsupported")
        c.step()
        k = f.kind
        if F.is_scalar_kind(k):
            if k == F.K_BOOL and len(b) == 1 and b[0] > 1:
                self.warnings.append(f"{name or 'value'}: bool byte 0x{b[0]:02x} read as true")
                return True
            return scalar_value(k, b)
        if k == F.K_ENUM:
            t = self.d.find_type(f.type_hash)
            v = _enum_value(b, t.underlying if t else F.K_U8)
            return Enum(t.name if t else "", t.enumerator_name(v) if t else "", v)
        image = c.image
        if k == F.K_ARRAY:
            if len(f.elem) != 1:
                raise DecodeError("corrupt_dictionary")
            e = f.elem[0]
            if e.size == 0:
                if f.extent != 0:
                    raise DecodeError("length_mismatch")
            elif f.extent > len(b) // e.size:
                raise DecodeError("length_mismatch")
            stride = F.slot_stride(e.size, e.align)
            items = []
            for i in range(f.extent):
                at = i * stride
                if at > len(b) or e.size > len(b) - at:
                    raise DecodeError("length_mismatch")
                items.append(self._value(b[at:at + e.size], e, "", depth + 1, c))
            return _array(items, e)
        if k == F.K_STRUCT:
            nested = self.d.find_type(f.type_hash)
            if nested is None:
                raise DecodeError("unknown_type")
            return self._struct(b, nested, name, depth + 1, c)
        if k == F.K_STRING:
            off, n = _read_slot(b)
            if not c.take(off, n, n):
                raise DecodeError("length_mismatch")
            return Str(bytes(image[off:off + n]) if n else b"")
        if k == F.K_VECTOR:
            if len(f.elem) != 1:
                raise DecodeError("corrupt_dictionary")
            e = f.elem[0]
            off, n = _read_slot(b)
            stride = F.slot_stride(e.size, e.align)
            if (n and e.size == 0) or n * stride >= 1 << 64 or not c.take(off, n, n * stride):
                raise DecodeError("length_mismatch")
            items = [self._value(image[off + i * stride:off + i * stride + e.size], e, "", depth + 1, c)
                     for i in range(n)]
            return _array(items, e)
        if k in (F.K_MAP, F.K_SET):
            is_map = k == F.K_MAP
            if len(f.elem) != (2 if is_map else 1) or f.extent == 0:
                raise DecodeError("corrupt_dictionary")
            key = f.elem[0]
            off, n = _read_slot(b)
            if n * f.extent >= 1 << 64 or not c.take(off, n, n * f.extent):
                raise DecodeError("length_mismatch")
            if is_map:
                val = f.elem[1]
                entries = []
                for i in range(n):
                    at = off + i * f.extent
                    kv = self._value(image[at:at + key.size], key, "key", depth + 1, c)
                    vv = self._value(image[at + val.offset:at + val.offset + val.size], val, "value",
                                     depth + 1, c)
                    entries.append((kv, vv))
                return Map(entries, key, val)
            return Set([self._value(image[off + i * f.extent:off + i * f.extent + key.size], key, "",
                                    depth + 1, c) for i in range(n)], key)
        if k == F.K_OPTIONAL:
            if len(f.elem) != 1:
                raise DecodeError("corrupt_dictionary")
            e = f.elem[0]
            off, n = _read_slot(b)
            if n > 1 or not c.take(off, n, e.size):
                raise DecodeError("length_mismatch")
            if n == 0:
                return Null
            return self._value(image[off:off + e.size], e, name, depth + 1, c)
        if k == F.K_VARIANT:
            if len(f.elem) != f.extent or f.extent == 0:
                raise DecodeError("corrupt_dictionary")
            off, n = _read_slot(b)
            if n > f.extent:
                raise DecodeError("length_mismatch")
            if n == 0:
                if not c.take(off, 0, 0):
                    raise DecodeError("length_mismatch")
                return Null
            a = f.elem[n - 1]
            if not c.take(off, n, a.size):
                raise DecodeError("length_mismatch")
            return Variant(n - 1, _alt_text(a, n - 1, self.d),
                           self._value(image[off:off + a.size], a, "value", depth + 1, c), a)
        raise DecodeError("unsupported")  # literal members

    def _struct(self, payload, t: Type, name: str, depth: int, c: _Budget) -> object:
        if depth > _MAX_DEPTH:
            raise DecodeError("unsupported")
        c.step()
        if t.is_enum:
            v = _enum_value(payload, t.underlying)
            return Enum(t.name, t.enumerator_name(v), v)
        if len(payload) < t.size:
            raise DecodeError("length_mismatch")
        if t.transparent:
            if len(t.fields) != 1 or t.fields[0].size > len(payload):
                raise DecodeError("corrupt_dictionary")
            f = t.fields[0]
            return Transparent(self._value(payload[:f.size], f, name, depth + 1, c), f)
        s = Struct(t.name, [], t.fields)
        for f in t.fields:
            if f.offset + f.size > len(payload):
                raise DecodeError("length_mismatch")
            s.fields.append((f.name, self._value(payload[f.offset:f.offset + f.size], f, f.name, depth + 1, c)))
        return s


def _array(items: list, e: Field) -> Array:
    if e.kind in F.CHAR_KINDS and e.size == 1 and items:
        return Array(items, bytes(v & 0xFF for v in items), e)
    return Array(items, None, e)


# ---- frames and records ----------------------------------------------------------------------------------------------
@dataclass
class Record:
    index: int
    ak: int = F.ARG_TYPE  # F.ARG_TYPE | ARG_LITERAL | ARG_SCALAR | ARG_STRING (what the record is)
    kind: int = 0  # scalar kind when ak == ARG_SCALAR
    schema_hash: int = 0
    literal_id: int = 0
    type: Type | None = None
    payload: bytes = b""  # object bytes | scalar bytes | string bytes | image (prefixes stripped)
    is_const: bool = False
    value: object = None  # the decoded value (see the module docstring), None when `error` is set
    error: str | None = None

    @property
    def is_literal(self) -> bool:
        return self.ak == F.ARG_LITERAL

    @property
    def type_label(self) -> str:
        """What srdump prints as "type": the kind name, "string", "literal" or the type's name."""
        if self.ak == F.ARG_SCALAR:
            return F.kind_name(self.kind)
        if self.ak == F.ARG_STRING:
            return "string"
        if self.is_literal:
            return "literal"
        return self.type.name if self.type else ""


@dataclass
class Frame:
    index: int  # among all frames, meta frames included
    offset: int
    frame_len: int
    flags: int
    n_records: int
    call_hash: int
    is_full: bool = False
    is_dict: bool = False
    has_crc: bool = False
    is_checkpoint: bool = False
    is_info: bool = False
    call: Call | None = None
    records: list[Record] = dc_field(default_factory=list)
    body: bytes = b""  # the payload between the header and the CRC trailer

    @property
    def is_meta(self) -> bool:
        return self.is_dict or self.is_checkpoint or self.is_info

    @property
    def form(self) -> str:
        return self.call.form if self.call else "call"


@dataclass
class FileHeader:
    major: int
    minor: int
    flags: int
    hash_algo: int
    program_hash: int
    dict_offset: int
    dict_len: int
    abi: int


def parse_file_header(h) -> FileHeader | str:
    """The 48-byte FILE header, or an error name."""
    if len(h) < F.FILE_HEADER_SIZE:
        return "truncated"
    magic, major, minor, flags, algo, prog, doff, dlen, abi = F.FILE_HEADER.unpack_from(h, 0)
    if magic != F.FILE_MAGIC:
        return "bad_magic"
    if major != F.FORMAT_MAJOR or not F.FORMAT_MINOR_OLDEST <= minor <= F.FORMAT_MINOR:
        return "bad_version"
    if not flags & F.FILE_FLAG_LE:
        return "endianness_mismatch"
    if algo != F.HASH_ALGO_XXH3_64:
        return "unsupported"
    return FileHeader(major, minor, flags, algo, prog, doff, dlen, abi)


def rri_length(buf, at: int) -> int | str:
    """Byte length of the RRI at `at` (header + its n_entries entries): the original dictionary area of a
    back-patched 1.6 file, which the frames follow."""
    if len(buf) - at < F.RRI_HEADER_SIZE or bytes(buf[at:at + 4]) != F.RRI_MAGIC:
        return "bad_magic"
    n_entries = F.U32.unpack_from(buf, at + 16)[0]
    pos = at + F.RRI_HEADER_SIZE
    for _ in range(n_entries):
        if len(buf) - pos < 4:
            return "truncated"
        total = F.U32.unpack_from(buf, pos)[0]
        if total < F.ENTRY_HEADER_SIZE or total % 8 != 0:
            return "corrupt_dictionary"
        pos += total
    return pos - at


def classify_frame(buf, at: int, room: int) -> tuple[str, Frame]:
    """Reads one frame header: ("ok" | "bad_len" | "unknown_flags", frame)."""
    frame_len, flags, n_records, call_hash = F.FRAME_HEADER.unpack_from(buf, at)
    f = Frame(0, at, frame_len, flags, n_records, call_hash)
    if frame_len < F.FRAME_HEADER_SIZE or frame_len % 8 != 0 or frame_len > room:
        return "bad_len", f
    f.is_dict = bool(flags & F.FRAME_FLAG_DICT) or call_hash == F.DICT_SENTINEL
    f.is_full = bool(flags & F.FRAME_FLAG_FULL)
    f.has_crc = bool(flags & F.FRAME_FLAG_CRC)
    f.is_checkpoint = bool(flags & F.FRAME_FLAG_CHECKPOINT)
    f.is_info = bool(flags & F.FRAME_FLAG_INFO)
    if flags & ~F.FRAME_FLAGS_KNOWN:
        return "unknown_flags", f
    if f.has_crc and frame_len < F.FRAME_HEADER_SIZE + F.CRC_TRAILER_SIZE:
        return "bad_len", f
    return "ok", f


def check_crc(frame) -> bool:
    """1.7: the trailer is u32 crc32c(frame[0 : len - 8]), u32 0."""
    stored = F.U32.unpack_from(frame, len(frame) - F.CRC_TRAILER_SIZE)[0]
    return F.crc32c(frame[:len(frame) - F.CRC_TRAILER_SIZE]) == stored


class _Stop(Exception):
    """The walk of one frame ends here (a problem that the C++ walker returns on)."""


class Reader:
    """Walks one capture held in memory (bytes, bytearray or mmap).

        r = Reader(data, extra=[dictionary_from_rri])   # extra dictionaries for bare frame streams
        for frame in r.frames():                         # data frames only; meta frames are consumed
            for rec in frame.records: ...
        r.problems, r.frame_count, r.record_count, r.byte_count, r.dictionary

    Counting follows srdump: `frame.index` counts every frame (dictionary, checkpoint and writer-info frames
    included); frame_count and byte_count count data frames only.
    """

    def __init__(self, data, extra: list[Dictionary] | None = None, decode: bool = True) -> None:
        self.data = data
        self.dictionary = Dictionary()
        self.problems: list[str] = []
        self.problems_omitted = 0
        self.warnings: list[str] = []
        self.frame_count = 0
        self.record_count = 0
        self.byte_count = 0
        self.header: FileHeader | None = None
        self.error: str | None = None  # a FILE that cannot be read at all (bad header, bad dictionary)
        self.decode = decode
        self._frames_begin = 0
        self._frames_end = len(data)
        self._open(extra or [])

    # -- set-up: the FILE header and the opening dictionary, or a bare stream
    def _open(self, extra: list[Dictionary]) -> None:
        data = self.data
        n = len(data)
        is_file = n >= 4 and bytes(data[:4]) == F.FILE_MAGIC
        if is_file and n >= F.FILE_HEADER_SIZE:
            # "ESSR" is also a legal frame_len: when the header does not read as a v1 FILE and the capture is large
            # enough for such a frame, it is a bare frame stream
            major = F.U16.unpack_from(data, 4)[0]
            algo = F.U32.unpack_from(data, 12)[0]
            if (major != F.FORMAT_MAJOR or algo != F.HASH_ALGO_XXH3_64) and n >= 0x52535345:
                is_file = False
        if is_file:
            fh = parse_file_header(data)
            if isinstance(fh, str):
                self.error = fh
                return
            self.header = fh
            d = self.dictionary
            d.program_hash = fh.program_hash
            d.abi, d.abi_known = fh.abi, True
            self._frames_begin = F.FILE_HEADER_SIZE
            if fh.flags & F.FILE_FLAG_DICT and fh.dict_len != 0:
                if fh.dict_offset > n or n - fh.dict_offset < fh.dict_len:
                    self.error = "truncated"
                    return
                err = d.add_rri(memoryview(data)[fh.dict_offset:fh.dict_offset + fh.dict_len])
                if err:
                    self.error = err
                    return
                if fh.minor >= 7:  # the CHAIN follows the opening dictionary; deltas are inline frames
                    self._frames_begin = F.align8(fh.dict_offset + fh.dict_len) + F.CHAIN_SIZE
                elif fh.dict_offset == F.FILE_HEADER_SIZE:
                    self._frames_begin = F.align8(fh.dict_offset + fh.dict_len)
                else:  # back-patched: skip the original dictionary area, stop before the relocated one
                    old_len = rri_length(data, F.FILE_HEADER_SIZE)
                    if isinstance(old_len, str):
                        self.error = old_len
                        return
                    self._frames_begin = F.align8(F.FILE_HEADER_SIZE + old_len)
                    self._frames_end = fh.dict_offset
            if self._frames_begin > self._frames_end:
                self.error = "truncated"
                return
        for x in extra:
            self.dictionary.merge(x)

    # -- problems
    def _problem(self, f: Frame, what: str) -> None:
        self._add(f"frame {f.index} @{f.offset}: {what}")

    def _add(self, text: str) -> None:
        if len(self.problems) < F.PROBLEMS_LIMIT:
            self.problems.append(text)
        else:
            self.problems_omitted += 1

    # -- the walk
    def frames(self) -> Iterator[Frame]:
        """Yields the data frames in order, each with its records cut and (by default) decoded. Dictionary
        frames are merged into `dictionary` as they are met; checkpoint and writer-info frames are consumed."""
        if self.error:
            return
        data = self.data
        off, end = self._frames_begin, self._frames_end
        index = 0
        while off < end:
            if end - off < F.FRAME_HEADER_SIZE:
                self._add(f"frame {index} @{off}: {end - off} stray bytes after last frame")
                break
            fc, f = classify_frame(data, off, min(end - off, F.FRAME_BYTES_LIMIT))
            f.index = index
            index += 1
            if fc == "bad_len":
                self._problem(f, f"bad frame_len {f.frame_len} (capture has {end - off} bytes left)")
                break  # the walk cannot resynchronise
            if fc == "unknown_flags":
                self._problem(f, f"unknown frame flags {f.flags} (frame skipped)")
                off += f.frame_len
                continue
            whole = memoryview(data)[off:off + f.frame_len]
            if f.has_crc and not check_crc(whole):
                self._problem(f, "crc mismatch (frame skipped)")
                off += f.frame_len
                continue
            f.body = whole[F.FRAME_HEADER_SIZE:f.frame_len - (F.CRC_TRAILER_SIZE if f.has_crc else 0)]
            if f.is_dict:
                self._dict_frame(f)
            if not f.is_meta:
                self.frame_count += 1
                self.byte_count += f.frame_len
                f.call = self.dictionary.find_call(f.call_hash)
                try:
                    if f.is_full:
                        self._full_frame(f)
                    else:
                        self._compact_frame(f)
                except _Stop:
                    pass
                if self.decode:
                    self._decode_records(f)
                yield f
            off += f.frame_len
        for p in self.dictionary.problems():
            self._add(p)

    def _dict_frame(self, f: Frame) -> None:
        delta = Dictionary()
        err = delta.add_rri(f.body)
        if err:
            self._problem(f, f"dictionary frame: {err}")
            return
        self.dictionary.merge(delta)

    def _emit(self, f: Frame, r: Record) -> None:
        self.record_count += 1
        f.records.append(r)

    def _compact_frame(self, f: Frame) -> None:
        k, body, d = f.call, f.body, self.dictionary
        if k is None:
            self._problem(f, f"unknown call {hex64(f.call_hash)} ({f.n_records} records not decodable)")
            return
        if len(k.args) != f.n_records:
            self._problem(f, f"n_records {f.n_records} != call arity {len(k.args)}")
        pos = 0
        for i, a in enumerate(k.args):
            r = Record(i)
            if a.ak == F.ARG_CONST:  # no bytes in the frame: the value in the call's entry
                r.is_const = True
                r.schema_hash = a.hash
                if F.kind_size(a.kind):
                    r.ak, r.kind, size = F.ARG_SCALAR, a.kind, d.scalar_size(a.kind)
                else:
                    r.ak, r.type = F.ARG_TYPE, d.find_type(a.hash)
                    if r.type is None:
                        self._problem(f, f"record {i}: constant of unknown type {hex64(a.hash)}")
                        raise _Stop()
                    size = r.type.size if r.type.is_flat else F.CONST_VALUE_SIZE + 1
                if size > F.CONST_VALUE_SIZE or (size < F.CONST_VALUE_SIZE and (a.value >> (8 * size)) != 0):
                    self._problem(f, f"record {i}: constant does not fit its type")
                    raise _Stop()
                r.payload = a.value.to_bytes(8, "little")[:size]
                self._emit(f, r)
                continue
            if a.ak == F.ARG_STRING:  # u32 len, bytes
                pos = F.align8(pos)
                if len(body) - pos < F.STRING_LEN_PREFIX:
                    self._problem(f, f"record {i}: string length prefix at {pos} exceeds frame")
                    raise _Stop()
                n = F.U32.unpack_from(body, pos)[0]
                if len(body) - pos - F.STRING_LEN_PREFIX < n:
                    self._problem(f, f"record {i}: string of {n} bytes at {pos} exceeds frame")
                    raise _Stop()
                r.ak, r.schema_hash = F.ARG_STRING, a.hash
                r.payload = bytes(body[pos + F.STRING_LEN_PREFIX:pos + F.STRING_LEN_PREFIX + n])
                pos += F.STRING_LEN_PREFIX + n
                self._emit(f, r)
                continue
            dt = d.find_type(a.hash) if a.ak == F.ARG_TYPE else None
            if dt is not None and not dt.is_flat:  # u32 len, u32 0, canonical image
                pos = F.align8(pos)
                if len(body) - pos < F.IMAGE_PREFIX:
                    self._problem(f, f"record {i}: image prefix at {pos} exceeds frame")
                    raise _Stop()
                n = F.U32.unpack_from(body, pos)[0]
                if len(body) - pos - F.IMAGE_PREFIX < n:
                    self._problem(f, f"record {i}: image of {n} bytes at {pos} exceeds frame")
                    raise _Stop()
                r.ak, r.schema_hash, r.type = F.ARG_TYPE, a.hash, dt
                r.payload = bytes(body[pos + F.IMAGE_PREFIX:pos + F.IMAGE_PREFIX + n])
                pos += F.IMAGE_PREFIX + n
                self._emit(f, r)
                continue
            # fixed size: a literal occupies no bytes, a scalar its kind's size, a flat type its size
            if a.is_literal:
                sz = 0
            elif a.ak == F.ARG_SCALAR:
                sz = d.scalar_size(a.kind)
            elif dt is None:
                self._problem(f, f"record {i}: {hex64(a.hash)}: unknown_type")
                raise _Stop()
            else:
                sz = dt.size
            pos = F.align8(pos)
            if len(body) - pos < sz:
                self._problem(f, f"record {i}: needs {sz} bytes at {pos}, frame has {len(body) - pos}")
                raise _Stop()
            r.payload = bytes(body[pos:pos + sz])
            r.ak, r.kind = a.ak, a.kind
            if a.ak == F.ARG_SCALAR:
                r.schema_hash = a.hash
            elif a.is_literal:
                r.literal_id = a.hash
            else:
                r.schema_hash, r.type = a.hash, dt
            pos += sz
            self._emit(f, r)
        if F.align8(pos) != len(body):
            self._problem(f, f"records end at {pos} but frame body is {len(body)} bytes")
            return
        if any(body[pos:]):
            self._problem(f, "non-zero trailing padding")

    def _full_frame(self, f: Frame) -> None:
        k, body, d = f.call, f.body, self.dictionary
        if k is not None and len(k.args) != f.n_records:
            self._problem(f, f"n_records {f.n_records} != call arity {len(k.args)}")
        pos = 0
        for i in range(f.n_records):
            if len(body) - pos < F.FULL_RECORD_HEADER_SIZE:
                self._problem(f, f"record {i}: truncated record header")
                return
            h, payload_len, layout, _rsv = F.FULL_RECORD_HEADER.unpack_from(body, pos)
            if len(body) - pos - F.FULL_RECORD_HEADER_SIZE < payload_len:
                self._problem(f, f"record {i}: payload_len {payload_len} exceeds frame")
                return
            r = Record(i, schema_hash=h)
            r.payload = bytes(body[pos + F.FULL_RECORD_HEADER_SIZE:pos + F.FULL_RECORD_HEADER_SIZE + payload_len])
            if layout != 0:
                self._problem(f, f"record {i}: non-native layout {layout}")
            arg = k.args[i] if k is not None and i < len(k.args) else None
            # without the signature a literal is recognised by its id being in the literal table, a string by the
            # "string" hash; scalars need the signature
            literal = (arg.is_literal if arg is not None else
                       payload_len == 8 and d.find_type(h) is None and d.find_literal(h) is not None)
            r.ak = (arg.ak if arg is not None else F.ARG_LITERAL if literal else
                    F.ARG_STRING if h == F.STRING_HASH else F.ARG_TYPE)
            if r.ak == F.ARG_CONST:  # a full-records frame carries the constant as the record it stands for
                r.is_const = True
                r.ak = F.ARG_SCALAR if F.kind_size(arg.kind) else F.ARG_TYPE
                if arg.hash == h and (payload_len > F.CONST_VALUE_SIZE or
                                      r.payload != arg.value.to_bytes(8, "little")[:payload_len]):
                    self._problem(f, f"record {i}: constant differs from the call's value {hex64(arg.value)}")
            if arg is not None and arg.hash != h:
                self._problem(f, f"record {i}: schema {hex64(h)} != call arg {hex64(arg.hash)}")
            if r.ak == F.ARG_SCALAR:
                r.kind = arg.kind
                if payload_len != d.scalar_size(r.kind):
                    self._problem(f, f"record {i}: scalar payload_len {payload_len} != size of "
                                     f"{F.kind_name(r.kind)}")
            elif r.ak == F.ARG_STRING:
                inner = F.U32.unpack_from(r.payload, 0)[0] if payload_len >= F.STRING_LEN_PREFIX else 0
                if payload_len < F.STRING_LEN_PREFIX or inner != payload_len - F.STRING_LEN_PREFIX:
                    self._problem(f, f"record {i}: string length prefix {inner} does not match payload_len "
                                     f"{payload_len}")
                r.payload = r.payload[F.STRING_LEN_PREFIX:] if payload_len >= F.STRING_LEN_PREFIX else b""
            elif literal:
                r.ak = F.ARG_LITERAL
                if payload_len != 8:
                    self._problem(f, f"record {i}: literal payload_len {payload_len} != 8")
                else:
                    r.literal_id = F.U64.unpack_from(r.payload, 0)[0]
                    if arg is not None and r.literal_id != arg.hash:
                        self._problem(f, f"record {i}: literal id mismatch: header {hex64(h)}, payload "
                                         f"{hex64(r.literal_id)}, call arg {hex64(arg.hash)}")
            else:
                r.type = d.find_type(h)
                if r.type is None:
                    self._problem(f, f"record {i}: unknown type {hex64(h)}")
                elif r.type.is_flat and r.type.size != payload_len:
                    self._problem(f, f"record {i}: payload_len {payload_len} != size of {r.type.name} "
                                     f"({r.type.size})")
                elif not r.type.is_flat:  # u32 len, u32 0, image
                    inner = F.U32.unpack_from(r.payload, 0)[0] if payload_len >= F.IMAGE_PREFIX else 0
                    if payload_len < F.IMAGE_PREFIX or inner != payload_len - F.IMAGE_PREFIX:
                        self._problem(f, f"record {i}: image length prefix {inner} does not match payload_len "
                                         f"{payload_len}")
                    r.payload = r.payload[F.IMAGE_PREFIX:] if payload_len >= F.IMAGE_PREFIX else b""
            pos = F.align8(pos + F.FULL_RECORD_HEADER_SIZE + payload_len)
            self._emit(f, r)
        if pos < len(body) and len(body) - pos >= 8:
            self._problem(f, f"{len(body) - pos} trailing bytes after last record")

    def _decode_records(self, f: Frame) -> None:
        dec = Decoder(self.dictionary)
        for r in f.records:
            where = f"frame {f.index} @{f.offset}: record {r.index}: "
            if r.ak == F.ARG_SCALAR:
                if r.kind == F.K_BOOL and len(r.payload) == 1 and r.payload[0] > 1:
                    self.warnings.append(where + f"bool byte 0x{r.payload[0]:02x} read as true")
                    r.value = True
                else:
                    r.value = scalar_value(r.kind, r.payload)
            elif r.ak == F.ARG_STRING:
                r.value = Str(r.payload)
            elif r.is_literal:
                lit = self.dictionary.find_literal(r.literal_id)
                r.value = Lit(r.literal_id, lit.text if lit else None)
            elif r.type is None:
                r.value = None
            else:
                try:
                    r.value = dec.record(r.payload, r.type)
                except DecodeError as e:
                    r.error = str(e)
                    self._add(where + "decode error: " + r.error)
                for w in dec.warnings:
                    self.warnings.append(where + w)
                dec.warnings.clear()

    # -- verify
    def verify(self) -> "VerifyReport":
        """Walks everything and reports what srdump --verify reports."""
        for _ in self.frames():
            pass
        return VerifyReport(self.error, self.frame_count, self.record_count, self.byte_count,
                            self.dictionary.program_hash, list(self.problems), self.problems_omitted,
                            list(self.warnings))


@dataclass
class VerifyReport:
    error: str | None
    frames: int
    records: int
    bytes: int
    program_hash: int
    problems: list[str]
    problems_omitted: int
    warnings: list[str]

    @property
    def ok(self) -> bool:
        return self.error is None and not self.problems and not self.problems_omitted

    def text(self) -> str:
        out = [f"problem: {p}" for p in self.problems]
        if self.problems_omitted:
            out.append(f"problem: ... and {self.problems_omitted} more")
        out += [f"warning: {w}" for w in self.warnings]
        head = "ok: " if self.ok else f"FAILED ({len(self.problems) + self.problems_omitted} problems): "
        out.append(f"{head}{self.frames} frames, {self.records} records, {self.bytes} bytes")
        return "\n".join(out) + "\n"

    def to_json_obj(self) -> dict:
        o: dict = {"ok": self.ok, "frames": self.frames, "records": self.records, "bytes": self.bytes,
                   "problems": list(self.problems)}
        if self.problems_omitted:
            o["problems_omitted"] = self.problems_omitted
        if self.warnings:
            o["warnings"] = list(self.warnings)
        return o


# ---- JSON rendering (srdump --json conventions) ----------------------------------------------------------------------
class RawJSON(str):
    """A number already spelled as srdump spells it (shortest floats); json_dumps writes it verbatim."""


def _fits_double_exactly(v: int) -> bool:
    return -(1 << 53) <= v <= (1 << 53)


def leaf_kind(f: Field | None) -> int:
    """The scalar kind a plain Python value under descriptor `f` has (an optional's value is its element's)."""
    while f is not None and f.kind == F.K_OPTIONAL and f.elem:
        f = f.elem[0]
    return f.kind if f is not None else 0


def scalar_json(k: int, v: object) -> object:
    """A scalar as srdump's JSON value: i64/u64 as decimal strings (JSON numbers are doubles in most parsers),
    floats in their shortest form, chars and bytes as numbers, a wrong-sized payload as "<hex bytes>"."""
    if isinstance(v, (bytes, bytearray)):
        return "<" + " ".join(f"{b:02x}" for b in v) + ">"
    if isinstance(v, bool):
        return v
    if k in (F.K_I64, F.K_U64):
        return str(v)
    if isinstance(v, float):
        if v != v or abs(v) == math.inf:
            return None
        return RawJSON(float_text(v, single=k == F.K_F32))
    return v


def value_json(v: object, f: Field | None = None) -> object:
    """A decoded value as a JSON-ready object: structs as objects, arrays/vectors/sets as arrays (an array of
    chars as a string), maps as [{"key":..,"value":..}], enums as {"name":..,"value":..}, variants as
    {"alt":i,"type":..,"value":..}, strings as text, null for an empty optional or a valueless variant."""
    if v is Null:
        return None
    if isinstance(v, (bool, int, float, bytes)):
        return scalar_json(leaf_kind(f), v)
    if isinstance(v, Struct):
        descs = v.descs + [None] * (len(v.fields) - len(v.descs))
        return {name: value_json(x, fd) for (name, x), fd in zip(v.fields, descs)}
    if isinstance(v, Transparent):
        return value_json(v.value, v.desc)
    if isinstance(v, Array):
        if v.chars is not None:
            return decode_text(v.chars)
        return [value_json(x, v.elem) for x in v.items]
    if isinstance(v, Set):
        return [value_json(x, v.elem) for x in v.items]
    if isinstance(v, Map):
        return [{"key": value_json(a, v.key), "value": value_json(b, v.val)} for a, b in v.entries]
    if isinstance(v, Str):
        return v.text
    if isinstance(v, Enum):
        return {"name": v.name, "value": v.value if _fits_double_exactly(v.value) else str(v.value)}
    if isinstance(v, Variant):
        return {"alt": v.index, "type": v.alt_text[v.alt_text.find(":") + 1:-1], "value": value_json(v.value, v.alt)}
    if isinstance(v, Lit):
        return {"lit": v.text, "id": hex64(v.id)}
    return str(v)


def record_json(f: Frame, r: Record, with_hex: bool = False) -> dict:
    """{"frame":N,"record":M,"type":T,"value":V} as srdump --json prints one record; "type" is the scalar kind
    name, "string", "literal", the type's name, or null (with "schema" and "hex") for an unknown type."""
    o: dict = {"frame": f.index, "record": r.index}
    if r.ak == F.ARG_SCALAR:
        o["type"] = F.kind_name(r.kind)
        o["value"] = scalar_json(r.kind, r.value)
    elif r.ak == F.ARG_STRING:
        o["type"] = "string"
        o["value"] = r.value.text
    elif r.is_literal:
        o["type"] = "literal"
        o["value"] = value_json(r.value)
    elif r.type is None:
        o["type"] = None
        o["schema"] = hex64(r.schema_hash)
    elif r.error:
        o["type"] = r.type.name
        o["error"] = r.error
    else:
        o["type"] = r.type.name
        o["value"] = value_json(r.value)
    if with_hex or (r.ak == F.ARG_TYPE and r.type is None):
        o["hex"] = r.payload.hex()
    return o


def json_dumps(o: object) -> str:
    """Compact JSON with srdump's string escaping (control characters as \\u00XX, UTF-8 passed through)."""
    if isinstance(o, RawJSON):
        return str(o)
    if o is None:
        return "null"
    if o is True:
        return "true"
    if o is False:
        return "false"
    if isinstance(o, int):
        return str(o)
    if isinstance(o, float):
        return "null" if o != o or abs(o) == math.inf else float_text(o)
    if isinstance(o, str):
        return _quote_json(o)
    if isinstance(o, dict):
        return "{" + ",".join(_quote_json(str(k)) + ":" + json_dumps(v) for k, v in o.items()) + "}"
    if isinstance(o, (list, tuple)):
        return "[" + ",".join(json_dumps(v) for v in o) + "]"
    return _quote_json(str(o))


_JSON_ESCAPES = {'"': '\\"', "\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}


def _quote_json(s: str) -> str:
    out = ['"']
    for ch in s:
        e = _JSON_ESCAPES.get(ch)
        if e is not None:
            out.append(e)
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\u{ord(ch):04x}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


# ---- text rendering (srdump --text's inline form; also what print/log messages use for struct arguments) ---------
def quote_chars(raw: bytes) -> str:
    """Text-mode quoting of char data: printable ASCII as is, NUL as \\0, newline as \\n, others as \\xNN."""
    out = ['"']
    for b in raw:
        if b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        elif b == 0:
            out.append("\\0")
        elif b == 0x0A:
            out.append("\\n")
        elif b < 0x20 or b >= 0x7F:
            out.append(f"\\x{b:02x}")
        else:
            out.append(chr(b))
    out.append('"')
    return "".join(out)


def scalar_text(k: int, v: object) -> str:
    """One scalar in text form: chars quoted, bytes as 0xNN, floats shortest, everything else as a number."""
    if isinstance(v, (bytes, bytearray)):
        return '"<' + " ".join(f"{b:02x}" for b in v) + '>"'
    if isinstance(v, bool):
        return "true" if v else "false"
    if k in F.CHAR_KINDS:
        return quote_chars(bytes([v & 0xFF]))
    if k == F.K_BYTE:
        return f"0x{v:02x}"
    if isinstance(v, float):
        return "null" if v != v or abs(v) == math.inf else float_text(v, single=k == F.K_F32)
    return str(v)


def value_text(v: object, f: Field | None = None) -> str:
    """The one-line text form: Type{a: 1, b: "x"}, [1, 2], {k: v}, {a, b}, Enum::name (1), "str", null."""
    if v is Null:
        return "null"
    if isinstance(v, (bool, int, float, bytes)):
        return scalar_text(leaf_kind(f), v)
    if isinstance(v, Struct):
        descs = v.descs + [None] * (len(v.fields) - len(v.descs))
        return v.type_name + "{" + ", ".join(f"{name}: {value_text(x, fd)}"
                                             for (name, x), fd in zip(v.fields, descs)) + "}"
    if isinstance(v, Transparent):
        return value_text(v.value, v.desc)
    if isinstance(v, Array):
        if v.chars is not None:
            return quote_chars(v.chars)
        return "[" + ", ".join(value_text(x, v.elem) for x in v.items) + "]"
    if isinstance(v, Set):
        return "{" + ", ".join(value_text(x, v.elem) for x in v.items) + "}"
    if isinstance(v, Map):
        return "{" + ", ".join(f"{value_text(a, v.key)}: {value_text(b, v.val)}" for a, b in v.entries) + "}"
    if isinstance(v, Str):
        return quote_chars(v.raw)
    if isinstance(v, Enum):
        tn = v.type_name or "enum"
        return f"{tn}::{v.name} ({v.value})" if v.name else f"{tn}({v.value})"
    if isinstance(v, Variant):
        return v.alt_text + "{value: " + value_text(v.value, v.alt) + "}"
    if isinstance(v, Lit):
        return quote_chars(v.text.encode("utf-8", "surrogateescape")) if v.text else "lit#" + hex64(v.id)[2:]
    return str(v)


def record_text(r: Record) -> str:
    """One record in text form (srdump's `[i]` lines, inline)."""
    if r.ak == F.ARG_SCALAR:
        return f"{F.kind_name(r.kind)} = {scalar_text(r.kind, r.value)}"
    if r.ak == F.ARG_STRING:
        return "string = " + quote_chars(r.payload)
    if r.is_literal:
        return value_text(r.value)
    if r.type is None:
        return f"<unknown type {hex64(r.schema_hash)}> ({len(r.payload)} bytes)"
    if r.error:
        return f"{r.type.name} <decode error: {r.error}>"
    return value_text(r.value)


# ---- print and log frames (std::format subset) -----------------------------------------------------------------------
class FormatError(Exception):
    """A replacement field std::format would reject (or one this subset does not implement)."""


_SPEC = re.compile(r"^(?:(?P<fill>.)?(?P<align>[<>^]))?(?P<sign>[-+ ])?(?P<alt>#)?(?P<zero>0)?(?P<width>\d+)?"
                   r"(?:\.(?P<prec>\d+))?(?P<loc>L)?(?P<type>[a-zA-Z])?$", re.S)


def _pad(body: str, fill: str, align: str, width: int, zero: bool, prefix_len: int) -> str:
    if len(body) >= width:
        return body
    if zero and not align:
        return body[:prefix_len] + "0" * (width - len(body)) + body[prefix_len:]
    n = width - len(body)
    if align == "<":
        return body + fill * n
    if align == "^":
        return fill * (n // 2) + body + fill * (n - n // 2)
    return fill * n + body


def cxx_format(spec: str, kind: str, v) -> str:
    """std::format's `{:spec}` for one argument. `kind` is one of "int", "uint", "float32", "float64", "bool",
    "char", "str". Supported: fill/align, sign, #, 0, width, precision, and the types d x X o b B c (integers),
    f F e E g G a A (floating point), s (strings and bool). Nested {} fields and L are not."""
    m = _SPEC.match(spec)
    if not m or m.group("loc"):
        raise FormatError(spec)
    fill = m.group("fill") or " "
    align = m.group("align") or ""
    sign = m.group("sign") or "-"
    alt = bool(m.group("alt"))
    zero = bool(m.group("zero"))
    width = int(m.group("width") or 0)
    prec = m.group("prec")
    typ = m.group("type") or ""
    if kind == "bool":
        if typ in ("", "s"):
            kind, v = "str", "true" if v else "false"
        else:
            kind, v = "int", int(v)
    if kind == "char":
        if typ in ("", "c"):
            kind, v, typ = "str", chr(v & 0xFF) if v >= 0 else chr(v & 0xFF), ""
        else:
            kind = "int"
    if kind == "str":
        if typ not in ("", "s") or sign != "-" or alt or zero or (fill == "" and align):
            raise FormatError(spec)
        s = v if prec is None else v[:int(prec)]
        return _pad(s, fill, align or "<", width, False, 0)
    if kind in ("int", "uint"):
        if prec is not None:
            raise FormatError(spec)
        if typ == "c":
            return _pad(chr(v), fill, align or "<", width, False, 0)
        neg = v < 0
        a = -v if neg else v
        if typ in ("", "d"):
            digits, prefix = str(a), ""
        elif typ in ("x", "X"):
            digits, prefix = format(a, "x"), "0x" if alt else ""
        elif typ in ("b", "B"):
            digits, prefix = format(a, "b"), "0b" if alt else ""
        elif typ == "o":
            digits, prefix = format(a, "o"), ("0" if alt and a != 0 else "")
        else:
            raise FormatError(spec)
        if typ in ("X", "B"):
            digits, prefix = digits.upper(), prefix.upper()
        sgn = "-" if neg else "+" if sign == "+" else " " if sign == " " else ""
        body = sgn + prefix + digits
        return _pad(body, fill, align, width, zero, len(sgn) + len(prefix))
    # floating point
    single = kind == "float32"
    if typ == "" and prec is None:
        if v != v or abs(v) == math.inf:
            digits = "nan" if v != v else "inf"
        else:
            digits = float_text(abs(v), single)
            if alt and "." not in digits and "e" not in digits:
                digits += "."
    elif typ in ("a", "A"):
        digits = _hexfloat(abs(v), int(prec) if prec is not None else None)
        if typ == "A":
            digits = digits.upper()
    elif typ in ("f", "F", "e", "E", "g", "G", ""):
        if v != v or abs(v) == math.inf:
            digits = "nan" if v != v else "inf"
            if typ in ("F", "E", "G"):
                digits = digits.upper()
        else:
            p = int(prec) if prec is not None else 6
            digits = format(abs(v), ("#" if alt else "") + "." + str(p) + (typ or "g"))
    else:
        raise FormatError(spec)
    neg = math.copysign(1.0, v) < 0 and not (v != v)
    sgn = "-" if neg else "+" if sign == "+" else " " if sign == " " else ""
    return _pad(sgn + digits, fill, align, width, zero, len(sgn))


def _hexfloat(v: float, prec: int | None) -> str:
    if v != v or v == math.inf:
        return "nan" if v != v else "inf"
    if prec is not None:
        # round the mantissa to `prec` hex digits through the exact fraction
        m, e = math.frexp(v)
        if v == 0:
            return "0." + "0" * prec + "p+0" if prec else "0p+0"
        m, e = m * 2, e - 1  # 1 <= m < 2
        scaled = round((m - 1) * (16 ** prec))
        lead = 1
        if scaled >= 16 ** prec:
            scaled, lead = 0, 2
        frac = format(scaled, "x").rjust(prec, "0") if prec else ""
        return f"{lead}{'.' + frac if frac else ''}p{e:+d}"
    s = v.hex()  # 0x1.8000000000000p+1
    if s.startswith("0x"):
        s = s[2:]
    mant, _, exp = s.partition("p")
    if "." in mant:
        mant = mant.rstrip("0").rstrip(".")
    return f"{mant}p{int(exp):+d}"


@dataclass
class LogView:
    level: int
    file: str
    line: int
    mask: int
    time: int
    thread: int | None
    sequence: int | None
    format_index: int

    @property
    def level_name(self) -> str:
        return F.LEVEL_NAMES[self.level] if self.level < 6 else str(self.level)

    def time_text(self) -> str:
        """A system-clock time as YYYY-MM-DD HH:MM:SS.nnnnnnnnn (UTC); any other clock as the number."""
        if (self.mask >> 2) & 3:
            return str(self.time)
        t = _dt.datetime.fromtimestamp(self.time // 1_000_000_000, _dt.timezone.utc)
        return t.strftime("%Y-%m-%d %H:%M:%S") + f".{self.time % 1_000_000_000:09d}"


def decode_log(d: Dictionary, k: Call, records: list[Record]) -> LogView | None:
    """The envelope of a log frame: level, file, line, mask, time, [thread], [sequence], and where the format
    literal is (log.hpp's log_layout)."""
    if not k.is_log or len(records) != len(k.args) or len(records) < 6:
        return None

    def u(r: Record, n: int) -> int:
        return int.from_bytes(r.payload[:n], "little")

    if not records[1].is_literal:
        return None
    lit = d.find_literal(records[1].literal_id)
    mask = u(records[3], 1)
    at = 5
    thread = sequence = None
    if mask & 1:
        if at >= len(records):
            return None
        thread, at = u(records[at], 4), at + 1
    if mask & 2:
        if at >= len(records):
            return None
        sequence, at = u(records[at], 8), at + 1
    if at >= len(records) or not records[at].is_literal:
        return None
    return LogView(u(records[0], 1), lit.text if lit else "", u(records[2], 4), mask, u(records[4], 8), thread,
                   sequence, at)


def _arg_text(d: Dictionary, r: Record) -> str:
    """The text an argument is formatted as when std::format's argument type is a string_view (literal, string,
    enum, struct, unknown type)."""
    if r.ak == F.ARG_STRING:
        return r.value.text
    if r.is_literal:
        lit = d.find_literal(r.literal_id)
        return lit.text if lit else hex64(r.literal_id)
    if r.type is None:
        return f"<{hex64(r.schema_hash)}>"
    if r.error:
        raise FormatError(r.error)
    if r.type.is_enum:
        if len(r.payload) != F.kind_size(r.type.underlying):
            raise FormatError("length_mismatch")
        return r.value.name or str(r.value.value)
    return value_text(r.value)


def _render_arg(d: Dictionary, r: Record, spec: str) -> str:
    if r.ak == F.ARG_SCALAR:
        v = r.value
        if isinstance(v, bytes):
            raise FormatError("length_mismatch")
        if r.kind in F.SIGNED_KINDS:
            return cxx_format(spec, "int", v)
        if r.kind == F.K_F32:
            return cxx_format(spec, "float32", v)
        if r.kind == F.K_F64:
            return cxx_format(spec, "float64", v)
        if r.kind == F.K_BOOL:
            return cxx_format(spec, "bool", v)
        if r.kind == F.K_CHAR:
            return cxx_format(spec, "char", v)
        return cxx_format(spec, "uint", v)
    return cxx_format(spec, "str", _arg_text(d, r))


def render_message(d: Dictionary, k: Call, records: list[Record], envelope: bool = True) -> str:
    """The rendered text of a print/println/log frame: the format literal with its placeholders filled as
    std::format fills them (`{}`, `{n}`, `{:spec}`, `{{`/`}}`), a log frame's envelope first when `envelope`.
    Raises FormatError where srdump reports a render problem."""
    if not k.is_print or not records or len(records) != len(k.args):
        raise FormatError("invalid_argument")
    out = []
    format_at = 0
    if k.is_log:
        v = decode_log(d, k, records)
        if v is None:
            raise FormatError("invalid_argument")
        format_at = v.format_index
        if envelope:
            out.append(log_envelope(v))
    if not records[format_at].is_literal:
        raise FormatError("invalid_argument")
    lit = d.find_literal(records[format_at].literal_id)
    if lit is None:
        raise FormatError("unknown_literal")
    out.append(format_text(d, lit.text, records[format_at + 1:]))
    if k.newline:
        out.append("\n")
    return "".join(out)


def log_envelope(v: LogView) -> str:
    """"<time> <LEVEL> <file>:<line> [thread] #seq " — the prefix of a text-mode log line."""
    lvl = v.level_name.upper()
    out = v.time_text() + " " + lvl + " " * (6 - len(lvl) if len(lvl) < 5 else 1)
    if v.file != "-":
        out += v.file.rsplit("/", 1)[-1] + ":" + str(v.line) + " "
    if v.thread is not None:
        out += f"[{v.thread}] "
    if v.sequence is not None:
        out += f"#{v.sequence} "
    return out


def format_text(d: Dictionary, fmt: str, args: list[Record]) -> str:
    out = []
    i, n = 0, len(fmt)
    nxt = 0
    indexing = None  # "automatic" | "manual"
    while i < n:
        c = fmt[i]
        if c == "}":
            if i + 1 < n and fmt[i + 1] == "}":
                out.append("}")
                i += 2
                continue
            raise FormatError("invalid_argument")  # a lone '}'
        if c != "{":
            out.append(c)
            i += 1
            continue
        if i + 1 < n and fmt[i + 1] == "{":
            out.append("{")
            i += 2
            continue
        j = i + 1
        idx_s = ""
        while j < n and fmt[j].isdigit():
            idx_s += fmt[j]
            j += 1
        spec = ""
        if j < n and fmt[j] == ":":
            e = j + 1
            while e < n and fmt[e] not in "{}":
                e += 1
            if e >= n or fmt[e] == "{":
                raise FormatError("invalid_argument")  # nested {} width/precision fields are not supported
            spec = fmt[j + 1:e]
            j = e
        if j >= n or fmt[j] != "}":
            raise FormatError("invalid_argument")
        has_idx = idx_s != ""
        if (has_idx and indexing == "automatic") or (not has_idx and indexing == "manual"):
            raise FormatError("invalid_argument")  # std::format forbids mixing
        indexing = "manual" if has_idx else "automatic"
        if has_idx:
            idx = int(idx_s)
            if idx > 0xFFFF:
                raise FormatError("invalid_argument")
        else:
            idx, nxt = nxt, nxt + 1
        if idx >= len(args):
            raise FormatError("length_mismatch")
        out.append(_render_arg(d, args[idx], spec))
        i = j + 1
    return "".join(out)


# ---- the output streams of srdump --json / --text, frame by frame ----------------------------------------------------
LEVEL_BY_NAME = {name: i for i, name in enumerate(F.LEVEL_NAMES)}


def json_lines(reader: Reader, min_level: int | None = None, with_hex: bool = False) -> Iterator[dict]:
    """Yields what srdump --json prints after the dictionary, one object per line: records
    {"frame","record","type","value"}, print messages {"frame","message"}, log frames {"frame","log":{...}},
    then {"problem": ...} lines. With `min_level`, only log frames at or above it (srdump --level)."""
    for f in reader.frames():
        k = f.call
        if min_level is not None:
            if k is None or not k.is_log or not k.args or k.args[0].value < min_level:
                continue
        is_print = k is not None and k.is_print
        if not (is_print and k.is_log):
            for r in f.records:
                yield record_json(f, r, with_hex)
        if is_print:
            try:
                msg = render_message(reader.dictionary, k, f.records, envelope=False)
            except FormatError as e:
                reader._add(f"frame {f.index} @{f.offset}: print: {e}")
                continue
            if k.is_log:
                v = decode_log(reader.dictionary, k, f.records)
                o: dict = {"level": v.level_name, "time": v.time, "file": v.file, "line": v.line}
                if v.thread is not None:
                    o["thread"] = v.thread
                if v.sequence is not None:
                    o["sequence"] = v.sequence
                o["message"] = msg[:-1] if msg.endswith("\n") else msg
                yield {"frame": f.index, "log": o}
            else:
                yield {"frame": f.index, "message": msg}
    for p in reader.problems:
        yield {"problem": p}
    if reader.problems_omitted:
        yield {"problems_omitted": reader.problems_omitted}


def text_lines(reader: Reader, min_level: int | None = None) -> Iterator[str]:
    """A readable dump: one header line per frame, one line per record (inline form), rendered messages."""
    for f in reader.frames():
        k = f.call
        if min_level is not None:
            if k is None or not k.is_log or not k.args or k.args[0].value < min_level:
                continue
            try:
                yield render_message(reader.dictionary, k, f.records).rstrip("\n")
            except FormatError as e:
                reader._add(f"frame {f.index} @{f.offset}: print: {e}")
            continue
        yield (f"frame {f.index}  {f.form} {hex64(f.call_hash)}  ({f.n_records} records"
               f"{', full' if f.is_full else ''})")
        if k is not None and k.is_print:
            try:
                msg = render_message(reader.dictionary, k, f.records)
                yield ("  log: " if k.is_log else "  print: ") + msg.rstrip("\n")
            except FormatError as e:
                reader._add(f"frame {f.index} @{f.offset}: print: {e}")
            continue
        for r in f.records:
            yield f"  [{r.index}] {record_text(r)}"
    for p in reader.problems:
        yield f"problem: {p}"
    if reader.problems_omitted:
        yield f"problem: ... and {reader.problems_omitted} more"


# ---- 1.7: the file's header, dictionary chain, checkpoints and writer info, by offsets (tools::info) -----------------
@dataclass
class Checkpoint:
    offset: int
    full_dictionary: bool
    prev: int
    next: int
    data_frames_before: int
    bytes_before: int
    wall_clock_ns: int
    dictionary_offset: int
    registry_version: int


@dataclass
class CaptureInfo:
    file_size: int = 0
    minor: int = 0
    flags: int = 0
    program_hash: int = 0
    abi: int = 0
    dictionary: Dictionary = dc_field(default_factory=Dictionary)
    dictionary_offsets: list[int] = dc_field(default_factory=list)
    checkpoints: list[Checkpoint] = dc_field(default_factory=list)
    writer: list[tuple[int, str]] = dc_field(default_factory=list)
    warnings: list[str] = dc_field(default_factory=list)  # chain links that could not be followed (same text as srdump)

    def writer_item(self, key: int) -> str:
        for k, v in self.writer:
            if k == key:
                return v
        return ""

    def text(self) -> str:
        flags = "".join(name for bit, name in ((F.FILE_FLAG_CRC_ALL, " crc"), (F.FILE_FLAG_CHECKPOINTS,
                                                                                  " checkpoints"),
                                                 (F.FILE_FLAG_INFO, " info"), (F.FILE_FLAG_BACKPATCHED,
                                                                                " back-patched"))
                        if self.flags & bit)
        d = self.dictionary
        out = [f"format 1.{self.minor}  program_hash {hex64(self.program_hash)}  size {self.file_size}  flags{flags}",
               f"dictionary  {len(d.types)} types, {len(d.literals)} literals, {len(d.calls)} calls; "
               f"{len(self.dictionary_offsets)} block(s) at" + "".join(f" {o}" for o in self.dictionary_offsets),
               f"checkpoints {len(self.checkpoints)}"]
        for c in self.checkpoints:
            out.append(f"  @{c.offset}  frames {c.data_frames_before}  bytes {c.bytes_before}  dictionary "
                       f"@{c.dictionary_offset}{' (full copy)' if c.full_dictionary else ''}  wall {c.wall_clock_ns}")
        if not self.writer:
            out.append("writer      none")
        for k, v in self.writer:
            out.append(f"writer      {F.INFO_KEYS.get(k, f'key{k}')} = {v}")
        out += [f"warning     {w}" for w in self.warnings]
        return "\n".join(out) + "\n"

    def to_json_obj(self) -> dict:
        d = self.dictionary
        return {"format": f"1.{self.minor}", "program_hash": hex64(self.program_hash), "size": self.file_size,
                "crc": bool(self.flags & F.FILE_FLAG_CRC_ALL), "dictionary_offsets": list(self.dictionary_offsets),
                "types": len(d.types), "literals": len(d.literals), "calls": len(d.calls),
                "checkpoints": [{"offset": c.offset, "frames": c.data_frames_before, "bytes": c.bytes_before,
                                 "dictionary": c.dictionary_offset, "wall_ns": c.wall_clock_ns}
                                for c in self.checkpoints],
                "writer": {str(k): v for k, v in self.writer},
                "warnings": list(self.warnings)}


def _frame_at(data, off: int) -> tuple[Frame, memoryview] | str:
    n = len(data)
    if off + F.FRAME_HEADER_SIZE > n:
        return "truncated"
    fc, f = classify_frame(data, off, min(n - off, F.FRAME_BYTES_LIMIT))
    if fc != "ok":
        return "length_mismatch"
    whole = memoryview(data)[off:off + f.frame_len]
    if f.has_crc and not check_crc(whole):
        return "crc_mismatch"
    return f, whole


def capture_info(data) -> CaptureInfo | str:
    """tools::info over a FILE held in memory; an error name when the header or the dictionary is unreadable."""
    ci = CaptureInfo(file_size=len(data))
    fh = parse_file_header(data)
    if isinstance(fh, str):
        return fh
    ci.minor, ci.flags, ci.program_hash, ci.abi = fh.minor, fh.flags, fh.program_hash, fh.abi
    ci.dictionary.program_hash = fh.program_hash
    ci.dictionary.abi, ci.dictionary.abi_known = fh.abi, True
    if not fh.flags & F.FILE_FLAG_DICT or fh.dict_len == 0:
        return ci
    if fh.dict_offset > len(data) or fh.dict_len > len(data) - fh.dict_offset:
        return "truncated"
    err = ci.dictionary.add_rri(memoryview(data)[fh.dict_offset:fh.dict_offset + fh.dict_len])
    if err:
        return err
    ci.dictionary_offsets.append(fh.dict_offset)
    if fh.minor < 7:
        return ci  # 1.6: no chains, no writer info
    chain = F.align8(fh.dict_offset + fh.dict_len)
    if chain + F.CHAIN_SIZE > len(data):
        return "truncated"
    next_dict, next_ckpt = struct.unpack_from("<QQ", data, chain)
    if fh.flags & F.FILE_FLAG_INFO:  # the writer-info frame, right after the CHAIN
        r = _frame_at(data, chain + F.CHAIN_SIZE)
        if isinstance(r, str):
            return r
        f, b = r
        if f.is_info:
            end = len(b) - (F.CRC_TRAILER_SIZE if f.has_crc else 0)
            at = F.FRAME_HEADER_SIZE
            n_items = 0
            if end - at >= 4:
                _ver, n_items = struct.unpack_from("<HH", b, at)
                at += 4
            for _ in range(n_items):
                if end - at < 4:
                    break
                key, ln = struct.unpack_from("<HH", b, at)
                at += 4
                if end - at < ln:
                    break
                ci.writer.append((key, decode_text(bytes(b[at:at + ln]))))
                at += ln + (ln % 2)
    def dangling(which: str, frm: int, to: int, prev: int) -> None:
        # a link that cannot be followed is a warning (the chain ends there; verify reports the rest)
        w = f"{which} chain: link @{frm} -> {to}"
        if to + F.FRAME_HEADER_SIZE > ci.file_size:
            w += f" points past the end of the file ({ci.file_size} bytes)"
        elif to <= prev:
            w += " does not go forward"
        else:
            w += f" is not a {which} frame"
        ci.warnings.append(w)

    last = chain  # the dictionary chain: offsets must increase (a loop in a corrupt file ends the chain)
    link_at = chain
    if next_dict != 0 and next_dict <= last:
        dangling("dictionary", link_at, next_dict, last)
    steps = 0
    while next_dict != 0 and next_dict > last and steps < F.DICTIONARY_ENTRIES_LIMIT:
        steps += 1
        r = _frame_at(data, next_dict)
        if isinstance(r, str) or not r[0].is_dict:
            dangling("dictionary", link_at, next_dict, last)
            break
        f, b = r
        end = len(b) - (F.CRC_TRAILER_SIZE if f.has_crc else 0)
        if end < F.FRAME_HEADER_SIZE + F.DICT_FRAME_LINK_SIZE:
            break
        delta = Dictionary()
        if delta.add_rri(b[F.FRAME_HEADER_SIZE:end - F.DICT_FRAME_LINK_SIZE]):
            break
        ci.dictionary.merge(delta)
        ci.dictionary_offsets.append(next_dict)
        last = next_dict
        link_at = next_dict + end - F.DICT_FRAME_LINK_SIZE
        next_dict = F.U64.unpack_from(b, end - F.DICT_FRAME_LINK_SIZE)[0]
        if next_dict != 0 and next_dict <= last:
            dangling("dictionary", link_at, next_dict, last)
    last = chain  # the checkpoint chain
    link_at = chain + 8
    if next_ckpt != 0 and next_ckpt <= last:
        dangling("checkpoint", link_at, next_ckpt, last)
    steps = 0
    while next_ckpt != 0 and next_ckpt > last and steps < F.DICTIONARY_ENTRIES_LIMIT:
        steps += 1
        r = _frame_at(data, next_ckpt)
        if isinstance(r, str) or not r[0].is_checkpoint or len(r[1]) < F.FRAME_HEADER_SIZE + F.CHECKPOINT_FIXED_SIZE:
            dangling("checkpoint", link_at, next_ckpt, last)
            break
        _ver, flags, _rri_len, prev, nxt, frames, nbytes, wall, dict_off, regver = F.CHECKPOINT_FIXED.unpack_from(
            r[1], F.FRAME_HEADER_SIZE)
        ci.checkpoints.append(Checkpoint(next_ckpt, bool(flags & F.CHECKPOINT_FLAG_FULL_DICTIONARY), prev, nxt,
                                         frames, nbytes, wall, dict_off, regver))
        link_at = next_ckpt + F.FRAME_HEADER_SIZE + 16
        if nxt != 0 and nxt <= next_ckpt:
            dangling("checkpoint", link_at, nxt, next_ckpt)
        last = next_ckpt
        next_ckpt = nxt
    return ci
