"""Generate assets/padsplit.ico (16/32/48/256 px PNG-in-ICO) using pure Python stdlib."""

import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_ICO = ROOT / "assets" / "padsplit.ico"


def clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return lo if v < lo else (hi if v > hi else v)


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def blend(dst: tuple[float, float, float, float], src: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    sr, sg, sb, sa = src
    dr, dg, db, da = dst
    out_a = sa + da * (1.0 - sa)
    if out_a <= 1e-6:
        return (0.0, 0.0, 0.0, 0.0)
    out_r = (sr * sa + dr * da * (1.0 - sa)) / out_a
    out_g = (sg * sa + dg * da * (1.0 - sa)) / out_a
    out_b = (sb * sa + db * da * (1.0 - sa)) / out_a
    return (out_r, out_g, out_b, out_a)


def sd_rounded_box(px: float, py: float, cx: float, cy: float, hx: float, hy: float, r: float) -> float:
    dx = abs(px - cx) - (hx - r)
    dy = abs(py - cy) - (hy - r)
    outside = math.hypot(max(dx, 0.0), max(dy, 0.0))
    inside = min(max(dx, dy), 0.0)
    return outside + inside - r


def sd_circle(px: float, py: float, cx: float, cy: float, r: float) -> float:
    return math.hypot(px - cx, py - cy) - r


def sd_segment(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = clamp((pax * bax + pay * bay) / denom) if denom > 1e-9 else 0.0
    return math.hypot(pax - bax * h, pay - bay * h)


def point_in_triangle(px: float, py: float, ax: float, ay: float, bx: float, by: float, cx: float, cy: float) -> bool:
    d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by)
    d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy)
    d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay)
    has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (has_neg and has_pos)


def sample_pixel(u: float, v: float, aa: float) -> tuple[float, float, float, float]:
    # Normalized coordinates u, v in [0, 1]
    col = (0.0, 0.0, 0.0, 0.0)

    # Outer dark slate card (rounded square monitor frame)
    d_outer = sd_rounded_box(u, v, 0.5, 0.5, 0.45, 0.39, 0.11)
    a_outer = clamp(0.5 - d_outer / aa)
    if a_outer > 0.0:
        col = blend(col, (15 / 255, 23 / 255, 42 / 255, a_outer))

    # Subtle outer rim highlight
    d_rim = abs(d_outer + 0.015) - 0.016
    a_rim = clamp(0.5 - d_rim / aa) * a_outer
    if a_rim > 0.0:
        rim_t = clamp(u)
        col = blend(col, (lerp(56, 52, rim_t) / 255, lerp(189, 211, rim_t) / 255, lerp(248, 153, rim_t) / 255, a_rim * 0.85))

    # Left half: Primary game screen (sky blue gradient)
    d_left = sd_rounded_box(u, v, 0.285, 0.5, 0.185, 0.315, 0.065)
    a_left = clamp(0.5 - d_left / aa)
    if a_left > 0.0:
        gt = clamp((v - 0.18) / 0.64)
        col = blend(col, (lerp(56, 2, gt) / 255, lerp(189, 132, gt) / 255, lerp(248, 199, gt) / 255, a_left))

    # Right half: Secondary desktop/agent screen (emerald gradient)
    d_right = sd_rounded_box(u, v, 0.715, 0.5, 0.185, 0.315, 0.065)
    a_right = clamp(0.5 - d_right / aa)
    if a_right > 0.0:
        gt = clamp((v - 0.18) / 0.64)
        col = blend(col, (lerp(52, 5, gt) / 255, lerp(211, 150, gt) / 255, lerp(153, 105, gt) / 255, a_right))

    # Minimalist white gamepad silhouette inside left screen
    d_pad_body = sd_rounded_box(u, v, 0.285, 0.485, 0.105, 0.058, 0.048)
    d_grip_l = sd_segment(u, v, 0.215, 0.50, 0.195, 0.575) - 0.036
    d_grip_r = sd_segment(u, v, 0.355, 0.50, 0.375, 0.575) - 0.036
    d_pad = min(d_pad_body, d_grip_l, d_grip_r)
    a_pad = clamp(0.5 - d_pad / aa) * a_left
    if a_pad > 0.0:
        col = blend(col, (1.0, 1.0, 1.0, a_pad * 0.96))

    # D-pad cross & face button dots carved in deep blue inside gamepad
    d_dpad_h = sd_rounded_box(u, v, 0.238, 0.485, 0.028, 0.009, 0.004)
    d_dpad_v = sd_rounded_box(u, v, 0.238, 0.485, 0.009, 0.028, 0.004)
    d_btn_1 = sd_circle(u, v, 0.335, 0.468, 0.012)
    d_btn_2 = sd_circle(u, v, 0.356, 0.488, 0.012)
    d_btn_3 = sd_circle(u, v, 0.314, 0.488, 0.012)
    d_btn_4 = sd_circle(u, v, 0.335, 0.508, 0.012)
    d_carve = min(d_dpad_h, d_dpad_v, d_btn_1, d_btn_2, d_btn_3, d_btn_4)
    a_carve = clamp(0.5 - d_carve / aa) * a_pad
    if a_carve > 0.0:
        col = blend(col, (3 / 255, 105 / 255, 161 / 255, a_carve))

    # Minimalist white cursor arrow inside right screen
    # Triangle head: tip=(0.645, 0.35), bottom-left=(0.645, 0.62), right-wing=(0.815, 0.515)
    # Plus stem from (0.71, 0.535) to (0.77, 0.645)
    in_tri = point_in_triangle(u, v, 0.645, 0.345, 0.645, 0.625, 0.815, 0.515)
    # Cut slight notch at bottom-center of triangle
    in_notch = point_in_triangle(u, v, 0.715, 0.535, 0.645, 0.625, 0.815, 0.515) and (v > 0.545)
    d_stem = sd_segment(u, v, 0.712, 0.525, 0.775, 0.645) - 0.023
    a_stem = clamp(0.5 - d_stem / aa)
    a_arrow = max(1.0 if (in_tri and not in_notch) else 0.0, a_stem) * a_right
    if a_arrow > 0.0:
        col = blend(col, (1.0, 1.0, 1.0, a_arrow * 0.96))

    return col


def render_png(size: int) -> bytes:
    # 2x2 supersampling for crisp edges
    aa = 1.2 / size
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # PNG filter type None
        for x in range(size):
            acc_r = acc_g = acc_b = acc_a = 0.0
            for sy in (0.25, 0.75):
                for sx in (0.25, 0.75):
                    r, g, b, a = sample_pixel((x + sx) / size, (y + sy) / size, aa)
                    acc_r += r * a
                    acc_g += g * a
                    acc_b += b * a
                    acc_a += a
            a = acc_a / 4.0
            if a > 1e-6:
                r = acc_r / acc_a
                g = acc_g / acc_a
                b = acc_b / acc_a
            else:
                r = g = b = 0.0
            raw.append(int(clamp(r) * 255 + 0.5))
            raw.append(int(clamp(g) * 255 + 0.5))
            raw.append(int(clamp(b) * 255 + 0.5))
            raw.append(int(clamp(a) * 255 + 0.5))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def write_ico(path: Path, sizes: tuple[int, ...] = (16, 32, 48, 256)) -> None:
    pngs = [(s, render_png(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries = bytearray()
    body = bytearray()
    for s, data in pngs:
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        body += data
        offset += len(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes(entries) + bytes(body))
    print(f"[icon] Wrote {path} ({path.stat().st_size:,} bytes, sizes={sizes})")


if __name__ == "__main__":
    write_ico(OUT_ICO)
