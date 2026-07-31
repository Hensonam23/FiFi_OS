#!/usr/bin/env python3
"""Compact, tolerant screenshot oracle for the FiFi QEMU boot gate."""

import json
import sys
from pathlib import Path

GRID_X = 32
GRID_Y = 18
MAX_MEAN_CHANNEL_ERROR = 18.0
MAX_TILE_CHANNEL_ERROR = 70.0
MAX_BAD_TILE_RATIO = 0.20


def read_token(stream):
    while True:
        token = stream.readline()
        if not token:
            raise ValueError("truncated PPM header")
        if not token.startswith(b"#"):
            return token


def read_ppm(path):
    with Path(path).open("rb") as stream:
        if stream.readline().strip() != b"P6":
            raise ValueError("QEMU screenshot is not a binary PPM")
        dimensions = read_token(stream).split()
        while len(dimensions) < 2:
            dimensions += read_token(stream).split()
        width, height = map(int, dimensions[:2])
        if int(read_token(stream)) != 255:
            raise ValueError("unsupported PPM color depth")
        pixels = stream.read()
    if len(pixels) != width * height * 3:
        raise ValueError("truncated PPM pixel data")
    return width, height, pixels


def signature(width, height, pixels):
    result = []
    for gy in range(GRID_Y):
        y0 = gy * height // GRID_Y
        y1 = (gy + 1) * height // GRID_Y
        for gx in range(GRID_X):
            x0 = gx * width // GRID_X
            x1 = (gx + 1) * width // GRID_X
            totals = [0, 0, 0]
            count = (x1 - x0) * (y1 - y0)
            for y in range(y0, y1):
                row = y * width * 3
                for x in range(x0, x1):
                    pos = row + x * 3
                    totals[0] += pixels[pos]
                    totals[1] += pixels[pos + 1]
                    totals[2] += pixels[pos + 2]
            result.append([round(value / count, 2) for value in totals])
    return result


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in {"record", "check"}:
        raise SystemExit("usage: screenshot_diff.py record|check IMAGE BASELINE")
    mode, image_path, baseline_path = sys.argv[1:]
    width, height, pixels = read_ppm(image_path)
    current = {
        "width": width,
        "height": height,
        "grid": [GRID_X, GRID_Y],
        "tiles": signature(width, height, pixels),
    }
    baseline = Path(baseline_path)
    if mode == "record":
        baseline.write_text(json.dumps(current, separators=(",", ":")) + "\n")
        print(f"[qemu-test] recorded screenshot baseline: {baseline}")
        return
    if not baseline.exists():
        raise SystemExit(f"missing screenshot baseline: {baseline}")
    expected = json.loads(baseline.read_text())
    for key in ("width", "height", "grid"):
        if current[key] != expected.get(key):
            raise SystemExit(
                f"screenshot {key} changed: {current[key]} != {expected.get(key)}"
            )
    errors = [
        sum(abs(a - b) for a, b in zip(actual, wanted)) / 3
        for actual, wanted in zip(current["tiles"], expected["tiles"])
    ]
    mean_error = sum(errors) / len(errors)
    bad_ratio = sum(error > MAX_TILE_CHANNEL_ERROR for error in errors) / len(errors)
    if mean_error > MAX_MEAN_CHANNEL_ERROR or bad_ratio > MAX_BAD_TILE_RATIO:
        raise SystemExit(
            "screenshot differs from baseline: "
            f"mean channel error {mean_error:.2f}, changed tiles {bad_ratio:.1%}"
        )
    print(
        "[qemu-test] screenshot matches baseline "
        f"(mean error {mean_error:.2f}, changed tiles {bad_ratio:.1%})"
    )


if __name__ == "__main__":
    main()
