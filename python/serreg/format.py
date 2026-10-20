"""serreg.format — the es::serreg wire format, byte for byte (include/es/serreg/format.hpp, descriptor.hpp).

Every integer is little-endian and fixed width. "u16 name_len, name bytes" means a length prefix immediately
followed by the bytes, no terminator. Offsets and sizes below are in bytes.

ENTRY (one blob; the unit stored in the ELF section, in an RRI and in dictionary frames)
    u32 total_len        whole entry including this header, a multiple of 8 (zero padded)
    u8  tag              1 struct, 2 enum, 3 literal, 4 call signature
    u8  version          1
    u16 reserved         0
    body by tag:
    [1] struct   u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 flags (bit0 flat, bit1 transparent),
                 u8 rsv, u16 since, u16 deprecated, u16 name_len, u16 n_fields, name, FIELD x n_fields
    [2] enum     u64 schema_hash, u64 layout_hash, u32 size, u32 align, u8 underlying_kind, u8 flags (bit0 fixed),
                 u16 since, u16 deprecated, u16 name_len, u16 n_enumerators, name,
                 ENUMERATOR x n where ENUMERATOR = i64 value, u16 name_len, name
    [3] literal  u64 id, u32 len, u32 rsv, text bytes
    [4] call     u64 call_hash, u16 n_args, u16 flags (bit0 print, bit1 newline, bit2 log), u32 rsv, ARG x n
                 ARG = u8 arg_kind (0 type, 1 literal, 2 scalar, 3 string, 4 constant), u8 kind, u8 rsv[6],
                 u64 hash; a constant's ARG is followed by u64 value (the object's bytes, zero-extended)
    FIELD        u16 name_len, u8 kind, u8 rsv, u32 offset, u32 size, u16 align, u16 since, u16 deprecated,
                 u16 n_elem, u64 type_hash, u32 extent, name, FIELD x n_elem (element descriptors, recursive:
                 an array has one, a vector/optional/set one, a map two — key at 0, value at its offset — and a
                 variant `extent` alternatives; a map's/set's `extent` is the entry stride)

RRI (Raw Registry Image = header + ENTRY stream)
    char[4] "ESRG", u16 major (1), u16 minor (6 or 7), u32 flags (bit0 delta), u32 hash_algo (2 = XXH3-64),
    u32 n_entries, u64 program_hash, u32 reserved, u64 abi_tag                      -> 40 bytes, then ENTRY x n

FILE
    char[4] "ESSR", u16 major (1), u16 minor, u32 flags (bit0 LE, bit1 dictionary present, bit2 back-patched —
    1.6 only, bit3 crc_all, bit4 checkpoints, bit5 writer info — 1.7), u32 hash_algo (2), u64 program_hash,
    u64 dict_offset, u64 dict_len, u64 abi_tag                                       -> 48 bytes
    then the opening dictionary at dict_offset (an RRI of dict_len bytes), padding to 8, and in 1.7 the CHAIN
    (u64 next_dictionary, u64 first_checkpoint; 0 until patched), then FRAME x N. A 1.6 file has no CHAIN; its
    dictionary may have been rewritten at the end (flag bit2): the frames then run from after the original
    dictionary area to dict_offset.

FRAME (16-byte header)
    u32 frame_len (whole frame, header included, a multiple of 8), u16 flags (bit0 full records, bit1 dictionary
    frame, bit2 CRC32C trailer, bit3 checkpoint frame, bit4 writer-info frame), u16 n_records, u64 call_hash
    compact records   each record starts 8-aligned; flat type -> its bytes (size from the dictionary);
                      literal and constant -> nothing; scalar -> its bytes; string -> u32 len, bytes;
                      dynamic type -> u32 len, u32 0, canonical image
    full records      u64 hash, u32 payload_len, u16 layout (0 = native), u16 rsv, payload, pad to 8
    dictionary frame  call_hash = DICT_SENTINEL, n_records 0, payload = one RRI (full or delta), pad to 8, and
                      when the RRI's minor is >= 7: u64 next_dictionary (the forward chain; 0 = none yet)
    CRC32C trailer    (flag bit2) the last 8 bytes are u32 crc32c(frame[0 : frame_len - 8]), u32 0; a frame
                      whose CRC does not match is reported and skipped, never decoded
    checkpoint frame  (flag bit3) call_hash = CHECKPOINT_SENTINEL, payload = u16 version (1), u16 flags (bit0 a
                      full RRI follows), u32 rri_len, u64 prev_checkpoint, u64 next_checkpoint, u64
                      data_frames_before, u64 bytes_before, u64 wall_clock_ns, u64 latest_dictionary_offset,
                      u64 registry_version, [RRI]
    writer-info frame (flag bit4) call_hash = INFO_SENTINEL, payload = u16 version (1), u16 n_items, then
                      ITEM x n where ITEM = u16 key, u16 len, bytes, pad to 2

Canonical image of a dynamic type (format 1.2+, book/08-dynamic-members.md)
    The record is `u32 len, u32 0` and then `len` bytes: the type's fixed part (its `size` bytes, members at
    their offsets) followed by tail blocks. A dynamic member's fixed part is a slot {u32 off, u32 n}: a string's
    bytes, a vector's n elements at their stride, a map's/set's n entries of `extent` bytes, an optional's one
    value (n = 1) or nothing (n = 0), a variant's alternative n - 1 (n = 0: valueless). Rules both C++ decoders
    apply: n == 0 <=> off == 0; a non-empty block starts at or after the root's fixed part and lies inside the
    image; all blocks together are at most image-size bytes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Final

FILE_MAGIC: Final = b"ESSR"
RRI_MAGIC: Final = b"ESRG"
FORMAT_MAJOR: Final = 1
FORMAT_MINOR: Final = 7  # what writers stamp (library 2.2.1)
FORMAT_MINOR_OLDEST: Final = 6  # the oldest minor readers accept
HASH_ALGO_XXH3_64: Final = 2

DICT_SENTINEL: Final = 0x4553535244494354  # "ESSRDICT"
CHECKPOINT_SENTINEL: Final = 0x45535352434B5054  # "ESSRCKPT"
INFO_SENTINEL: Final = 0x45535352494E464F  # "ESSRINFO"
STRING_HASH: Final = 0xD8AD194648E774F1  # fingerprint64("string"): the hash of a runtime string argument

FILE_HEADER_SIZE: Final = 48
RRI_HEADER_SIZE: Final = 40
FRAME_HEADER_SIZE: Final = 16
ENTRY_HEADER_SIZE: Final = 8
FULL_RECORD_HEADER_SIZE: Final = 16
CONST_VALUE_SIZE: Final = 8
STRING_LEN_PREFIX: Final = 4
IMAGE_PREFIX: Final = 8
CRC_TRAILER_SIZE: Final = 8
CHAIN_SIZE: Final = 16
DICT_FRAME_LINK_SIZE: Final = 8
CHECKPOINT_FIXED_SIZE: Final = 2 + 2 + 4 + 8 * 7
SLOT_SIZE: Final = 8
MAX_NESTING: Final = 64  # struct levels; the dictionary walker counts two units per level

# entry tags
TAG_STRUCT, TAG_ENUM, TAG_LITERAL, TAG_CALL = 1, 2, 3, 4
# argument kinds of a call signature
ARG_TYPE, ARG_LITERAL, ARG_SCALAR, ARG_STRING, ARG_CONST = 0, 1, 2, 3, 4
ARG_NAMES: Final = {ARG_TYPE: "type", ARG_LITERAL: "literal", ARG_SCALAR: "scalar", ARG_STRING: "string",
                    ARG_CONST: "const"}
# call flags
CALL_FLAG_PRINT, CALL_FLAG_NEWLINE, CALL_FLAG_LOG = 1, 2, 4
# FILE flags
FILE_FLAG_LE, FILE_FLAG_DICT, FILE_FLAG_BACKPATCHED = 1, 2, 4
FILE_FLAG_CRC_ALL, FILE_FLAG_CHECKPOINTS, FILE_FLAG_INFO = 8, 16, 32
# frame flags
FRAME_FLAG_FULL, FRAME_FLAG_DICT, FRAME_FLAG_CRC, FRAME_FLAG_CHECKPOINT, FRAME_FLAG_INFO = 1, 2, 4, 8, 16
FRAME_FLAGS_KNOWN: Final = (FRAME_FLAG_FULL | FRAME_FLAG_DICT | FRAME_FLAG_CRC | FRAME_FLAG_CHECKPOINT
                            | FRAME_FLAG_INFO)
RRI_FLAG_DELTA: Final = 1
STRUCT_FLAG_FLAT, STRUCT_FLAG_TRANSPARENT = 1, 2
ENUM_FLAG_FIXED: Final = 1
CHECKPOINT_FLAG_FULL_DICTIONARY: Final = 1

# the limits the C++ readers apply by default (es::serreg::decode_limits)
FRAME_BYTES_LIMIT: Final = 64 << 20
DICTIONARY_BYTES_LIMIT: Final = 256 << 20
DICTIONARY_ENTRIES_LIMIT: Final = 1_000_000
PROBLEMS_LIMIT: Final = 1000

# writer-info keys (values are UTF-8 text)
INFO_KEYS: Final = {1: "library", 2: "format", 3: "program", 4: "host", 5: "pid", 6: "start_wall_ns",
                    7: "scrub_padding", 8: "full_records", 9: "canonical_unordered", 10: "crc",
                    11: "checkpoint_bytes", 12: "checkpoint_ns", 13: "dictionary", 14: "sync", 15: "clock",
                    16: "thread", 17: "abi"}
INFO_CLOCK, INFO_THREAD = 15, 16

# log levels (a log call's first constant)
LEVEL_NAMES: Final = ("trace", "debug", "info", "warn", "error", "fatal")

# ---- kinds (descriptor.hpp) ------------------------------------------------------------------------------------
K_I8, K_I16, K_I32, K_I64, K_U8, K_U16, K_U32, K_U64 = 1, 2, 3, 4, 5, 6, 7, 8
K_F32, K_F64, K_BOOL, K_CHAR, K_CHAR8, K_CHAR16, K_CHAR32, K_WCHAR, K_BYTE = 9, 10, 11, 12, 13, 14, 15, 16, 17
K_ENUM, K_ARRAY, K_STRUCT = 32, 33, 34
K_STRING, K_VECTOR, K_OPTIONAL, K_VARIANT, K_LITERAL, K_MAP, K_SET = 48, 49, 50, 51, 52, 53, 54

KIND_NAMES: Final = {
    K_I8: "i8", K_I16: "i16", K_I32: "i32", K_I64: "i64", K_U8: "u8", K_U16: "u16", K_U32: "u32", K_U64: "u64",
    K_F32: "f32", K_F64: "f64", K_BOOL: "bool", K_CHAR: "char", K_CHAR8: "char8", K_CHAR16: "char16",
    K_CHAR32: "char32", K_WCHAR: "wchar", K_BYTE: "byte", K_ENUM: "enum", K_ARRAY: "array", K_STRUCT: "struct",
    K_STRING: "string", K_VECTOR: "vector", K_OPTIONAL: "optional", K_VARIANT: "variant", K_LITERAL: "literal",
    K_MAP: "map", K_SET: "set"}
KIND_SIZES: Final = {K_I8: 1, K_U8: 1, K_BOOL: 1, K_CHAR: 1, K_CHAR8: 1, K_BYTE: 1, K_I16: 2, K_U16: 2,
                     K_CHAR16: 2, K_I32: 4, K_U32: 4, K_F32: 4, K_CHAR32: 4, K_WCHAR: 4, K_I64: 8, K_U64: 8,
                     K_F64: 8}
SIGNED_KINDS: Final = frozenset((K_I8, K_I16, K_I32, K_I64))
CHAR_KINDS: Final = frozenset((K_CHAR, K_CHAR8))  # rendered as characters, and char arrays as strings
SLOT_KINDS: Final = frozenset((K_STRING, K_VECTOR, K_OPTIONAL, K_VARIANT, K_MAP, K_SET))


def kind_name(k: int) -> str:
    return KIND_NAMES.get(k, "?")


def kind_size(k: int) -> int:
    """Native size of one scalar kind, 0 for everything else."""
    return KIND_SIZES.get(k, 0)


def is_scalar_kind(k: int) -> bool:
    return 0 < k < 32


def align8(n: int) -> int:
    return (n + 7) & ~7


def slot_stride(size: int, align: int) -> int:
    """Element stride of a block: the element size rounded up to its alignment."""
    a = align if align else 1
    return (size + a - 1) // a * a


# ---- struct layouts (little-endian) ---------------------------------------------------------------------------
FILE_HEADER: Final = struct.Struct("<4sHHIIQQQQ")  # magic, major, minor, flags, algo, prog, dict_off, dict_len, abi
RRI_HEADER: Final = struct.Struct("<4sHHIIIQIQ")  # magic, major, minor, flags, hash_algo, n_entries, prog, rsv, abi
FRAME_HEADER: Final = struct.Struct("<IHHQ")  # frame_len, flags, n_records, call_hash
ENTRY_HEADER: Final = struct.Struct("<IBBH")  # total_len, tag, version, reserved
FULL_RECORD_HEADER: Final = struct.Struct("<QIHH")  # hash, payload_len, layout, rsv
SLOT: Final = struct.Struct("<II")  # off, n
CHECKPOINT_FIXED: Final = struct.Struct("<HHIQQQQQQQ")
U16: Final = struct.Struct("<H")
U32: Final = struct.Struct("<I")
U64: Final = struct.Struct("<Q")

assert FILE_HEADER.size == FILE_HEADER_SIZE
assert RRI_HEADER.size == RRI_HEADER_SIZE
assert FRAME_HEADER.size == FRAME_HEADER_SIZE
assert CHECKPOINT_FIXED.size == CHECKPOINT_FIXED_SIZE


@dataclass(frozen=True)
class AbiTag:
    """The writer's C++ ABI facts, packed into the u64 at FILE +40 and RRI +32 (byte 0 = version)."""
    version: int
    endian: int  # 0 little, 1 big
    ptr_size: int
    long_size: int
    wchar_size: int
    char_signed: int
    max_align: int
    long_double_size: int

    @staticmethod
    def unpack(v: int) -> "AbiTag":
        b = v.to_bytes(8, "little")
        return AbiTag(*b)

    def text(self) -> str:
        if self.version != 1:
            return f"unknown(version {self.version})"
        return (f"ptr{self.ptr_size} long{self.long_size} wchar{self.wchar_size} "
                f"{'char-signed' if self.char_signed else 'char-unsigned'} max-align{self.max_align} "
                f"ldbl{self.long_double_size} {'be' if self.endian else 'le'}")


class Short(Exception):
    """Raised by Cursor when the input ends before a field does; parsers turn it into a problem."""


class Cursor:
    """Bounded little-endian reader over a bytes-like object: every read is checked against the end."""

    __slots__ = ("buf", "pos", "end")

    def __init__(self, buf, start: int = 0, end: int | None = None) -> None:
        self.buf = buf
        self.pos = start
        self.end = len(buf) if end is None else end

    def remaining(self) -> int:
        return self.end - self.pos

    def can(self, n: int) -> bool:
        return self.end - self.pos >= n

    def _take(self, n: int) -> int:
        if self.end - self.pos < n:
            raise Short()
        at = self.pos
        self.pos = at + n
        return at

    def u8(self) -> int:
        return self.buf[self._take(1)]

    def u16(self) -> int:
        return U16.unpack_from(self.buf, self._take(2))[0]

    def u32(self) -> int:
        return U32.unpack_from(self.buf, self._take(4))[0]

    def u64(self) -> int:
        return U64.unpack_from(self.buf, self._take(8))[0]

    def i64(self) -> int:
        return struct.unpack_from("<q", self.buf, self._take(8))[0]

    def bytes(self, n: int) -> bytes:
        at = self._take(n)
        return bytes(self.buf[at:at + n])

    def skip(self, n: int) -> None:
        self._take(n)


# ---- scalars ------------------------------------------------------------------------------------------------------
_SCALAR_FORMATS = {K_I8: "<b", K_I16: "<h", K_I32: "<i", K_I64: "<q", K_U8: "<B", K_U16: "<H",
                   K_U32: "<I", K_U64: "<Q", K_F32: "<f", K_F64: "<d", K_CHAR: "<b", K_CHAR8: "<B",
                   K_BYTE: "<B", K_CHAR16: "<H", K_CHAR32: "<I"}


def scalar_value(k: int, b) -> object:
    """One scalar from its bytes: int / float / bool, or the raw bytes when the size is not the kind's."""
    n = len(b)
    if k == K_BOOL:
        return bool(b[0]) if n == 1 else bytes(b)
    if k == K_WCHAR:
        if n == 2:
            return U16.unpack_from(b)[0]
        if n == 4:
            return U32.unpack_from(b)[0]
        return bytes(b)
    fmt = _SCALAR_FORMATS.get(k)
    if fmt is None or struct.calcsize(fmt) != n:
        return bytes(b)
    return struct.unpack_from(fmt, b)[0]


# ---- CRC-32C (Castagnoli), the 1.7 frame trailer ------------------------------------------------------------------
def _crc32c_table() -> list[int]:
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC32C_TABLE: Final = _crc32c_table()


def crc32c(data, crc: int = 0) -> int:
    """Reflected CRC-32C (polynomial 0x1EDC6F41, reflected 0x82F63B78); crc32c(b"123456789") == 0xE3069283."""
    c = crc ^ 0xFFFFFFFF
    t = _CRC32C_TABLE
    for b in bytes(data):
        c = t[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


assert crc32c(b"123456789") == 0xE3069283
