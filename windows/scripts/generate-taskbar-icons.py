"""Generate native-size taskbar glyphs (requires Pillow).

Straight strokes align with the target pixel grid. Only diagonal edges receive
coverage antialiasing; small glyphs are never downscaled from another ICO frame.
The original large application-icon frames are retained unchanged.
"""

import argparse
from io import BytesIO
from pathlib import Path
import struct

from PIL import Image, ImageChops, ImageDraw


SIZES = (16, 20, 24, 28, 32, 36, 40, 44, 48, 64)
IMAGE_DIR = Path(__file__).resolve().parents[1] / "image"


def glyph(size: int, mode: str) -> Image.Image:
    mask = Image.new("L", (size, size))
    draw = ImageDraw.Draw(mask)
    stroke = max(1, int(size / 16 + 0.5))

    def grid(value: float) -> int:
        return int(value * size / 16 + 0.5)

    def horizontal(left: float, top: float, right: float) -> None:
        x, y = grid(left), grid(top)
        draw.rectangle((x, y, grid(right) - 1, y + stroke - 1), fill=255)

    def vertical(left: float, top: float, bottom: float) -> None:
        x, y = grid(left), grid(top)
        draw.rectangle((x, y, x + stroke - 1, grid(bottom) - 1), fill=255)

    def diagonal(points: list[tuple[float, float]]) -> None:
        nonlocal mask
        scale = 4
        layer = Image.new("L", (size * scale, size * scale))
        # Pixel centers keep diagonal terminals connected to the snapped strokes.
        coordinates = [(int((grid(x) + stroke / 2) * scale),
                        int((grid(y) + stroke / 2) * scale)) for x, y in points]
        ImageDraw.Draw(layer).line(coordinates, fill=255, width=stroke * scale, joint="curve")
        mask = ImageChops.lighter(mask, layer.resize((size, size), Image.Resampling.BOX))

    if mode == "cn":
        horizontal(2, 5, 14)
        horizontal(2, 10, 14)
        vertical(2, 5, 11)
        vertical(13, 5, 11)
        vertical(7.5, 2, 14)
    elif mode == "en":
        horizontal(2, 3, 14)
        vertical(5, 1, 6)
        vertical(10, 1, 6)
        horizontal(3, 7, 13)
        horizontal(2, 10, 14)
        vertical(3, 7, 11)
        vertical(12, 7, 11)
        vertical(7.5, 6, 11)
        diagonal([(7.5, 10), (5.5, 12), (2, 14)])
        diagonal([(8, 10), (10, 12), (13, 14)])
    else:
        raise ValueError(mode)
    return mask


def brand(size: int) -> Image.Image:
    # Keep the established green frame, dark panel and white zigzag mark, with
    # simpler strokes fitted to each small taskbar size instead of texture blur.
    image = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(image)
    scale = size / 16
    left, right = round(2 * scale), round(14 * scale) - 1
    top, bottom = round(scale), round(15 * scale) - 1
    border = max(1, round(scale))
    draw.rectangle((left, top, right, bottom), fill="#252525", outline="#A8C897", width=border)
    supersample = 4
    mark = Image.new("L", (size * supersample, size * supersample))
    points = [(11, 3), (5, 6), (11, 8), (5, 10), (11, 12), (5, 14)]
    points = [(round(x * scale * supersample), round(y * scale * supersample)) for x, y in points]
    ImageDraw.Draw(mark).line(points, fill=255, width=max(4, round(scale * 1.2 * supersample)), joint="curve")
    mask = mark.resize((size, size), Image.Resampling.BOX)
    image.alpha_composite(Image.composite(Image.new("RGBA", image.size, "white"),
                                        Image.new("RGBA", image.size), mask))
    return image


def png_frame(image: Image.Image) -> bytes:
    output = BytesIO()
    image.save(output, format="PNG", optimize=False)
    return output.getvalue()


def original_large_frames(path: Path) -> dict[int, bytes]:
    data = path.read_bytes()
    frames = {}
    for index in range(struct.unpack_from("<H", data, 4)[0]):
        width, height, _, _, _, _, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        width, height = width or 256, height or 256
        if width == height and width >= 64:
            frames[width] = data[offset:offset + length]
    return frames


def ico(frames: dict[int, bytes]) -> bytes:
    offset = 6 + 16 * len(frames)
    entries, payloads = [], []
    for size, data in sorted(frames.items()):
        entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        payloads.append(data)
        offset += len(data)
    return struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries + payloads)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = {}
    for mode in ("cn", "en"):
        for theme, color in (("light", "#202020"), ("dark", "#FFFFFF")):
            frames = {}
            for size in SIZES:
                image = Image.new("RGBA", (size, size), color)
                image.putalpha(glyph(size, mode))
                frames[size] = png_frame(image)
            outputs[f"{mode}-{theme}.ico"] = ico(frames)
    path = IMAGE_DIR / "msime.ico"
    frames = original_large_frames(path)
    frames.update({size: png_frame(brand(size)) for size in SIZES if size < 64})
    outputs[path.name] = ico(frames)
    for name, content in outputs.items():
        path = IMAGE_DIR / name
        if args.check:
            if path.read_bytes() != content:
                raise SystemExit(f"Stale taskbar icon: {path}")
        else:
            path.write_bytes(content)
        print(f"{'Checked' if args.check else 'Generated'} {name}")


if __name__ == "__main__":
    main()
