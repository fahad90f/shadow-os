#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prints the colour of one pixel of a PNG file as #RRGGBB (standard library
only), for session tests that check what is on the screen:

    png-pixel.py FILE.png X Y

Supported: 8-bit greyscale, RGB, palette, grey+alpha and RGBA images, not
interlaced (what xwd2png.py and ImageMagick's "import" write). Exit status 2
for anything else or a point outside the image.
"""
import struct
import sys
import zlib

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def pixel(path, x, y):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, idat, palette, header = 8, b"", None, None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError("no IHDR chunk")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in CHANNELS or interlace != 0:
        raise ValueError(f"unsupported PNG (bit depth {depth}, colour type {colour}, interlace {interlace})")
    if not (0 <= x < width and 0 <= y < height):
        raise ValueError(f"({x}, {y}) is outside the {width}x{height} image")
    bpp = CHANNELS[colour]
    stride = width * bpp
    raw = zlib.decompress(idat)
    previous = bytearray(stride)
    row = None
    for r in range(y + 1):
        start = r * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = previous[i]
            corner = previous[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                line[i] = (line[i] + paeth(left, up, corner)) & 0xFF
            elif kind != 0:
                raise ValueError(f"unknown filter type {kind} in row {r}")
        previous, row = line, line
    values = row[x * bpp:(x + 1) * bpp]
    if colour == 3:
        if palette is None:
            raise ValueError("palette image without PLTE")
        values = palette[values[0] * 3:values[0] * 3 + 3]
    elif colour in (0, 4):
        values = bytes([values[0]] * 3)
    return "#%02X%02X%02X" % tuple(values[:3])


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        print(pixel(sys.argv[1], int(sys.argv[2]), int(sys.argv[3])))
    except (OSError, ValueError, zlib.error, struct.error) as exc:
        print(f"png-pixel: {sys.argv[1]}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
