#!/usr/bin/env python3
"""Bundle two compatible .mkpk clips for the Waveshare touch-switch demo."""
import argparse
import pathlib

ALIGNMENT = 512


def dimensions(pack: bytes) -> tuple[int, int]:
    if len(pack) < 64 or pack[:4] != b"MKPK":
        raise ValueError("input is not an MKPK pack")
    # Header fields are little-endian; bbox width/height are u16 at 20/22.
    return int.from_bytes(pack[20:22], "little"), int.from_bytes(pack[22:24], "little")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("packs", type=pathlib.Path, nargs=2, metavar="PACK")
    args = parser.parse_args()
    blobs = [path.read_bytes() for path in args.packs]
    size = dimensions(blobs[0])
    if any(dimensions(blob) != size for blob in blobs[1:]):
        raise SystemExit("all packs must have matching bbox dimensions")
    output = bytearray(blobs[0])
    for blob in blobs[1:]:
        output.extend(b"\xff" * ((-len(output)) % ALIGNMENT))
        output.extend(blob)
    args.output.write_bytes(output)
    print(f"wrote {len(blobs)} clips ({len(output)} bytes) to {args.output}")


if __name__ == "__main__":
    main()
