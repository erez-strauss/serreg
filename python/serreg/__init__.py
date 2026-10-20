"""serreg — a stand-alone, pure-Python reader for es::serreg binary captures (wire format 1.6 and 1.7).

    import serreg
    with open("orders.ser", "rb") as fh:
        r = serreg.Reader(fh.read())
    for frame in r.frames():
        for rec in frame.records:
            print(frame.index, rec.index, rec.type_label, serreg.value_json(rec.value))
    print(r.problems)

The modules: `format` (constants and byte layouts), `dictionary` (RRI/ENTRY parsing), `reader` (frames, records,
the value decoder, JSON/text renderings, print/log messages, the 1.7 chains). `python3 -m serreg --help` is the
command line (mirrors srdump's main modes). Standard library only, Python 3.10 or later.
"""
from .dictionary import Call, CallArg, Dictionary, Field, Literal, Type, load_rri
from .reader import (Array, CaptureInfo, Decoder, DecodeError, Enum, FormatError, Frame, Lit, LogView, Map,
                     Null, Reader, Record, Set, Str, Struct, Transparent, Variant, VerifyReport, capture_info,
                     cxx_format, decode_log, json_dumps, json_lines, record_json, record_text, render_message,
                     text_lines, value_json, value_text)

__version__ = "2.2.6"
__all__ = ["Array", "Call", "CallArg", "CaptureInfo", "Decoder", "DecodeError", "Dictionary", "Enum", "Field",
           "FormatError", "Frame", "Lit", "Literal", "LogView", "Map", "Null", "Reader", "Record", "Set", "Str",
           "Struct", "Transparent", "Type", "Variant", "VerifyReport", "capture_info", "cxx_format", "decode_log",
           "json_dumps", "json_lines", "load_rri", "record_json", "record_text", "render_message", "text_lines",
           "value_json", "value_text"]
