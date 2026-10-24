#!/usr/bin/env python3
"""Join compatible .mkpk clips frame-by-frame into one looping sequence.

Usage: tools/masko_sequence.py OUTPUT.mkpk FIRST.mkpk SECOND.mkpk [MORE.mkpk ...]
Each input is played once, in order; the combined pack loops at the shared FPS.
"""

from __future__ import annotations

import argparse
import binascii
import struct
from pathlib import Path

ALIGN = 512
HEADER_SIZE = 64


def read_header(data: bytes) -> dict[str, int]:
    if len(data) < HEADER_SIZE or data[:4] != b"MKPK":
        raise ValueError("input is not an MKPK pack")
    version, header_size = struct.unpack_from("<HH", data, 4)
    if version != 1 or header_size != HEADER_SIZE:
        raise ValueError("only MKPK v1 packs are supported")
    values = struct.unpack_from("<8H", data, 12)
    (canvas_w, canvas_h, bbox_x, bbox_y, bbox_w, bbox_h,
     fps_num, fps_den) = values
    frame_count, index_offset, data_offset, file_size = struct.unpack_from("<4I", data, 28)
    bg_rgb565 = struct.unpack_from("<H", data, 44)[0]
    return {
        "codec": data[8], "flags": data[9], "subsampling": data[10],
        "canvas_w": canvas_w, "canvas_h": canvas_h,
        "bbox_x": bbox_x, "bbox_y": bbox_y,
        "bbox_w": bbox_w, "bbox_h": bbox_h,
        "fps_num": fps_num, "fps_den": fps_den,
        "frame_count": frame_count, "index_offset": index_offset,
        "data_offset": data_offset, "file_size": file_size,
        "bg_rgb565": bg_rgb565,
    }


def frames(data: bytes, h: dict[str, int]) -> list[bytes]:
    result = []
    for i in range(h["frame_count"]):
        off, size = struct.unpack_from("<II", data, h["index_offset"] + i * 8)
        result.append(data[off:off + size])
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("inputs", type=Path, nargs="+", help="packs in playback order")
    args = ap.parse_args()
    if len(args.inputs) < 2:
        ap.error("provide at least two input packs")

    sources = [(path, path.read_bytes()) for path in args.inputs]
    headers = [read_header(blob) for _, blob in sources]
    first = headers[0]
    common = ("codec", "subsampling", "canvas_w", "canvas_h", "bbox_x", "bbox_y",
              "bbox_w", "bbox_h", "fps_num", "fps_den", "bg_rgb565")
    for path, h in zip(args.inputs[1:], headers[1:]):
        if any(h[k] != first[k] for k in common):
            raise SystemExit(f"incompatible pack geometry/timing/codec: {path}")
    if first["codec"] != 1 or any((h["flags"] & 0x02) for h in headers):
        raise SystemExit("only opaque baseline-JPEG packs are supported")

    all_frames = [frame for blob, h in zip((b for _, b in sources), headers)
                  for frame in frames(blob, h)]
    count = len(all_frames)
    index_offset = HEADER_SIZE
    data_offset = (HEADER_SIZE + count * 8 + ALIGN - 1) // ALIGN * ALIGN
    index = bytearray()
    payload = bytearray()
    for frame in all_frames:
        payload.extend(bytes((-(data_offset + len(payload))) % ALIGN))
        offset = data_offset + len(payload)
        index.extend(struct.pack("<II", offset, len(frame)))
        payload.extend(frame)

    file_size = data_offset + len(payload)
    gap = bytes(data_offset - (HEADER_SIZE + len(index)))
    payload_crc = binascii.crc32(index + gap + payload) & 0xFFFFFFFF
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sHH", header, 0, b"MKPK", 1, HEADER_SIZE)
    header[8] = first["codec"]
    header[9] = 1  # loop the full ordered comparison sequence
    header[10] = first["subsampling"]
    struct.pack_into("<8H", header, 12,
                     first["canvas_w"], first["canvas_h"], first["bbox_x"],
                     first["bbox_y"], first["bbox_w"], first["bbox_h"],
                     first["fps_num"], first["fps_den"])
    struct.pack_into("<4I", header, 28, count, index_offset, data_offset, file_size)
    struct.pack_into("<HH", header, 44, first["bg_rgb565"], 0)
    struct.pack_into("<I", header, 48, payload_crc)
    header_crc = binascii.crc32(header[:52]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 52, header_crc)

    args.output.write_bytes(header + index + gap + payload)
    print(f"wrote {args.output}: {count} frames @ {first['fps_num']}/{first['fps_den']} fps, "
          f"{file_size:,} bytes")
    for path, h in zip(args.inputs, headers):
        print(f"  {path}: {h['frame_count']} frames, {h['file_size']:,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
