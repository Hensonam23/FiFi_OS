#!/usr/bin/env python3
"""Verify removing a pointer does not restart the live compositor."""

import sys


def load_ppm(path):
    with open(path, "rb") as image:
        if image.readline().strip() != b"P6":
            raise SystemExit(f"{path}: not a binary PPM")
        dimensions = image.readline()
        while dimensions.startswith(b"#"):
            dimensions = image.readline()
        width, height = map(int, dimensions.split())
        if image.readline().strip() != b"255":
            raise SystemExit(f"{path}: unsupported color depth")
        pixels = image.read()
    if len(pixels) != width * height * 3:
        raise SystemExit(f"{path}: truncated pixel data")
    return width, height, pixels


if len(sys.argv) != 3:
    raise SystemExit("usage: hotunplug_survival.py BEFORE AFTER")

before = load_ppm(sys.argv[1])
after = load_ppm(sys.argv[2])
if before[:2] != (1920, 1080) or after[:2] != before[:2]:
    raise SystemExit("hot-unplug check requires matching 1920x1080 screenshots")

# The network Settings window occupies the center of the screen. A compositor
# restart closes it and changes most pixels in this box back to the wallpaper.
x0, y0, x1, y1 = 450, 150, 1470, 950
width = before[0]
changed = 0
total = (x1 - x0) * (y1 - y0)
for y in range(y0, y1):
    row = y * width * 3
    for x in range(x0, x1):
        offset = row + x * 3
        if before[2][offset:offset + 3] != after[2][offset:offset + 3]:
            changed += 1

if changed > total // 5:
    raise SystemExit(
        f"pointer hot-unplug lost the live desktop ({changed} changed pixels)"
    )

print(f"[qemu-test] pointer hot-unplug preserved the compositor ({changed} changed pixels)")
