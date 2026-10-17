#!/usr/bin/env python3
"""Generate the Windows icon used by notepadcc from the project logo."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MASTER = ROOT / "res" / "notepadcc-logo.png"
DEFAULT_ICON = ROOT / "res" / "notepadcc.ico"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def prepare_master(source: Path) -> Image.Image:
    """Turn the square logo into a transparent circular master image."""
    image = Image.open(source).convert("RGBA")
    width, height = image.size
    if width != height:
        raise ValueError(f"logo must be square, got {width}x{height}")

    # The supplied artwork is a circular emblem on a black square. Remove the
    # thin outer border, then apply an antialiased ellipse so Windows never
    # paints black corners behind the badge.
    inset = max(1, round(width * 0.0231))
    image = image.crop((inset, inset, width - inset, height - inset))
    size = image.width
    scale = 4
    mask = Image.new("L", (size * scale, size * scale), 0)
    ImageDraw.Draw(mask).ellipse(
        (2 * scale, 2 * scale, (size - 3) * scale, (size - 3) * scale),
        fill=255,
    )
    image.putalpha(mask.resize((size, size), Image.Resampling.LANCZOS))
    return image.resize((512, 512), Image.Resampling.LANCZOS)


def verify_icon(path: Path) -> None:
    data = path.read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or count != len(SIZES):
        raise ValueError("notepadcc.ico is not a valid multi-resolution ICO")
    actual = []
    for index in range(count):
        width, height, _, _, _, _, _, _ = struct.unpack_from(
            "<BBBBHHII", data, 6 + index * 16
        )
        actual.append((width or 256, height or 256))
    expected = [(size, size) for size in SIZES]
    if actual != expected:
        raise ValueError(f"unexpected ICO sizes: {actual}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path,
                        help="square source artwork; also refreshes the PNG master")
    parser.add_argument("--master", type=Path, default=DEFAULT_MASTER)
    parser.add_argument("--output", type=Path, default=DEFAULT_ICON)
    args = parser.parse_args()

    master_path = args.master.resolve()
    output_path = args.output.resolve()
    if args.source:
        master = prepare_master(args.source.resolve())
        master_path.parent.mkdir(parents=True, exist_ok=True)
        master.save(master_path, optimize=True)
    else:
        master = Image.open(master_path).convert("RGBA")

    if master.width != master.height:
        raise ValueError(f"icon master must be square, got {master.size}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    master.save(output_path, format="ICO", sizes=[(size, size) for size in SIZES],
                bitmap_format="bmp")
    verify_icon(output_path)
    print(f"[ok] {master_path}")
    print(f"[ok] {output_path} ({', '.join(str(size) for size in SIZES)} px)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
