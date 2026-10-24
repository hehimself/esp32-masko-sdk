#!/usr/bin/env python3
"""Summarize board-side Kylo cold-switch timings from an idf.py monitor log."""

import re
import statistics
import sys
from pathlib import Path

PIXEL = re.compile(
    r"first pixel: (\S+) -> (\S+), preparation-to-display=(\d+) ms, "
    r"input-to-display=(-?\d+) ms"
)


def percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    pos = (len(ordered) - 1) * fraction
    lo = int(pos)
    return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (pos - lo)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} monitor.log", file=sys.stderr)
        return 2
    text = Path(sys.argv[1]).read_text(errors="replace")
    groups: dict[str, list[int]] = {"transition first frame": [], "target loop first frame": []}
    for source, target, _, since_input in PIXEL.findall(text):
        elapsed = int(since_input)
        if elapsed < 0:
            continue  # boot frame has no preceding input
        group = "target loop first frame" if source == target else "transition first frame"
        groups[group].append(elapsed)
    for name, values in groups.items():
        if values:
            print(f"{name}: n={len(values)}, median={statistics.median(values):.0f} ms, "
                  f"p95={percentile(values, .95):.0f} ms, max={max(values)} ms")
        else:
            print(f"{name}: no samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
