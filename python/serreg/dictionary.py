"""serreg.dictionary — ENTRY and RRI parsing into Python objects (mirrors src/dictionary.cpp).

A Dictionary holds the types (structs and enums, by schema hash), the literals (by id) and the call signatures
(by call hash) of a capture. It is built from an RRI image (`add_rri`), a bare ENTRY stream (`add_entries`) or
other dictionaries (`merge`). The first definition of a hash wins; a later, different body is recorded as a
conflict, as the C++ reader does. Nothing here raises on malformed input: parsers return an error name.
"""
from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from typing import Iterator

from . import format as F
from .format import Cursor, Short, scalar_value

MAX_FIELD_DEPTH = 32
# minimum on-wire bytes of each repeated element: a count must fit the remaining bytes before anything is built
MIN_FIELD_BYTES, MIN_ENUMERATOR_BYTES, ARG_BYTES, MIN_ENTRY_BYTES = 32, 10, 16, 8
MAX_CONFLICTS = 64


class CorruptEntry(Exception):
    """An ENTRY body that the rules of src/dictionary.cpp reject."""


@dataclass
class Field:
    """One FIELD descriptor: a member of a struct, or an element descriptor of an array/container/variant."""
    name: str = ""
    kind: int = 0
    offset: int = 0
    size: int = 0
    align: int = 0
    since: int = 0
    deprecated: int = 0
    type_hash: int = 0  # struct_/enum_: the nested type's schema hash
    extent: int = 0  # array: element count; variant: alternatives; map/set: the entry stride
    elem: list["Field"] = dc_field(default_factory=list)


@dataclass
class Type:
    name: str = ""
    schema_hash: int = 0
    layout_hash: int = 0
    size: int = 0
    align: int = 0
    is_enum: bool = False
    underlying: int = 0  # enum: the underlying scalar kind
    is_flat: bool = False
    transparent: bool = False  # 1.6: a container argument's synthetic type (one unnamed field at 0)
    enum_fixed: bool = False  # 1.6: the enum has a fixed underlying type
    since: int = 0
    deprecated: int = 0
    fields: list[Field] = dc_field(default_factory=list)
    enumerators: list[tuple[str, int]] = dc_field(default_factory=list)

    def enumerator_name(self, value: int) -> str:
        for name, v in self.enumerators:
            if v == value:
                return name
        return ""


@dataclass
class Literal:
    id: int = 0
    text: str = ""
    raw: bytes = b""


@dataclass
class CallArg:
    ak: int = F.ARG_TYPE  # one of F.ARG_*
    kind: int = 0  # scalar / constant: the scalar kind (0 for an enum constant)
    hash: int = 0  # schema hash | literal id | scalar name hash | "string" hash
    value: int = 0  # constant: the object's bytes as a u64 (zero-extended)

    @property
    def is_literal(self) -> bool:
        return self.ak == F.ARG_LITERAL

    @property
    def is_const(self) -> bool:
        return self.ak == F.ARG_CONST


@dataclass
class Call:
    hash: int = 0
    flags: int = 0
    args: list[CallArg] = dc_field(default_factory=list)

    @property
    def is_print(self) -> bool:
        return bool(self.flags & F.CALL_FLAG_PRINT)

    @property
    def newline(self) -> bool:
        return bool(self.flags & F.CALL_FLAG_NEWLINE)

    @property
    def is_log(self) -> bool:
        return bool(self.flags & F.CALL_FLAG_LOG)

    @property
    def form(self) -> str:
        return "log" if self.is_log else "println" if self.newline else "print" if self.is_print else "call"


@dataclass
class Conflict:
    what: str  # "schema", "literal" or "call"
    id: int
    kept: str
    dropped: str


def hex64(v: int) -> str:
    return f"0x{v & 0xFFFFFFFFFFFFFFFF:016x}"


# ---- ENTRY parsing ---------------------------------------------------------------------------------------------
def _read_name(c: Cursor, n: int) -> str:
    return c.bytes(n).decode("utf-8", "replace")


def _count_fits(c: Cursor, count: int, min_bytes: int) -> bool:
    return count <= c.remaining() // min_bytes


def parse_field(c: Cursor, depth: int = 0) -> Field:
    """FIELD: u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated,
    u16 n_elem, u64 type_hash, u32 extent, name bytes, FIELD x n_elem."""
    if depth > MAX_FIELD_DEPTH:
        raise CorruptEntry("field nesting")
    f = Field()
    name_len = c.u16()
    f.kind = c.u8()
    c.u8()
    f.offset = c.u32()
    f.size = c.u32()
    f.align = c.u16()
    f.since = c.u16()
    f.deprecated = c.u16()
    n_elem = c.u16()
    f.type_hash = c.u64()
    f.extent = c.u32()
    f.name = _read_name(c, name_len)
    if not _count_fits(c, n_elem, MIN_FIELD_BYTES):
        raise CorruptEntry("n_elem")
    f.elem = [parse_field(c, depth + 1) for _ in range(n_elem)]
    k = f.kind
    if k == F.K_ARRAY:
        if len(f.elem) != 1:
            raise CorruptEntry("array elements")
        e = f.elem[0]
        if (e.size == 0 and f.extent > 0) or e.size * f.extent > f.size:
            raise CorruptEntry("array extent")
    elif k in F.SLOT_KINDS:
        if f.size != 8:
            raise CorruptEntry("slot size")
        want = 0 if k == F.K_STRING else f.extent if k == F.K_VARIANT else 2 if k == F.K_MAP else 1
        if len(f.elem) != want or (k == F.K_VARIANT and f.extent == 0):
            raise CorruptEntry("element descriptors")
        for e in f.elem:
            if e.size == 0 and e.kind != F.K_STRUCT:
                raise CorruptEntry("empty element")
        if k in (F.K_MAP, F.K_SET):
            key = f.elem[0]
            end = max(key.size, f.elem[1].offset + f.elem[1].size) if k == F.K_MAP else key.size
            al = key.align or 1
            if k == F.K_MAP and f.elem[1].align > al:
                al = f.elem[1].align
            if (f.extent == 0 or f.extent < end or f.extent % al != 0 or key.offset != 0
                    or (k == F.K_MAP and f.elem[1].offset < key.size)):
                raise CorruptEntry("entry stride")
    elif (k == F.K_LITERAL or k > F.K_SET or F.K_STRUCT < k < F.K_STRING or F.K_BYTE < k < F.K_ENUM
          or k == 0):
        raise CorruptEntry("unknown kind")
    if F.is_scalar_kind(k) and f.size != F.kind_size(k) and not (k == F.K_WCHAR and f.size in (2, 4)):
        raise CorruptEntry("scalar size")
    if k == F.K_ENUM and f.size not in (1, 2, 4, 8):
        raise CorruptEntry("enum size")
    if f.align > 4096 or (f.align & (f.align - 1)) != 0:
        raise CorruptEntry("alignment")
    return f


def parse_type(c: Cursor, is_enum: bool) -> Type:
    """The body of a struct (tag 1) or enum (tag 2) ENTRY."""
    t = Type(is_enum=is_enum)
    t.schema_hash = c.u64()
    t.layout_hash = c.u64()
    t.size = c.u32()
    t.align = c.u32()
    flags_or_underlying = c.u8()
    rsv = c.u8()
    t.since = c.u16()
    t.deprecated = c.u16()
    name_len = c.u16()
    n = c.u16()
    t.name = _read_name(c, name_len)
    if is_enum:
        t.underlying = flags_or_underlying
        t.is_flat = True
        if rsv & ~F.ENUM_FLAG_FIXED:
            raise CorruptEntry("enum flags")
        t.enum_fixed = bool(rsv & F.ENUM_FLAG_FIXED)
        if not _count_fits(c, n, MIN_ENUMERATOR_BYTES):
            raise CorruptEntry("n_enumerators")
        for _ in range(n):
            value = c.i64()
            name = _read_name(c, c.u16())
            t.enumerators.append((name, value))
    else:
        if flags_or_underlying & ~(F.STRUCT_FLAG_FLAT | F.STRUCT_FLAG_TRANSPARENT):
            raise CorruptEntry("struct flags")
        t.is_flat = bool(flags_or_underlying & F.STRUCT_FLAG_FLAT)
        t.transparent = bool(flags_or_underlying & F.STRUCT_FLAG_TRANSPARENT)
        if t.transparent and (t.is_flat or n != 1):
            raise CorruptEntry("transparent type")
        if not _count_fits(c, n, MIN_FIELD_BYTES):
            raise CorruptEntry("n_fields")
        for _ in range(n):
            f = parse_field(c)
            if f.offset + f.size > t.size:
                raise CorruptEntry("field outside type")
            t.fields.append(f)
        if t.transparent:
            f = t.fields[0]
            if f.name or f.offset != 0 or f.kind not in (F.K_VECTOR, F.K_OPTIONAL, F.K_VARIANT, F.K_MAP,
                                                        F.K_SET) or t.size != 8:
                raise CorruptEntry("transparent field")
    return t


def parse_literal(c: Cursor) -> Literal:
    """literal ENTRY body: u64 id, u32 len, u32 rsv, bytes."""
    lit = Literal()
    lit.id = c.u64()
    n = c.u32()
    c.u32()
    lit.raw = c.bytes(n)
    lit.text = decode_text(lit.raw)
    return lit


def parse_call(c: Cursor) -> Call:
    """call ENTRY body: u64 call_hash, u16 n_args, u16 flags, u32 rsv, ARG x n (a constant's ARG + u64 value)."""
    k = Call()
    k.hash = c.u64()
    n = c.u16()
    k.flags = c.u16()
    c.u32()
    if k.flags & ~(F.CALL_FLAG_PRINT | F.CALL_FLAG_NEWLINE | F.CALL_FLAG_LOG):
        raise CorruptEntry("call flags")
    if k.is_log and (k.flags & (F.CALL_FLAG_PRINT | F.CALL_FLAG_NEWLINE)) != (F.CALL_FLAG_PRINT
                                                                             | F.CALL_FLAG_NEWLINE):
        raise CorruptEntry("a log call is a println")
    if not _count_fits(c, n, ARG_BYTES):
        raise CorruptEntry("n_args")
    for _ in range(n):
        a = CallArg()
        a.ak = c.u8()
        kb = c.u8()
        c.skip(6)
        a.hash = c.u64()
        if a.ak > F.ARG_CONST:
            raise CorruptEntry("arg kind")
        if a.ak == F.ARG_SCALAR:
            a.kind = kb
            if F.kind_size(kb) == 0:
                raise CorruptEntry("scalar kind")
        elif a.ak == F.ARG_CONST:
            a.kind = kb
            if kb != 0 and F.kind_size(kb) == 0:
                raise CorruptEntry("constant kind")
            a.value = c.u64()
            sz = F.kind_size(kb)
            if 0 < sz < 8 and (a.value >> (8 * sz)) != 0:
                raise CorruptEntry("constant value")
            if kb == F.K_BOOL and a.value > 1:
                raise CorruptEntry("constant bool")
        elif kb != 0:
            raise CorruptEntry("arg kind byte")
        k.args.append(a)
    if k.is_log:  # level(const u8), file(literal), line(const u32), mask(const u8), time(u64), [thread u32],
        # [sequence u64], format(literal), then the format arguments (log.hpp's log_layout)
        if len(k.args) < 6:
            raise CorruptEntry("log arity")

        def is_const(i: int, want: int) -> bool:
            return k.args[i].ak == F.ARG_CONST and k.args[i].kind == want

        if (not is_const(0, F.K_U8) or k.args[1].ak != F.ARG_LITERAL or not is_const(2, F.K_U32)
                or not is_const(3, F.K_U8) or k.args[3].value > 15 or k.args[4].ak != F.ARG_SCALAR
                or k.args[4].kind != F.K_U64):
            raise CorruptEntry("log envelope")
        at = 5
        if k.args[3].value & 1:
            if at >= len(k.args) or k.args[at].ak != F.ARG_SCALAR or k.args[at].kind != F.K_U32:
                raise CorruptEntry("log thread")
            at += 1
        if k.args[3].value & 2:
            if at >= len(k.args) or k.args[at].ak != F.ARG_SCALAR or k.args[at].kind != F.K_U64:
                raise CorruptEntry("log sequence")
            at += 1
        if at >= len(k.args) or k.args[at].ak != F.ARG_LITERAL:
            raise CorruptEntry("log format")
    elif k.is_print and (not k.args or k.args[0].ak != F.ARG_LITERAL):
        raise CorruptEntry("print format")
    return k


def decode_text(raw: bytes) -> str:
    """Bytes as text the way srdump's JSON does it: well-formed UTF-8 passes, any other byte is the code point
    of its own value (U+0080..U+00FF), so nothing is lost and the result is always valid text."""
    s = raw.decode("utf-8", "surrogateescape")
    if any(0xDC80 <= ord(ch) <= 0xDCFF for ch in s):
        s = "".join(chr(ord(ch) - 0xDC00) if 0xDC80 <= ord(ch) <= 0xDCFF else ch for ch in s)
    return s


# ---- the dictionary --------------------------------------------------------------------------------------------
class Dictionary:
    def __init__(self) -> None:
        self.types: dict[int, Type] = {}
        self.literals: dict[int, Literal] = {}
        self.calls: dict[int, Call] = {}
        self.program_hash = 0
        self.abi = 0
        self.abi_known = False
        self.abi_mixed = False
        self.conflicts: list[Conflict] = []
        self.conflicts_omitted = 0

    # -- building
    def add_entries(self, entries) -> str | None:
        """Parses a bare ENTRY stream (an RRI body, an ELF section). Returns an error name or None."""
        c = Cursor(entries)
        try:
            while c.remaining() > 0:
                total_len = c.u32()
                if total_len == 0:  # trailing zero padding (section alignment) is tolerated
                    return None if not any(entries[c.pos:c.end]) else "corrupt_dictionary"
                if total_len < F.ENTRY_HEADER_SIZE or total_len % 8 != 0 or total_len - 4 > c.remaining():
                    return "corrupt_dictionary"
                tag = c.u8()
                version = c.u8()
                c.u16()
                body_start = c.pos
                c.skip(total_len - F.ENTRY_HEADER_SIZE)
                if version != 1:
                    continue  # unknown entry version: skipped (forward compatibility)
                b = Cursor(entries, body_start, c.pos)
                if tag in (F.TAG_STRUCT, F.TAG_ENUM):
                    self._add_type(parse_type(b, tag == F.TAG_ENUM))
                elif tag == F.TAG_LITERAL:
                    self._add_literal(parse_literal(b))
                elif tag == F.TAG_CALL:
                    self._add_call(parse_call(b))
                # other tags are skipped
        except (Short, CorruptEntry):
            return "corrupt_dictionary"
        return None

    def add_rri(self, rri) -> str | None:
        """Parses one RRI image (header + n_entries entries); trailing bytes are ignored. Error name or None."""
        if len(rri) < F.RRI_HEADER_SIZE:
            return "truncated"
        magic, major, minor, _flags, hash_algo, n_entries, prog, _rsv, abi = F.RRI_HEADER.unpack_from(rri, 0)
        if magic != F.RRI_MAGIC:
            return "bad_magic"
        if major != F.FORMAT_MAJOR or not F.FORMAT_MINOR_OLDEST <= minor <= F.FORMAT_MINOR:
            return "bad_version"
        if hash_algo != F.HASH_ALGO_XXH3_64:
            return "unsupported"
        if n_entries > F.DICTIONARY_ENTRIES_LIMIT:
            return "corrupt_dictionary"
        c = Cursor(rri, F.RRI_HEADER_SIZE)
        if not _count_fits(c, n_entries, MIN_ENTRY_BYTES):
            return "corrupt_dictionary"
        begin = c.pos
        try:
            for _ in range(n_entries):
                total_len = c.u32()
                if total_len < F.ENTRY_HEADER_SIZE or total_len % 8 != 0:
                    return "corrupt_dictionary"
                c.skip(total_len - 4)
        except Short:
            return "corrupt_dictionary"
        err = self.add_entries(memoryview(rri)[begin:c.pos])
        if err:
            return err
        if self.program_hash == 0:
            self.program_hash = prog
        if not self.abi_known:
            self.abi, self.abi_known = abi, True
        elif self.abi != abi:
            self.abi_mixed = True
        return None

    def merge(self, other: "Dictionary") -> None:
        for t in other.types.values():
            self._add_type(t)
        for lit in other.literals.values():
            self._add_literal(lit)
        for k in other.calls.values():
            self._add_call(k)
        for cf in other.conflicts:
            if self._room():
                self.conflicts.append(cf)
        self.conflicts_omitted += other.conflicts_omitted
        if self.program_hash == 0:
            self.program_hash = other.program_hash
        if other.abi_known:
            if not self.abi_known:
                self.abi, self.abi_known = other.abi, True
            elif self.abi != other.abi:
                self.abi_mixed = True
        self.abi_mixed = self.abi_mixed or other.abi_mixed

    def _room(self) -> bool:
        if len(self.conflicts) < MAX_CONFLICTS:
            return True
        self.conflicts_omitted += 1
        return False

    def _add_type(self, t: Type) -> None:
        old = self.types.get(t.schema_hash)
        if old is None:
            self.types[t.schema_hash] = t
        elif old != t and self._room():
            self.conflicts.append(Conflict("schema", t.schema_hash,
                                           f"{old.name} {hex64(t.schema_hash)} (layout {hex64(old.layout_hash)}, "
                                           f"size {old.size})",
                                           f"{t.name} (layout {hex64(t.layout_hash)}, size {t.size})"))

    def _add_literal(self, lit: Literal) -> None:
        old = self.literals.get(lit.id)
        if old is None:
            self.literals[lit.id] = lit
        elif old.raw != lit.raw and self._room():
            self.conflicts.append(Conflict("literal", lit.id, old.text[:256], lit.text[:256]))

    def _add_call(self, k: Call) -> None:
        old = self.calls.get(k.hash)
        if old is None:
            self.calls[k.hash] = k
        elif old != k and self._room():
            self.conflicts.append(Conflict("call", k.hash, _call_text(old), _call_text(k)))

    # -- lookups
    def find_type(self, schema_hash: int) -> Type | None:
        return self.types.get(schema_hash)

    def find_type_by_name(self, name: str) -> Type | None:
        for t in self.types.values():
            if t.name == name:
                return t
        return None

    def find_literal(self, lit_id: int) -> Literal | None:
        return self.literals.get(lit_id)

    def find_call(self, call_hash: int) -> Call | None:
        return self.calls.get(call_hash)

    def scalar_size(self, k: int) -> int:
        """A top-level scalar's size as the writer laid it out: `wchar` comes from the writer's ABI tag."""
        return F.AbiTag.unpack(self.abi).wchar_size if k == F.K_WCHAR else F.kind_size(k)

    def problems(self) -> list[str]:
        """What the merged dictionary itself is wrong about (reported by verify and dump alike)."""
        out = []
        for c in self.conflicts:
            if c.what == "schema":
                out.append(f"schema conflict: {c.kept} vs {c.dropped}")
            else:
                out.append(f'{c.what} conflict: {hex64(c.id)} has two bodies: "{c.kept}" vs "{c.dropped}"')
        if self.conflicts_omitted:
            out.append(f"{self.conflicts_omitted} more conflicts not listed")
        if self.abi_mixed:
            out.append("abi: dictionaries of different writer ABIs in one capture")
        return out

    # -- renderings
    def sorted_types(self) -> Iterator[Type]:
        return (self.types[h] for h in sorted(self.types))

    def to_json_obj(self) -> dict:
        """The dictionary as srdump --json prints it (keys sorted, arrays in hash order); constants carry
        their typed value and their exact bits."""
        calls = []
        for h in sorted(self.calls):
            k = self.calls[h]
            args = []
            for a in k.args:
                o: dict = {"hash": hex64(a.hash), "kind": F.ARG_NAMES[a.ak]}
                if a.ak == F.ARG_SCALAR or (a.ak == F.ARG_CONST and F.kind_size(a.kind) != 0):
                    o["scalar"] = F.kind_name(a.kind)
                if a.ak == F.ARG_CONST:
                    o["value"] = self.const_value(a)
                    o["bits"] = hex64(a.value)
                args.append(o)
            c: dict = {"args": args}
            if k.flags:
                c["form"] = k.form
            c["hash"] = hex64(h)
            calls.append(c)
        lits = [{"id": hex64(i), "text": self.literals[i].text} for i in sorted(self.literals)]
        types = []
        for t in self.sorted_types():
            o = {"align": t.align, "deprecated": t.deprecated}
            if t.is_enum:
                o["enumerators"] = [{"name": n, "value": v} for n, v in t.enumerators]
                o["fixed"] = t.enum_fixed
            else:
                o["fields"] = [_field_json(f) for f in t.fields]
            o.update({"is_enum": t.is_enum, "is_flat": t.is_flat, "layout_hash": hex64(t.layout_hash),
                      "name": t.name, "schema_hash": hex64(t.schema_hash), "since": t.since, "size": t.size})
            if t.transparent:
                o["transparent"] = True
            if t.is_enum:
                o["underlying"] = F.kind_name(t.underlying)
            types.append(o)
        return {"calls": calls, "literals": lits, "program_hash": hex64(self.program_hash), "types": types}

    def const_value(self, a: CallArg):
        """A constant as the value it stands for: int, bool, float, a character or an enumerator name (str)."""
        sz = F.kind_size(a.kind)
        if sz:
            v = scalar_value(a.kind, a.value.to_bytes(8, "little")[:sz])
            if a.kind in (F.K_F32, F.K_F64):
                return v if isinstance(v, float) and v == v and abs(v) != float("inf") else str(v)
            if a.kind == F.K_BYTE:
                return f"0x{a.value & 0xFF:02x}"
            if a.kind in F.CHAR_KINDS:
                return chr(a.value & 0xFF)
            if a.kind in (F.K_CHAR16, F.K_CHAR32, F.K_WCHAR):
                return str(v)
            return v
        t = self.find_type(a.hash)
        if t and t.is_enum:
            bits = 8 * F.kind_size(t.underlying)
            for name, value in t.enumerators:
                if value & 0xFFFFFFFFFFFFFFFF == a.value or (value < 0 and bits < 64 and
                                                            (value & ((1 << bits) - 1)) == a.value):
                    return name
        return str(a.value)

    def to_text(self) -> str:
        """A readable listing: one line per type, enumerator, field, literal and call."""
        out = [f"dictionary  program_hash {hex64(self.program_hash)}  ({len(self.types)} types, "
               f"{len(self.literals)} literals, {len(self.calls)} calls)"]
        for t in self.sorted_types():
            line = ("enum " if t.is_enum else "struct ") + t.name
            if t.is_enum:
                line += " : " + F.kind_name(t.underlying)
            line += (f"  schema {hex64(t.schema_hash)}  layout {hex64(t.layout_hash)}  size {t.size}"
                     f"  align {t.align}")
            if not t.is_enum:
                line += "  flat" if t.is_flat else "  dynamic"
            if t.transparent:
                line += "  transparent"
            if t.is_enum and not t.enum_fixed:
                line += "  unfixed"
            out.append(line)
            for name, value in t.enumerators:
                out.append(f"  {name} = {value}")
            for f in t.fields:
                out.append(f"  {f.name}: {self.kind_text(f)} @{f.offset} [{f.size},{f.align}]")
        for i in sorted(self.literals):
            out.append(f"literal {hex64(i)} {self.literals[i].text!r}")
        for h in sorted(self.calls):
            k = self.calls[h]
            out.append(f"{k.form} {hex64(h)}  ({', '.join(self.arg_text(a) for a in k.args)})")
        return "\n".join(out) + "\n"

    def arg_text(self, a: CallArg) -> str:
        if a.is_literal:
            return "lit"
        if a.ak == F.ARG_CONST:
            t = None if F.kind_size(a.kind) else self.find_type(a.hash)
            name = F.kind_name(a.kind) if F.kind_size(a.kind) else t.name if t else hex64(a.hash)
            return f"const {name}={hex64(a.value)}"
        if a.ak == F.ARG_SCALAR:
            return F.kind_name(a.kind)
        if a.ak == F.ARG_STRING:
            return "string"
        t = self.find_type(a.hash)
        return t.name if t else hex64(a.hash)

    def kind_text(self, f: Field) -> str:
        k = f.kind
        if k == F.K_ARRAY:
            return f"array<{self.kind_text(f.elem[0]) if f.elem else '?'},{f.extent}>"
        if k in (F.K_VECTOR, F.K_OPTIONAL, F.K_SET):
            return f"{F.kind_name(k)}<{self.kind_text(f.elem[0]) if f.elem else '?'}>"
        if k == F.K_VARIANT:
            return "variant<" + "|".join(self.kind_text(e) for e in f.elem) + ">"
        if k == F.K_MAP:
            inner = ",".join(self.kind_text(e) for e in f.elem) if len(f.elem) == 2 else "?"
            return f"map<{inner}>"
        if k in (F.K_STRUCT, F.K_ENUM):
            t = self.find_type(f.type_hash)
            return f"{F.kind_name(k)}<{t.name if t else hex64(f.type_hash)}>"
        return F.kind_name(k)


def _field_json(f: Field) -> dict:
    return {"align": f.align, "deprecated": f.deprecated, "elem": [_field_json(e) for e in f.elem],
            "extent": f.extent, "kind": F.kind_name(f.kind), "name": f.name, "offset": f.offset,
            "since": f.since, "size": f.size, "type_hash": hex64(f.type_hash)}


def _call_text(k: Call) -> str:
    return f"flags {k.flags} (" + ", ".join(f"{a.ak}:{hex64(a.hash)}" for a in k.args) + ")"


def load_rri(path: str) -> tuple[Dictionary, str | None]:
    """Reads an .rri file into a new Dictionary; returns (dictionary, error name or None)."""
    d = Dictionary()
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return d, "io_error"
    return d, d.add_rri(data)
