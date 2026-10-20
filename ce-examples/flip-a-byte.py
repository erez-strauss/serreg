#!/usr/bin/env python3
# flip-a-byte.py IN.ser OUT.ser — copies a capture and flips one bit of the first data frame's frame_len (48 -> 56),
# so `srdump --verify OUT.ser` reports the damage and exits 1 (slide 10 / slide 44 of the talk: never crash on wrong
# data). Standard library only. Layout (format.hpp): FILE header (dict_offset @24, dict_len @32) | RRI | pad to 8 |
# CHAIN (16 B) | writer-info frame | data frames; every frame starts with its u32 frame_len.
import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
at = (struct.unpack_from("<Q", b, 24)[0] + struct.unpack_from("<Q", b, 32)[0] + 7) // 8 * 8 + 16  # past the CHAIN
while b[at + 12:at + 16] == b"RSSE": at += struct.unpack_from("<I", b, at)[0]  # skip "ESSRINFO"/"ESSRCKPT"/"ESSRDICT" frames (LE)
b[at] ^= 0x08; open(sys.argv[2], "wb").write(b); print(f"flipped bit 3 of the frame_len at offset {at}: {b[at] ^ 8} -> {b[at]}")
