"""Draws the Stemcel logo: Yggdrasil, the world tree, as a flat green print on a black circle.

One symmetrical tree: a trunk that widens into its roots and divides into limbs, the limbs
forking up into a rounded crown and the roots mirroring them below, every branch growing
into a ring inside the circle. Each limb is a tapered stroke, rounded at both ends, so a
fork joins smoothly. Drawn at 4096 px and scaled down for each file (that is the
anti-aliasing).

usage: python scripts/make-logo.py
"""
import math
import os

from PIL import Image, ImageDraw

SIZE = 4096
GREEN = (51, 255, 110, 255)  # the theme's accent (Theme.h sky500)
BLACK = (0, 0, 0, 255)
CENTRE = SIZE / 2
RADIUS = SIZE * 0.48     # the black disc
RING = RADIUS * 0.92     # the green ring the branches grow into
RING_WIDTH = SIZE * 0.018


def limb(draw, x0, y0, w0, x1, y1, w1):
    """A tapered stroke from (x0, y0) at width w0 to (x1, y1) at width w1, round at both ends."""
    angle = math.atan2(y1 - y0, x1 - x0)
    nx, ny = -math.sin(angle), math.cos(angle)
    draw.polygon([(x0 + nx * w0 / 2, y0 + ny * w0 / 2), (x1 + nx * w1 / 2, y1 + ny * w1 / 2),
                  (x1 - nx * w1 / 2, y1 - ny * w1 / 2), (x0 - nx * w0 / 2, y0 - ny * w0 / 2)], fill=GREEN)
    for x, y, w in ((x0, y0, w0), (x1, y1, w1)):
        draw.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=GREEN)


MIN_TILT = math.radians(20)  # no limb flatter than this, so none reads as a spoke


def grow(draw, x, y, tips, width, total, rise=0.0):
    """A limb from (x, y) serving `tips` (points on the ring, in order around it): it grows
    part of the way towards their middle, then forks into two limbs that each serve half.
    A single tip is reached in one stroke, so every twig meets the ring and, the tips being
    in order, no two branches cross. A limb's width follows how many tips it feeds."""
    w = width * (len(tips) / total) ** 0.55
    if len(tips) == 1:
        tx, ty = tips[0]
        # On to the ring and a little past it, under the ring's stroke.
        dx, dy = tx - x, ty - y
        d = math.hypot(dx, dy)
        limb(draw, x, y, w * 1.1, tx + dx / d * RING_WIDTH, ty + dy / d * RING_WIDTH, w * 0.8)
        return
    mx = sum(p[0] for p in tips) / len(tips)
    my = sum(p[1] for p in tips) / len(tips)
    # Longer steps while many tips share a limb, shorter as it nears the ring.
    reach = 0.42 if len(tips) > 4 else 0.5
    x1, y1 = x + (mx - x) * reach, y + (my - y) * reach
    # The first step of a main limb rises (or falls, for a root) before it turns outward.
    x1 = x + (x1 - x) * (1.0 - rise)
    limb(draw, x, y, w * 1.1, x1, y1, w)
    half = len(tips) // 2
    grow(draw, x1, y1, tips[:half], width, total)
    grow(draw, x1, y1, tips[half:], width, total)


def ring_points(count, upward):
    """`count` points spread evenly over the ring's upper (or lower) arc, clear of the sides."""
    points = []
    for i in range(count):
        a = MIN_TILT + (math.pi - 2 * MIN_TILT) * (i + 0.5) / count
        points.append((CENTRE + RING * math.cos(a), CENTRE + (-1 if upward else 1) * RING * math.sin(a)))
    return points


def trunk(draw, top, bottom, top_width, waist_width, bottom_width):
    """The trunk as one smooth outline, pinched a little above its middle."""
    left, right = [], []
    steps = 64
    for i in range(steps + 1):
        u = i / steps
        w = top_width * (1 - u) ** 2 + 2 * u * (1 - u) * waist_width + bottom_width * u ** 2
        y = top + (bottom - top) * u
        left.append((CENTRE - w / 2, y))
        right.append((CENTRE + w / 2, y))
    draw.polygon(left + right[::-1], fill=GREEN)


def draw_logo():
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(image).ellipse([CENTRE - RADIUS, CENTRE - RADIUS, CENTRE + RADIUS, CENTRE + RADIUS], fill=BLACK)

    tree = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    t = ImageDraw.Draw(tree)
    crown_base = CENTRE - SIZE * 0.05   # where the trunk divides
    root_base = CENTRE + SIZE * 0.09    # where it spreads into roots
    # The trunk, flaring into the crown above and the roots below.
    trunk(t, crown_base - SIZE * 0.01, root_base + SIZE * 0.01, SIZE * 0.048, SIZE * 0.038, SIZE * 0.08)

    # The crown: 24 twigs spread over the top of the ring, in three main limbs; the roots
    # mirror it below with 16.
    crown = ring_points(24, upward=True)
    for group, rise in ((crown[:8], 0.45), (crown[8:16], 0.0), (crown[16:], 0.45)):
        grow(t, CENTRE, crown_base, group, SIZE * 0.06, len(crown), rise)
    roots = ring_points(16, upward=False)
    for group, rise in ((roots[:5], 0.35), (roots[5:11], 0.0), (roots[11:], 0.35)):
        grow(t, CENTRE, root_base, group, SIZE * 0.075, len(roots), rise)

    # Everything stops at the ring.
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse([CENTRE - RING, CENTRE - RING, CENTRE + RING, CENTRE + RING], fill=255)
    image.paste(tree, (0, 0), Image.composite(tree, Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0)), mask))
    ImageDraw.Draw(image).ellipse([CENTRE - RING, CENTRE - RING, CENTRE + RING, CENTRE + RING], outline=GREEN,
                                  width=int(RING_WIDTH))
    return image


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logo = draw_logo()
    outputs = {
        os.path.join(root, "images", "logo-1024.png"): 1024,
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


if __name__ == "__main__":
    main()
