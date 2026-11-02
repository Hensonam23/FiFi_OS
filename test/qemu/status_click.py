#!/usr/bin/env python3
"""Verify each visible Horizon-bar control produces its intended surface."""

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


def changed(before, after, box):
    width, height, first = before
    if after[:2] != (width, height):
        raise SystemExit("status screenshots have different dimensions")
    x0, y0, x1, y1 = box
    second = after[2]
    count = 0
    for y in range(y0, y1):
        row = y * width * 3
        for x in range(x0, x1):
            offset = row + x * 3
            if first[offset:offset + 3] != second[offset:offset + 3]:
                count += 1
    return count


if len(sys.argv) != 6:
    raise SystemExit("usage: status_click.py BASE BRAND VOLUME CLOCK NETWORK")

base, brand, volume, clock, network = map(load_ppm, sys.argv[1:])
if base[:2] != (1920, 1080):
    raise SystemExit("status click check requires the 1920x1080 QEMU gate")

checks = (
    ("FiFi app menu", brand, (16, 64, 720, 760), 3000),
    ("volume popup", volume, (1550, 64, 1910, 260), 1000),
    ("clock calendar", clock, (1400, 64, 1910, 700), 5000),
    ("network settings", network, (300, 100, 1620, 1000), 10000),
)
for label, image, box, minimum in checks:
    difference = changed(base, image, box)
    if difference < minimum:
        raise SystemExit(
            f"{label} did not visibly open ({difference} changed pixels)"
        )
    print(f"[qemu-test] {label} opened ({difference} changed pixels)")
