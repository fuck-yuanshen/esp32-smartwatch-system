from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "sdcard" / "fonts" / "HZK16.hzk"
FONT_CANDIDATES = [
    Path(r"C:\Windows\Fonts\simsun.ttc"),
    Path(r"C:\Windows\Fonts\simhei.ttf"),
    Path(r"C:\Windows\Fonts\msyh.ttc"),
]


def pick_font() -> Path:
    for path in FONT_CANDIDATES:
        if path.exists():
            return path
    raise FileNotFoundError("No Chinese Windows font found")


def glyph_bytes(font: ImageFont.FreeTypeFont, char: str) -> bytes:
    image = Image.new("1", (16, 16), 0)
    draw = ImageDraw.Draw(image)
    bbox = draw.textbbox((0, 0), char, font=font)
    width = bbox[2] - bbox[0]
    height = bbox[3] - bbox[1]
    x = max(0, (16 - width) // 2 - bbox[0])
    y = max(0, (16 - height) // 2 - bbox[1] - 1)
    draw.text((x, y), char, fill=1, font=font)

    data = bytearray()
    for row in range(16):
        left = 0
        right = 0
        for col in range(8):
            if image.getpixel((col, row)):
                left |= 0x80 >> col
        for col in range(8, 16):
            if image.getpixel((col, row)):
                right |= 0x80 >> (col - 8)
        data.append(left)
        data.append(right)
    return bytes(data)


def main() -> None:
    font_path = pick_font()
    font = ImageFont.truetype(str(font_path), 16)
    OUT.parent.mkdir(parents=True, exist_ok=True)

    with OUT.open("wb") as output:
        for high in range(0xA1, 0xFF):
            for low in range(0xA1, 0xFF):
                try:
                    char = bytes([high, low]).decode("gb2312")
                    output.write(glyph_bytes(font, char))
                except UnicodeDecodeError:
                    output.write(bytes(32))

    print(f"Wrote {OUT}")
    print(f"Font source: {font_path}")
    print(f"Size: {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
