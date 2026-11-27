"""Deterministic synthetic grayscale scenes for replay and the local demo."""

import math
from functools import lru_cache

WIDTH = 320
HEIGHT = 180
REFERENCE = (120, 65, 80, 80)
MOVEMENT = [((71, 65, 80, 80), 12), (REFERENCE, 12),
            ((130, 63, 60, 60), 12), ((130, 63, 60, 60), 4), (REFERENCE, 12)]
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


@lru_cache(maxsize=64)
def color_texture(width, height, variant=0):
    out = bytearray(width * height * 3)
    idx = 0
    for y in range(height):
        ny = y / height
        c_y = math.cos(ny * 17)
        for x in range(width):
            nx = x / width
            luma = 153 + 27 * math.sin(nx * 23 + variant) + 29 * c_y + 23 * math.sin((nx + ny) * 14)
            r = max(0, min(255, round(luma * 1.18 + 18)))
            g = max(0, min(255, round(luma * 0.94 + 4)))
            b = max(0, min(255, round(luma * 0.70)))
            out[idx] = r
            out[idx + 1] = g
            out[idx + 2] = b
            idx += 3
    return bytes(out)


@lru_cache(maxsize=48)
def color_backdrop(pan_x=0, pan_y=0):
    out = bytearray(WIDTH * HEIGHT * 3)
    horizon = max(36, min(110, 74 + pan_y))
    for y in range(HEIGHT):
        row = bytearray(WIDTH * 3)
        if y < horizon:
            shade = 28 + (y * 18) // horizon
            for x in range(WIDTH):
                stripe = 5 if ((x + pan_x) // 48) % 2 == 0 else 0
                i = x * 3
                row[i] = shade + 6 + stripe
                row[i + 1] = shade + 12 + stripe
                row[i + 2] = shade + 18 + stripe
        else:
            depth = (y - horizon) / max(1, HEIGHT - horizon)
            base_r = round(42 + depth * 28)
            base_g = round(36 + depth * 22)
            base_b = round(30 + depth * 16)
            for x in range(WIDTH):
                plank = 4 if ((x + pan_x + (y // 16) * 20) % 40) < 2 else 0
                i = x * 3
                row[i] = max(0, base_r - plank)
                row[i + 1] = max(0, base_g - plank)
                row[i + 2] = max(0, base_b - plank)
        out[y * WIDTH * 3:(y + 1) * WIDTH * 3] = row
    return bytearray(out)


def color_scene(*boxes, pan=(0, 0)):
    pixels = bytearray(color_backdrop(int(round(pan[0])), int(round(pan[1]))))
    for x, y, width, height in boxes:
        patch = color_texture(width, height, 0)
        for row in range(height):
            dst = ((y + row) * WIDTH + x) * 3
            src = row * width * 3
            pixels[dst:dst + width * 3] = patch[src:src + width * 3]
    return bytes(pixels)


def retreat_steps():
    yield (124, 66, 72, 72)
    yield (128, 67, 64, 64)
    yield (128, 66, 64, 64)
    yield (128, 65, 64, 64)
    yield (124, 65, 72, 72)
    yield (120, 65, 80, 80)
