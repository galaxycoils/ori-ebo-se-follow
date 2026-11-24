"""Deterministic synthetic grayscale scenes for replay and the local demo."""

import math
from functools import lru_cache

WIDTH = 320
HEIGHT = 180
REFERENCE = (120, 65, 80, 80)
BACKGROUND = bytes([34]) * (WIDTH * HEIGHT)


@lru_cache(maxsize=96)
def texture(width, height, variant):
    return bytes(
        round(153 + 27 * math.sin(x / width * 23 + variant)
              + 29 * math.cos(y / height * 17)
              + 23 * math.sin((x / width + y / height) * 14))
        for y in range(height) for x in range(width)
    )


def scene(*boxes):
    pixels = bytearray(BACKGROUND)
    for x, y, width, height in boxes:
        patch = texture(width, height, 0)
        for row in range(height):
            start = (y + row) * WIDTH + x
            pixels[start:start + width] = patch[row * width:(row + 1) * width]
    return pixels


def retreat_steps():
    yield (124, 66, 72, 72)
    yield (128, 67, 64, 64)
    yield (128, 66, 64, 64)
    yield (128, 65, 64, 64)
    yield (124, 65, 72, 72)
    yield (120, 65, 80, 80)
