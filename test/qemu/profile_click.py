#!/usr/bin/env python3
"""Confirm the QEMU pointer click moved the active profile to Play."""

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


def average_blue(image, box):
    width, height, pixels = image
    x0, y0, x1, y1 = box
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise SystemExit("profile sample box is outside the screenshot")
    total = count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += pixels[(y * width + x) * 3 + 2]
            count += 1
    return total / count


before = load_ppm(sys.argv[1])
after = load_ppm(sys.argv[2])
if before[:2] != (1920, 1080) or after[:2] != (1920, 1080):
    raise SystemExit("profile click check requires the 1920x1080 QEMU gate")

general_box = (116, 26, 212, 51)
play_box = (412, 26, 478, 51)
before_general = average_blue(before, general_box)
before_play = average_blue(before, play_box)
after_general = average_blue(after, general_box)
after_play = average_blue(after, play_box)

if before_general <= before_play + 30:
    raise SystemExit("General was not selected before the profile click")
if after_play <= after_general + 30:
    raise SystemExit("Play was not selected after the profile click")

print("[qemu-test] top-bar profile click selected Play")
