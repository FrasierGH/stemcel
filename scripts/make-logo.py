"""Draws the Stemcel logo: Yggdrasil, the world tree, as a flat green print on a black circle.

The tree is symmetrical: a tapered trunk, branches that fork and spread up into a rounded
crown with leaf clusters at their tips, and roots that mirror them below. Everything is
drawn at 2048 px and scaled down for each file.

usage: python scripts/make-logo.py
"""
import math
import os

from PIL import Image, ImageDraw

SIZE = 2048
GREEN = (51, 255, 110, 255)  # the theme's accent (Theme.h sky500)
BLACK = (0, 0, 0, 255)
CENTRE = SIZE / 2
RADIUS = SIZE * 0.48


def branch(draw, x, y, angle, length, width, depth, spread, shrink, tips, upward):
    """One limb and the two it forks into; tips collect where the leaves go."""
    x2 = x + math.cos(angle) * length
    y2 = y - math.sin(angle) * length if upward else y + math.sin(angle) * length
    draw.line([(x, y), (x2, y2)], fill=GREEN, width=max(3, int(width)))
    r = width / 2
    draw.ellipse([x2 - r, y2 - r, x2 + r, y2 + r], fill=GREEN)
    if depth == 0:
        tips.append((x2, y2))
        return
    for side in (-1, 1):
        branch(draw, x2, y2, angle + side * spread, length * shrink, width * 0.7, depth - 1, spread * 0.9, shrink, tips, upward)


def draw_logo():
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse([CENTRE - RADIUS, CENTRE - RADIUS, CENTRE + RADIUS, CENTRE + RADIUS], fill=BLACK)

    # The tree on its own layer, so the ring can cut its branches and roots off cleanly.
    tree = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    t = ImageDraw.Draw(tree)
    trunk_top = CENTRE - SIZE * 0.04
    trunk_bottom = CENTRE + SIZE * 0.10
    t.polygon([(CENTRE - SIZE * 0.028, trunk_top), (CENTRE + SIZE * 0.028, trunk_top),
               (CENTRE + SIZE * 0.05, trunk_bottom), (CENTRE - SIZE * 0.05, trunk_bottom)], fill=GREEN)

    # The crown: five limbs fanning up and out, each forking four times.
    tips = []
    for offset, length in ((0.0, 0.15), (0.5, 0.16), (-0.5, 0.16), (1.05, 0.15), (-1.05, 0.15)):
        branch(t, CENTRE, trunk_top, math.pi / 2 + offset, SIZE * length, SIZE * 0.032, 4, 0.36, 0.72, tips, True)
    for x, y in tips:
        r = SIZE * 0.012
        t.ellipse([x - r, y - r, x + r, y + r], fill=GREEN)

    # The roots mirror it below.
    roots = []
    for offset, length in ((0.0, 0.15), (0.55, 0.17), (-0.55, 0.17), (1.1, 0.17), (-1.1, 0.17)):
        branch(t, CENTRE, trunk_bottom, math.pi / 2 + offset, SIZE * length, SIZE * 0.028, 3, 0.4, 0.72, roots, False)

    ring = RADIUS * 0.93
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse([CENTRE - ring, CENTRE - ring, CENTRE + ring, CENTRE + ring], fill=255)
    image.paste(tree, (0, 0), Image.composite(tree, Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0)), mask))

    # The ring the branches and roots grow into.
    draw.ellipse([CENTRE - ring, CENTRE - ring, CENTRE + ring, CENTRE + ring], outline=GREEN, width=int(SIZE * 0.016))
    return image


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logo = draw_logo()
    outputs = {
        os.path.join(root, "images", "logo-large.png"): 550,
        os.path.join(root, "images", "logo-small.png"): 330,
        os.path.join(root, "backend", "app", "resources", "256x256.png"): 256,
    }
    for path, size in outputs.items():
        logo.resize((size, size), Image.LANCZOS).save(path)
        print("wrote", path)
    ico = os.path.join(root, "backend", "app", "resources", "stemcel-plugin.ico")
    logo.resize((256, 256), Image.LANCZOS).save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", ico)
    svg_preview = os.path.join(root, "images", "logo-1024.png")
    logo.resize((1024, 1024), Image.LANCZOS).save(svg_preview)
    print("wrote", svg_preview)


if __name__ == "__main__":
    main()
