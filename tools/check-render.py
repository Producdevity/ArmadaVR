#!/usr/bin/env python3
"""Check decoded RGB content in the lab's two virtual display captures."""
import subprocess
import sys


def decode(path):
    return subprocess.run(["magick", path, "-depth", "8", "rgb:-"],
                          check=True, capture_output=True, timeout=10).stdout


a, b = (decode(path) for path in sys.argv[1:])
if len(a) != 1280 * 800 * 3 or len(b) != len(a):
    raise SystemExit("Unexpected virtual display capture dimensions")
changed = sum(a[i:i+3] != b[i:i+3] for i in range(0, len(a), 3))
colors = len(set(zip(b[0::3], b[1::3], b[2::3])))
if changed < 100 or colors < 5:
    raise SystemExit(f"Insufficient rendered content: changed={changed}, colors={colors}")
print(f"ARMADA_VR_RENDER_PASS changed_pixels={changed} colors={colors}")
