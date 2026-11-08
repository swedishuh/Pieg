
#!/usr/bin/env python3

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

MAX_DIMENSION = 4096

def convert(source: Path, destination: Path) -> None:
    with Image.open(source) as original:
        image = original.convert("RGBA")

        if image.width > MAX_DIMENSION or image.height > MAX_DIMENSION:
            raise ValueError(
                f"image is too large: maximum is "
                f"{MAX_DIMENSION}x{MAX_DIMENSION}"
            )

        # Composite transparent pixels onto white.
        background = Image.new("RGBA", image.size, (255, 255, 255, 255))
        background.alpha_composite(image)

        rgb = background.convert("RGB")

        header = struct.pack(
            "<4sIII",
            b"RPF1",
            rgb.width,
            rgb.height,
            1,  # RGB24
        )

        destination.parent.mkdir(parents=True, exist_ok=True)

        with destination.open("wb") as output:
            output.write(header)
            output.write(rgb.tobytes())

    print(
        f"Converted {source} -> {destination} "
        f"({rgb.width}x{rgb.height}, RGB24)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert images into PIEG RPF1 files."
    )

    parser.add_argument("input", type=Path, help="source image")
    parser.add_argument("output", type=Path, help="output .RPF file")

    args = parser.parse_args()

    if not args.input.is_file():
        print(f"eye-convert: input file not found: {args.input}", file=sys.stderr)
        return 1

    try:
        convert(args.input, args.output)
    except Exception as error:
        print(f"eye-convert: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
