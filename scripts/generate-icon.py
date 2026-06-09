from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "resources" / "icon.ico"


def draw_icon(size: int) -> Image.Image:
    scale = size / 96
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def box(values):
        return tuple(int(v * scale) for v in values)

    radius = int(22 * scale)
    draw.rounded_rectangle(box((2, 2, 94, 94)), radius=radius, fill=(108, 92, 231, 255))
    draw.rounded_rectangle(box((8, 8, 88, 88)), radius=int(17 * scale), outline=(255, 255, 255, 42), width=max(1, int(2 * scale)))

    width = max(3, int(7 * scale))
    for bbox in ((18, 31, 78, 72), (30, 43, 66, 72), (42, 56, 54, 72)):
        draw.arc(box(bbox), 205, 335, fill=(255, 255, 255, 255), width=width)

    shield = [
        (48 * scale, 33 * scale),
        (68 * scale, 41 * scale),
        (65 * scale, 65 * scale),
        (48 * scale, 78 * scale),
        (31 * scale, 65 * scale),
        (28 * scale, 41 * scale),
    ]
    draw.polygon(shield, fill=(255, 107, 107, 246))
    draw.line(box((48, 45, 48, 60)), fill=(255, 255, 255, 255), width=max(3, int(6 * scale)))
    dot = int(3.5 * scale)
    draw.ellipse((int(48 * scale - dot), int(68 * scale - dot), int(48 * scale + dot), int(68 * scale + dot)), fill=(255, 255, 255, 255))
    return image


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [draw_icon(size) for size in sizes]
    images[-1].save(OUT, format="ICO", sizes=[(size, size) for size in sizes], append_images=images[:-1])
    print(OUT)


if __name__ == "__main__":
    main()
