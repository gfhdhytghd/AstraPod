#!/usr/bin/env python3
"""
Generate exact 320x240 pixel UI mockups for AstraPod T3 Code Remote Control
in two native looks:
  - mockups/fruit-company/*.png  (classic light stock-like look, AstraPod branding)
  - mockups/scrim/*.png          (AstraPod built-in Scrim dark blurred-cover theme)

Usage:
  python3 docs/podbox/design/t3-remote/render_mockups.py
"""

from __future__ import annotations

import math
import os
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 320, 240
ROOT = Path(__file__).resolve().parent
OUT_FRUIT = ROOT / "mockups" / "fruit-company"
OUT_SCRIM = ROOT / "mockups" / "scrim"

# Catppuccin-Latte semantic dot palette (matches agents_screen.c)
DOT_RED = (210, 15, 57)      # asking / approval
DOT_FAIL = (224, 79, 51)     # failed / error
DOT_GREEN = (64, 160, 43)    # working / idle ring
DOT_TEAL = (23, 146, 153)    # done
DOT_GREY = (156, 160, 176)   # unknown
DOT_AMBER = (230, 154, 38)   # question accent


def find_font(candidates: Iterable[str]) -> str:
    for path in candidates:
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(f"None of the candidate fonts exist: {list(candidates)}")


FONT_REG_PATH = find_font([
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/harmonyos-sans/HarmonyOS_Sans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
])
FONT_MED_PATH = find_font([
    "/usr/share/fonts/noto/NotoSans-Medium.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
])
FONT_BOLD_PATH = find_font([
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
])
FONT_MONO_PATH = find_font([
    "/usr/share/fonts/TTF/JetBrainsMonoNL-Medium.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    FONT_REG_PATH,
])


def font_reg(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_REG_PATH, size)


def font_med(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_MED_PATH, size)


def font_bold(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_BOLD_PATH, size)


def font_mono(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_MONO_PATH, size)


def text_size(draw: ImageDraw.ImageDraw, text: str, fnt: ImageFont.FreeTypeFont) -> Tuple[int, int]:
    bbox = draw.textbbox((0, 0), text, font=fnt)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


def draw_centered(
    draw: ImageDraw.ImageDraw,
    cx: int,
    cy: int,
    text: str,
    fnt: ImageFont.FreeTypeFont,
    fill: Tuple[int, int, int],
) -> None:
    bbox = draw.textbbox((0, 0), text, font=fnt)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    draw.text((cx - tw // 2 - bbox[0], cy - th // 2 - bbox[1]), text, font=fnt, fill=fill)


def draw_key_pair(
    draw: ImageDraw.ImageDraw,
    x: int,
    y: int,
    key: str,
    label: str,
    key_col: Tuple[int, int, int],
    lbl_col: Tuple[int, int, int],
    size: int = 11,
    gap: int = 5,
) -> int:
    kf = font_bold(size)
    lf = font_reg(size)
    draw.text((x, y), key, font=kf, fill=key_col)
    kw, _ = text_size(draw, key, kf)
    draw.text((x + kw + gap, y), label, font=lf, fill=lbl_col)
    lw, _ = text_size(draw, label, lf)
    return x + kw + gap + lw


def fit_text(draw: ImageDraw.ImageDraw, text: str, fnt: ImageFont.FreeTypeFont, max_w: int) -> str:
    if text_size(draw, text, fnt)[0] <= max_w:
        return text
    ell = "…"
    lo, hi = 0, len(text)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if text_size(draw, text[:mid].rstrip() + ell, fnt)[0] <= max_w:
            lo = mid
        else:
            hi = mid - 1
    return text[:lo].rstrip() + ell if lo > 0 else ell


def wrap_lines(
    draw: ImageDraw.ImageDraw,
    text: str,
    fnt: ImageFont.FreeTypeFont,
    max_w: int,
    max_lines: Optional[int] = None,
) -> List[str]:
    lines: List[str] = []
    for para in text.split("\n"):
        if not para:
            lines.append("")
            continue
        words = para.split(" ")
        cur = ""
        for w in words:
            cand = f"{cur} {w}".strip() if cur else w
            if text_size(draw, cand, fnt)[0] <= max_w:
                cur = cand
            else:
                if cur:
                    lines.append(cur)
                if text_size(draw, w, fnt)[0] <= max_w:
                    cur = w
                else:
                    part = ""
                    for ch in w:
                        if text_size(draw, part + ch, fnt)[0] <= max_w:
                            part += ch
                        else:
                            if part:
                                lines.append(part)
                            part = ch
                    cur = part
        if cur:
            lines.append(cur)
    if max_lines is not None and len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = fit_text(draw, lines[-1] + "…", fnt, max_w)
    return lines


def vgrad(img: Image.Image, box: Tuple[int, int, int, int], top: Tuple[int, int, int], bot: Tuple[int, int, int]) -> None:
    x0, y0, x1, y1 = box
    draw = ImageDraw.Draw(img)
    h = max(1, y1 - y0 - 1)
    for y in range(y0, y1):
        t = (y - y0) / h
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        draw.line([(x0, y), (x1 - 1, y)], fill=c)


def make_album_art(size: int = 240) -> Image.Image:
    """Procedural geometric album artwork (original, no third-party assets)."""
    art = Image.new("RGB", (size, size), (14, 30, 44))
    vgrad(art, (0, 0, size, size), (17, 46, 68), (11, 20, 34))
    draw = ImageDraw.Draw(art, "RGBA")

    draw.polygon(
        [(0, int(size * 0.62)), (size, int(size * 0.25)), (size, int(size * 0.68)), (0, size)],
        fill=(32, 92, 118, 115),
    )
    draw.polygon(
        [(0, int(size * 0.78)), (size, int(size * 0.56)), (size, size), (0, size)],
        fill=(18, 42, 62, 190),
    )

    cx, cy, r = int(size * 0.54), int(size * 0.43), int(size * 0.24)
    for i in range(6, 0, -1):
        rr = r + i * 4
        alpha = int(16 * (7 - i) / 6)
        draw.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=(240, 152, 90, alpha))

    sun = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sun)
    for y in range(cy - r, cy + r + 1):
        dy = y - cy
        if abs(dy) > r:
            continue
        dx = int(math.sqrt(max(0, r * r - dy * dy)))
        t = (dy + r) / max(1, 2 * r)
        col = (
            int(250 - 38 * t),
            int(196 - 102 * t),
            int(114 - 22 * t),
            255,
        )
        if t > 0.68 and ((y - (cy - r)) % 7) < 2:
            continue
        sd.line([(cx - dx, y), (cx + dx, y)], fill=col)
    art.paste(sun, (0, 0), sun)

    hy = int(size * 0.66)
    draw.polygon(
        [
            (0, hy + 10),
            (int(size * 0.22), hy - 18),
            (int(size * 0.44), hy + 4),
            (int(size * 0.68), hy - 22),
            (int(size * 0.88), hy - 4),
            (size, hy - 14),
            (size, size),
            (0, size),
        ],
        fill=(12, 24, 38, 235),
    )
    draw.line([(0, hy + 10), (size, hy + 10)], fill=(108, 200, 235, 110), width=1)
    for idx in range(5):
        ry = hy + 18 + idx * 10
        rw = int(size * (0.28 - idx * 0.04))
        draw.line([(cx - rw, ry), (cx + rw, ry)], fill=(238, 166, 108, 120 - idx * 18), width=2)

    return art


ALBUM_ART_240 = make_album_art(240)
ALBUM_ART_104 = ALBUM_ART_240.resize((104, 104), Image.Resampling.LANCZOS)


def make_scrim_bg() -> Image.Image:
    """Blurred cover background matching Scrim's blurred_cover + dominant dark scrim."""
    scaled = ALBUM_ART_240.resize((W, H), Image.Resampling.BILINEAR)
    blurred = scaled.filter(ImageFilter.GaussianBlur(radius=18))
    overlay = Image.new("RGBA", (W, H), (8, 18, 26, 148))
    comp = Image.alpha_composite(blurred.convert("RGBA"), overlay)
    return comp.convert("RGB")


SCRIM_BG = make_scrim_bg()


def dim_image(img: Image.Image, dim_alpha: int = 105) -> Image.Image:
    """Apply native sheet backdrop dimming (~35-40% UI_COLOR_DIM)."""
    ov = Image.new("RGBA", img.size, (0, 0, 0, dim_alpha))
    return Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")


def draw_chevron(draw: ImageDraw.ImageDraw, x: int, y: int, color: Tuple[int, int, int], size: int = 5) -> None:
    pts = [(x, y - size), (x + size, y), (x, y + size)]
    draw.line(pts, fill=color, width=2)


def draw_tri_left(draw: ImageDraw.ImageDraw, x: int, cy: int, color: Tuple[int, int, int], w: int = 6, h: int = 8) -> None:
    draw.polygon([(x, cy), (x + w, cy - h // 2), (x + w, cy + h // 2)], fill=color)


def draw_tri_right(draw: ImageDraw.ImageDraw, x: int, cy: int, color: Tuple[int, int, int], w: int = 6, h: int = 8) -> None:
    draw.polygon([(x + w, cy), (x, cy - h // 2), (x, cy + h // 2)], fill=color)


def draw_reply_glyph(draw: ImageDraw.ImageDraw, x: int, cy: int, color: Tuple[int, int, int]) -> None:
    """Crisp vector corner-return arrow for quick reply presets."""
    draw.line([(x, cy - 4), (x, cy + 1), (x + 7, cy + 1)], fill=color, width=2)
    draw.polygon([(x + 9, cy + 1), (x + 5, cy - 2), (x + 5, cy + 4)], fill=color)


def draw_pencil_glyph(draw: ImageDraw.ImageDraw, cx: int, cy: int, color: Tuple[int, int, int]) -> None:
    """Crisp vector pencil glyph inside option badge box."""
    draw.line([(cx - 3, cy + 3), (cx + 3, cy - 3)], fill=color, width=2)
    draw.polygon([(cx - 4, cy + 4), (cx - 4, cy + 1), (cx - 1, cy + 4)], fill=color)


def draw_play_icon(draw: ImageDraw.ImageDraw, x: int, y: int, color: Tuple[int, int, int], h: int = 10) -> None:
    draw.polygon([(x, y), (x, y + h), (x + int(h * 0.85), y + h // 2)], fill=color)


def draw_battery(draw: ImageDraw.ImageDraw, x: int, y: int, fruit: bool, pct: int = 82) -> None:
    if fruit:
        draw.rounded_rectangle((x, y, x + 22, y + 11), radius=2, fill=(235, 240, 236), outline=(88, 96, 104), width=1)
        draw.rectangle((x + 22, y + 3, x + 24, y + 8), fill=(88, 96, 104))
        fill_w = max(2, int(18 * pct / 100))
        draw.rectangle((x + 2, y + 2, x + 2 + fill_w, y + 5), fill=(116, 208, 96))
        draw.rectangle((x + 2, y + 6, x + 2 + fill_w, y + 9), fill=(66, 164, 52))
    else:
        fg = (224, 238, 246)
        draw.rounded_rectangle((x, y, x + 21, y + 11), radius=2, outline=fg, width=1)
        draw.rectangle((x + 21, y + 3, x + 23, y + 8), fill=fg)
        fill_w = max(2, int(17 * pct / 100))
        draw.rectangle((x + 2, y + 2, x + 2 + fill_w, y + 9), fill=fg)


def draw_status_dot(
    img: Image.Image,
    cx: int,
    cy: int,
    state: str,
    radius: int = 5,
    halo: Tuple[int, int, int] = (255, 255, 255),
) -> None:
    """Draw anti-aliased status dot at 2x supersampling for crisp edges."""
    scale = 2
    box = (radius + 4) * 2
    patch = Image.new("RGBA", (box * scale, box * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(patch)
    c = (box * scale) // 2
    r = radius * scale
    hr = (radius + 1.5) * scale

    if state == "b":
        d.ellipse((c - hr, c - hr, c + hr, c + hr), fill=halo + (235,))
        d.ellipse((c - r, c - r, c + r, c + r), fill=DOT_RED + (255,))
    elif state == "f":
        d.ellipse((c - hr, c - hr, c + hr, c + hr), fill=halo + (235,))
        d.ellipse((c - r, c - r, c + r, c + r), fill=DOT_FAIL + (255,))
        d.rectangle((c - 1.5, c - r * 0.58, c + 1.5, c + r * 0.12), fill=(255, 255, 255, 255))
        d.ellipse((c - 1.6, c + r * 0.28, c + 1.6, c + r * 0.62), fill=(255, 255, 255, 255))
    elif state == "w":
        d.ellipse((c - hr, c - hr, c + hr, c + hr), fill=halo + (235,))
        d.ellipse((c - r, c - r, c + r, c + r), fill=DOT_GREEN + (255,))
    elif state == "d":
        d.ellipse((c - hr, c - hr, c + hr, c + hr), fill=halo + (235,))
        d.ellipse((c - r, c - r, c + r, c + r), fill=DOT_TEAL + (255,))
    elif state == "i":
        d.ellipse((c - hr, c - hr, c + hr, c + hr), fill=halo + (210,))
        d.ellipse((c - r, c - r, c + r, c + r), outline=DOT_GREEN + (255,), width=int(2.2 * scale))
    elif state == "u":
        sr = int(r * 0.65)
        shr = sr + scale
        d.ellipse((c - shr, c - shr, c + shr, c + shr), fill=halo + (210,))
        d.ellipse((c - sr, c - sr, c + sr, c + sr), fill=DOT_GREY + (255,))

    down = patch.resize((box, box), Image.Resampling.LANCZOS)
    img.paste(down, (cx - box // 2, cy - box // 2), down)


class Theme:
    def __init__(self, kind: str) -> None:
        self.kind = kind
        self.fruit = kind == "fruit-company"
        self.scrim = kind == "scrim"
        if self.fruit:
            self.status_h = 26
            self.list_x = 0
            self.list_y = 26
            self.row_h = 29
            self.bg = (255, 255, 255)
            self.fg = (24, 28, 34)
            self.muted = (102, 112, 128)
            self.faint = (156, 164, 176)
            self.divider = (228, 232, 238)
            self.accent = (28, 108, 218)
            self.sel_top = (76, 157, 248)
            self.sel_bot = (24, 106, 216)
            self.sel_edge = (20, 88, 184)
            self.sel_text = (255, 255, 255)
            self.sel_muted = (218, 235, 255)
            self.notes_bg = (241, 244, 248)
            self.notes_border = (210, 216, 226)
            self.sheet_bg = (248, 249, 252)
            self.sheet_border = (176, 184, 198)
            self.chip_bg = (254, 242, 224)
            self.chip_fg = (176, 102, 10)
            self.alert_tint = (253, 242, 244)
            self.alert_fg = (182, 18, 52)
            self.yes_bg = (232, 248, 236)
            self.yes_border = (48, 152, 74)
            self.yes_fg = (24, 110, 46)
            self.no_bg = (253, 236, 238)
            self.no_border = (206, 42, 64)
            self.no_fg = (168, 22, 44)
            self.keybar_bg = (236, 239, 244)
            self.keybar_border = (208, 214, 224)
            self.code_bg = (240, 243, 247)
            self.toast_bg = (252, 253, 255)
            self.toast_border = (174, 184, 198)
        else:
            self.status_h = 35
            self.list_x = 10
            self.list_y = 42
            self.row_h = 29
            self.bg = (12, 24, 34)
            self.fg = (255, 255, 255)
            self.muted = (166, 200, 216)
            self.faint = (118, 146, 162)
            self.divider = (44, 72, 88)
            self.accent = (108, 204, 238)
            self.sel_top = self.accent
            self.sel_bot = self.accent
            self.sel_edge = self.accent
            self.sel_text = (255, 255, 255)
            self.sel_muted = (210, 232, 242)
            self.notes_bg = (10, 22, 32)
            self.notes_border = (42, 74, 92)
            self.sheet_bg = (18, 22, 26)
            self.sheet_border = (98, 108, 118)
            self.chip_bg = (70, 50, 18)
            self.chip_fg = (255, 196, 92)
            self.alert_tint = (56, 20, 28)
            self.alert_fg = (255, 134, 148)
            self.yes_bg = (22, 52, 34)
            self.yes_border = (112, 216, 136)
            self.yes_fg = (156, 238, 172)
            self.no_bg = (58, 22, 26)
            self.no_border = (242, 102, 114)
            self.no_fg = (255, 152, 160)
            self.keybar_bg = (28, 32, 36)
            self.keybar_border = (62, 70, 78)
            self.code_bg = (26, 32, 38)
            self.toast_bg = (18, 26, 34)
            self.toast_border = (84, 114, 132)

    def canvas(self) -> Image.Image:
        if self.fruit:
            return Image.new("RGB", (W, H), self.bg)
        return SCRIM_BG.copy()

    def draw_statusbar(
        self,
        img: Image.Image,
        title: str,
        clock: str = "9:41 AM",
        playing: bool = True,
        agent_badge: Optional[Tuple[str, Optional[int]]] = None,
    ) -> None:
        draw = ImageDraw.Draw(img)
        if self.fruit:
            vgrad(img, (0, 0, W, self.status_h - 1), (245, 246, 248), (200, 205, 212))
            draw.line([(0, 0), (W - 1, 0)], fill=(252, 253, 254))
            draw.line([(0, self.status_h - 1), (W - 1, self.status_h - 1)], fill=(154, 161, 170))
            title_f = font_bold(13)
            meta_f = font_med(11)
            fg = (24, 28, 34)
            draw.text((8, 5), title, font=title_f, fill=fg)
            bx = W - 32
            draw_battery(draw, bx, 7, fruit=True, pct=84)
            rx = bx - 8
            if playing:
                rx -= 10
                draw_play_icon(draw, rx, 8, (30, 104, 210), h=9)
                rx -= 7
            if agent_badge is not None:
                st, cnt = agent_badge
                if cnt is not None:
                    cw, _ = text_size(draw, str(cnt), font_bold(11))
                    rx -= cw + 2
                    draw.text((rx + 1, 6), str(cnt), font=font_bold(11), fill=DOT_RED if st == "b" else fg)
                rx -= 11
                draw_status_dot(img, rx + 5, 12, st, radius=4, halo=(244, 246, 248))
                rx -= 6
            cw, _ = text_size(draw, clock, meta_f)
            cx = (W - cw) // 2
            tw, _ = text_size(draw, title, title_f)
            if cx < 8 + tw + 12 or cx + cw > rx - 6:
                cx = rx - cw - 4
            draw.text((cx, 6), clock, font=meta_f, fill=(56, 62, 72))
        else:
            draw.rectangle((0, 0, W, self.status_h), fill=(22, 36, 48))
            draw.line([(0, self.status_h - 1), (W - 1, self.status_h - 1)], fill=(38, 60, 76))
            title_f = font_bold(14)
            meta_f = font_reg(12)
            fg = (232, 244, 250)
            draw.text((10, 8), title, font=title_f, fill=fg)
            bx = W - 31
            draw_battery(draw, bx, 12, fruit=False, pct=84)
            rx = bx - 8
            if playing:
                rx -= 11
                draw_play_icon(draw, rx, 12, fg, h=10)
                rx -= 8
            if agent_badge is not None:
                st, cnt = agent_badge
                if cnt is not None:
                    cw, _ = text_size(draw, str(cnt), font_bold(12))
                    rx -= cw + 2
                    draw.text((rx + 1, 10), str(cnt), font=font_bold(12), fill=(255, 138, 152) if st == "b" else fg)
                rx -= 12
                draw_status_dot(img, rx + 5, 17, st, radius=4, halo=(22, 36, 48))
                rx -= 7
            cw, _ = text_size(draw, clock, meta_f)
            cx = (W - cw) // 2
            tw, _ = text_size(draw, title, title_f)
            if cx < 10 + tw + 14 or cx + cw > rx - 8:
                cx = rx - cw - 6
            draw.text((cx, 10), clock, font=meta_f, fill=(198, 224, 236))

    def draw_list_row(
        self,
        img: Image.Image,
        y: int,
        h: int,
        label: str,
        right: str = "",
        dot: Optional[str] = None,
        selected: bool = False,
        chevron: bool = False,
        badge_char: Optional[str] = None,
        alert_row: bool = False,
        reply_row: bool = False,
        x0: int = 0,
        width: int = W,
        sheet_mode: bool = False,
    ) -> None:
        draw = ImageDraw.Draw(img)
        lbl_f = font_med(14) if (selected and self.fruit) else font_reg(14)
        right_f = font_reg(12)

        if self.fruit:
            if selected:
                vgrad(img, (x0, y, x0 + width, y + h - 1), self.sel_top, self.sel_bot)
                draw.line([(x0, y + h - 1), (x0 + width - 1, y + h - 1)], fill=self.sel_edge)
                fg = self.sel_text
                rfg = self.sel_muted
            else:
                if alert_row:
                    draw.rectangle((x0, y, x0 + width, y + h - 1), fill=self.alert_tint)
                draw.line([(x0 + 8, y + h - 1), (x0 + width - 8, y + h - 1)], fill=self.divider)
                fg = self.alert_fg if alert_row else self.fg
                rfg = self.alert_fg if alert_row else self.muted

            tx = x0 + 10
            if dot is not None:
                draw_status_dot(img, x0 + 17, y + h // 2, dot, radius=5, halo=(255, 255, 255))
                tx = x0 + 30
            elif badge_char is not None:
                if alert_row:
                    draw.ellipse((x0 + 10, y + h // 2 - 7, x0 + 24, y + h // 2 + 7), fill=DOT_RED if not selected else (255, 255, 255))
                    draw_centered(draw, x0 + 17, y + h // 2, "!", font_bold(12), (255, 255, 255) if not selected else DOT_RED)
                else:
                    box_col = (255, 255, 255) if selected else (142, 150, 164)
                    draw.rounded_rectangle((x0 + 10, y + h // 2 - 8, x0 + 26, y + h // 2 + 8), radius=3, outline=box_col, width=1)
                    if badge_char == "pencil":
                        draw_pencil_glyph(draw, x0 + 18, y + h // 2, fg)
                    else:
                        draw_centered(draw, x0 + 18, y + h // 2, badge_char, font_med(11), fg)
                tx = x0 + 33
            elif reply_row:
                draw_reply_glyph(draw, x0 + 11, y + h // 2, (255, 255, 255) if selected else self.accent)
                tx = x0 + 25

            rx = x0 + width - 10
            if chevron:
                draw_chevron(draw, rx - 5, y + h // 2, (255, 255, 255) if selected else (168, 176, 188), size=4)
                rx -= 12
            if right:
                rw, rh = text_size(draw, right, right_f)
                draw.text((rx - rw, y + (h - rh) // 2 - 1), right, font=right_f, fill=rfg)
                rx -= rw + 8

            fitted = fit_text(draw, label, lbl_f, max(20, rx - tx))
            _, lh = text_size(draw, fitted, lbl_f)
            draw.text((tx, y + (h - lh) // 2 - 1), fitted, font=lbl_f, fill=fg)
        else:
            lx = x0 if sheet_mode else self.list_x
            lw = width if sheet_mode else (W - 2 * self.list_x)
            if alert_row and not selected:
                ov = Image.new("RGBA", (lw, h - 2), (160, 26, 48, 72))
                img.paste(ov, (lx, y + 1), ov)
            fg = self.sel_text if selected else (self.alert_fg if alert_row else self.muted)
            rfg = self.sel_muted if selected else (self.alert_fg if alert_row else self.faint)

            icon_x = lx + 12
            bar_x = lx + 28
            tx = lx + 38
            if dot is None and badge_char is None:
                bar_x = lx + 6
                tx = lx + 16
                if reply_row:
                    draw_reply_glyph(draw, lx + 16, y + h // 2, self.accent if selected else self.faint)
                    tx = lx + 30

            if dot is not None:
                draw_status_dot(img, icon_x, y + h // 2, dot, radius=5, halo=(245, 250, 255))
            elif badge_char is not None:
                if alert_row:
                    draw.ellipse((icon_x - 7, y + h // 2 - 7, icon_x + 7, y + h // 2 + 7), fill=DOT_RED)
                    draw_centered(draw, icon_x, y + h // 2, "!", font_bold(12), (255, 255, 255))
                else:
                    bc = self.accent if selected else (112, 138, 152)
                    draw.rectangle((icon_x - 8, y + h // 2 - 8, icon_x + 8, y + h // 2 + 8), outline=bc, width=1)
                    if badge_char == "pencil":
                        draw_pencil_glyph(draw, icon_x, y + h // 2, fg)
                    else:
                        draw_centered(draw, icon_x, y + h // 2, badge_char, font_med(11), fg)

            if selected:
                draw.rounded_rectangle((bar_x, y + 3, bar_x + 3, y + h - 4), radius=1, fill=self.accent)

            rx = lx + lw - 8
            if chevron:
                draw_chevron(draw, rx - 5, y + h // 2, self.sel_text if selected else self.muted, size=4)
                rx -= 12
            if right:
                rw, rh = text_size(draw, right, right_f)
                draw.text((rx - rw, y + (h - rh) // 2 - 1), right, font=right_f, fill=rfg)
                rx -= rw + 8

            fitted = fit_text(draw, label, lbl_f, max(20, rx - tx))
            _, lh = text_size(draw, fitted, lbl_f)
            draw.text((tx, y + (h - lh) // 2 - 1), fitted, font=lbl_f, fill=fg)


# ---------------------------------------------------------------------------
# Screen Generators (10 key screens x 2 themes)
# ---------------------------------------------------------------------------

def render_01_projects(t: Theme) -> Image.Image:
    img = t.canvas()
    t.draw_statusbar(img, "Agents", clock="9:41 AM", playing=True, agent_badge=("b", 1))
    y = t.list_y
    rows = [
        ("Approval: git commit…", "podbox", None, False, False, "!", True),
        ("podbox", "1 ask", "b", True, True, None, False),
        ("astrapod-desktop", "2 run", "w", False, True, None, False),
        ("t3-code", "1 err", "f", False, True, None, False),
        ("audio-dsp-bench", "done", "d", False, True, None, False),
        ("homelab-infra", "", "i", False, True, None, False),
    ]
    for label, right, dot, sel, chev, badge, alert in rows:
        t.draw_list_row(img, y, t.row_h, label, right=right, dot=dot, selected=sel, chevron=chev, badge_char=badge, alert_row=alert)
        y += t.row_h

    # Subtle bottom page hint (LEFT -> Usage, Hold SEL -> New thread)
    draw = ImageDraw.Draw(img)
    hy = H - 20
    if t.fruit:
        draw.line([(0, hy), (W, hy)], fill=t.keybar_border)
        draw.rectangle((0, hy + 1, W, H), fill=t.keybar_bg)
    x = 26
    draw_tri_left(draw, x, hy + 10, t.accent, w=5, h=7)
    draw.text((x + 9, hy + 3), "Usage", font=font_reg(11), fill=t.muted)
    draw.text((104, hy + 3), "·", font=font_bold(11), fill=t.faint)
    draw_key_pair(draw, 122, hy + 3, "Hold SEL", "New thread", t.accent, t.muted)
    return img


def render_02_threads(t: Theme) -> Image.Image:
    img = t.canvas()
    title = "podbox" if t.fruit else "podbox · Active ›"
    t.draw_statusbar(img, title, clock="9:41 AM", playing=True, agent_badge=("b", 1))
    draw = ImageDraw.Draw(img)
    y = t.list_y

    if t.fruit:
        bar_h = 21
        draw.rectangle((0, y, W, y + bar_h), fill=(242, 244, 248))
        draw.line([(0, y + bar_h - 1), (W, y + bar_h - 1)], fill=t.divider)
        draw.rounded_rectangle((8, y + 2, 60, y + bar_h - 3), radius=3, fill=t.accent)
        draw_centered(draw, 34, y + bar_h // 2, "Active", font_bold(11), (255, 255, 255))
        draw_centered(draw, 78, y + bar_h // 2, "All", font_med(11), t.muted)
        # Clean vector L/R hint
        draw_tri_left(draw, W - 36, y + bar_h // 2, t.faint, w=5, h=6)
        draw.text((W - 27, y + 3), "/", font=font_reg(11), fill=t.faint)
        draw_tri_right(draw, W - 18, y + bar_h // 2, t.faint, w=5, h=6)
        y += bar_h

    row_h = 27 if t.fruit else 28
    threads = [
        ("T3 remote UI design", "run 2m", "w", True),
        ("Fix PodLink USB stall", "ask", "b", False),
        ("ALAC seek regression", "err", "f", False),
        ("Scrim notes contrast", "done 8m", "d", False),
    ]
    for name, right, dot, sel in threads:
        t.draw_list_row(img, y, row_h, name, right=right, dot=dot, selected=sel, chevron=True)
        y += row_h

    # Bottom preview strip (notes panel)
    nh = 62
    ny = H - nh
    if t.fruit:
        draw.rectangle((0, ny, W, H), fill=t.notes_bg)
        draw.line([(0, ny), (W, ny)], fill=t.notes_border)
        px = 10
        pw = W - 20
    else:
        ov = Image.new("RGBA", (W, nh), (8, 18, 26, 224))
        img.paste(ov, (0, ny), ov)
        draw = ImageDraw.Draw(img)
        draw.line([(0, ny), (W, ny)], fill=t.notes_border)
        px = 12
        pw = W - 24

    meta_f = font_med(11)
    body_f = font_reg(13)
    meta = "claude-4.5 · t3/t3-remote-design"
    draw.text((px, ny + 6), fit_text(draw, meta, meta_f, pw), font=meta_f, fill=t.accent if t.scrim else t.muted)
    summary = "Drafting button map and 320x240 theme tokens for Fruit Company and Scrim."
    for idx, ln in enumerate(wrap_lines(draw, summary, body_f, pw, max_lines=2)):
        draw.text((px, ny + 22 + idx * 17), ln, font=body_f, fill=t.fg)
    return img


def render_03_detail(t: Theme) -> Image.Image:
    img = t.canvas()
    t.draw_statusbar(img, "T3 remote UI design", clock="9:42 AM", playing=True, agent_badge=("w", None))
    draw = ImageDraw.Draw(img)

    px = 10
    pw = W - 24
    y = t.status_h + 4

    # Compact thread header: model · branch + state/age
    meta_f = font_med(11)
    state_txt = "working 2m"
    sw, _ = text_size(draw, state_txt, meta_f)
    draw.text((W - 10 - sw, y), state_txt, font=meta_f, fill=DOT_GREEN if t.fruit else (126, 224, 140))
    draw_status_dot(img, W - 10 - sw - 9, y + 7, "w", radius=4, halo=(255, 255, 255) if t.fruit else (18, 32, 44))
    draw = ImageDraw.Draw(img)
    draw.text((px, y), fit_text(draw, "claude-4.5 · t3/t3-remote-design", meta_f, W - 20 - sw - 20), font=meta_f, fill=t.muted)
    y += 17
    draw.line([(px, y), (W - 10, y)], fill=t.divider)
    y += 6

    # Live activity strip with crisp green vector dot
    act_f = font_med(12)
    act_col = (26, 128, 52) if t.fruit else (126, 224, 140)
    draw.ellipse((px, y + 5, px + 6, y + 11), fill=act_col)
    draw.text((px + 11, y), fit_text(draw, "Running: python3 render_mockups.py", act_f, pw - 11), font=act_f, fill=act_col)
    y += 19

    # Summary-first wrapped assistant message
    body_f = font_reg(13)
    msg = (
        "Finished the screen hierarchy and click-wheel button map. "
        "LEFT/RIGHT now page long replies in Detail, while SELECT opens the Actions sheet.\n"
        "Next: generating 320x240 Fruit Company and Scrim mockups."
    )
    body_top = y
    for para_i, para in enumerate(msg.split("\n")):
        for ln in wrap_lines(draw, para, body_f, pw):
            draw.text((px, y), ln, font=body_f, fill=t.fg)
            y += 17
        if para_i == 0:
            y += 4

    # Recent tool steps (muted, below message)
    y += 2
    step_f = font_reg(11)
    steps = [
        "› Wrote docs/podbox/design/t3-remote/t3-remote-design.md",
        "› Verified HerdRemote v1 + tc 1 frame budgets",
    ]
    for st in steps:
        if y + 14 < H - 22:
            draw.text((px, y), fit_text(draw, st, step_f, pw), font=step_f, fill=t.muted)
            y += 15

    # Thin right-edge scroll track + thumb
    track_x = W - 6
    track_top = body_top
    track_bot = H - 24
    draw.rectangle((track_x, track_top, track_x + 2, track_bot), fill=t.divider)
    thumb_h = max(18, (track_bot - track_top) // 3)
    draw.rectangle((track_x, track_top, track_x + 2, track_top + thumb_h), fill=t.accent)

    # Bottom bar: page indicator & actions affordance
    bar_y = H - 20
    draw.line([(0, bar_y), (W, bar_y)], fill=t.keybar_border)
    draw.rectangle((0, bar_y + 1, W, H), fill=t.keybar_bg)
    x_after = draw_key_pair(draw, 10, bar_y + 3, "SEL", "Actions", t.accent, t.muted)
    px_lr = x_after + 18
    draw_tri_left(draw, px_lr, bar_y + 10, t.accent, w=5, h=7)
    draw.text((px_lr + 7, bar_y + 3), "/", font=font_reg(11), fill=t.faint)
    draw_tri_right(draw, px_lr + 14, bar_y + 10, t.accent, w=5, h=7)
    draw.text((px_lr + 24, bar_y + 3), "Page", font=font_reg(11), fill=t.muted)
    pg_txt = "1/3"
    pw2, _ = text_size(draw, pg_txt, font_med(11))
    draw.text((W - 10 - pw2, bar_y + 3), pg_txt, font=font_med(11), fill=t.muted)
    return img


def render_04_approval(t: Theme) -> Image.Image:
    base = render_02_threads(t)
    img = dim_image(base, dim_alpha=120 if t.fruit else 145)
    draw = ImageDraw.Draw(img)

    mx, my, mw, mh = 8, 12, W - 16, H - 24
    radius = 6 if t.fruit else 0
    if radius:
        draw.rounded_rectangle((mx, my, mx + mw, my + mh), radius=radius, fill=t.sheet_bg, outline=t.sheet_border, width=1)
        # Bottom keybar band cleanly respecting rounded card corners
        ky = my + mh - 24
        draw.rounded_rectangle((mx + 1, ky - 4, mx + mw - 1, my + mh - 1), radius=radius - 1, fill=t.keybar_bg)
        draw.rectangle((mx + 1, ky - 4, mx + mw - 1, ky + 1), fill=t.sheet_bg)
        draw.line([(mx + 1, ky), (mx + mw - 1, ky)], fill=t.keybar_border)
        # Top crimson accent stripe (inset from rounded corners)
        draw.rectangle((mx + 4, my + 1, mx + mw - 4, my + 4), fill=DOT_RED)
    else:
        draw.rectangle((mx, my, mx + mw, my + mh), fill=t.sheet_bg, outline=t.sheet_border, width=2)
        ky = my + mh - 24
        draw.line([(mx + 2, ky), (mx + mw - 2, ky)], fill=t.keybar_border)
        draw.rectangle((mx + 2, ky + 1, mx + mw - 2, my + mh - 2), fill=t.keybar_bg)
        draw.rectangle((mx + 2, my + 2, mx + mw - 2, my + 5), fill=DOT_RED)

    # Header
    hx, hy = mx + 12, my + 12
    draw_status_dot(img, hx + 4, hy + 8, "b", radius=4, halo=t.sheet_bg)
    draw = ImageDraw.Draw(img)
    draw.text((hx + 14, hy), "T3 · podbox", font=font_bold(13), fill=t.fg)
    th_txt = "Fix PodLink stall"
    tw, _ = text_size(draw, th_txt, font_reg(11))
    draw.text((mx + mw - 12 - tw, hy + 2), th_txt, font=font_reg(11), fill=t.muted)
    draw.line([(hx, hy + 22), (mx + mw - 12, hy + 22)], fill=t.divider)

    # Action category chip
    cy = hy + 29
    chip = "RUN COMMAND"
    cw, _ = text_size(draw, chip, font_bold(10))
    draw.rounded_rectangle((hx, cy, hx + cw + 12, cy + 17), radius=3 if t.fruit else 0, fill=t.chip_bg)
    draw.text((hx + 6, cy + 2), chip, font=font_bold(10), fill=t.chip_fg)

    # Command preview box
    cmd_y = cy + 23
    cmd_h = 46
    draw.rounded_rectangle((hx, cmd_y, mx + mw - 12, cmd_y + cmd_h), radius=3 if t.fruit else 0, fill=t.code_bg, outline=t.divider, width=1)
    cmd_f = font_mono(11)
    cmd_txt = 'git commit -m "fix: guard USB EP0 stall on reconnect"'
    for i, ln in enumerate(wrap_lines(draw, cmd_txt, cmd_f, mw - 36, max_lines=2)):
        draw.text((hx + 7, cmd_y + 7 + i * 16), ln, font=cmd_f, fill=t.fg)

    # Two large Allow / Deny tactile buttons with crisp vector triangles
    by = cmd_y + cmd_h + 12
    bh = 40
    bw = (mw - 24 - 10) // 2
    btn_f = font_bold(14)

    # Left: Allow
    draw.rounded_rectangle((hx, by, hx + bw, by + bh), radius=4 if t.fruit else 0, fill=t.yes_bg, outline=t.yes_border, width=2)
    aw, _ = text_size(draw, "Allow", btn_f)
    total_w = 7 + 8 + aw
    ax0 = hx + (bw - total_w) // 2
    draw_tri_left(draw, ax0, by + bh // 2, t.yes_fg, w=7, h=10)
    draw_centered(draw, ax0 + 7 + 8 + aw // 2, by + bh // 2, "Allow", btn_f, t.yes_fg)

    # Right: Deny
    rx = hx + bw + 10
    draw.rounded_rectangle((rx, by, rx + bw, by + bh), radius=4 if t.fruit else 0, fill=t.no_bg, outline=t.no_border, width=2)
    dw, _ = text_size(draw, "Deny", btn_f)
    total_d = dw + 8 + 7
    dx0 = rx + (bw - total_d) // 2
    draw_centered(draw, dx0 + dw // 2, by + bh // 2, "Deny", btn_f, t.no_fg)
    draw_tri_right(draw, dx0 + dw + 8, by + bh // 2, t.no_fg, w=7, h=10)

    # Bottom key bar labels (dynamically spaced)
    draw_key_pair(draw, mx + 14, ky + 4, "SELECT", "All options", t.accent, t.muted)
    draw_key_pair(draw, mx + mw - 92, ky + 4, "MENU", "Later", t.accent, t.muted)
    return img


def render_05_question(t: Theme) -> Image.Image:
    img = t.canvas()
    t.draw_statusbar(img, "Q 1/2 · podbox", clock="9:42 AM", playing=True, agent_badge=("b", 1))
    draw = ImageDraw.Draw(img)

    y = t.status_h + 6
    px = 10
    pw = W - 20
    qf = font_med(13)
    qtxt = "Where should background notifications store unread thread events?"
    for ln in wrap_lines(draw, qtxt, qf, pw, max_lines=2):
        draw.text((px, y), ln, font=qf, fill=t.fg)
        y += 18
    y += 4
    draw.line([(px, y), (W - px, y)], fill=t.divider)
    y += 4

    opts = [
        ("1", "Static 16-byte summary", "Recommended", True),
        ("2", "Core allocator ring", "4 KB", False),
        ("3", "Persist to config.cfg", "", False),
        ("pencil", "Other (custom reply)…", "", False),
    ]
    rh = 30
    for badge, label, right, sel in opts:
        t.draw_list_row(img, y, rh, label, right=right, selected=sel, badge_char=badge)
        y += rh

    ky = H - 20
    draw.line([(0, ky), (W, ky)], fill=t.keybar_border)
    draw.rectangle((0, ky + 1, W, H), fill=t.keybar_bg)
    x1 = draw_key_pair(draw, 10, ky + 3, "SEL", "Choose", t.accent, t.muted)
    draw_key_pair(draw, x1 + 16, ky + 3, "Wheel", "Scroll", t.accent, t.muted)
    draw_key_pair(draw, W - 76, ky + 3, "MENU", "Back", t.accent, t.muted)
    return img


def render_06_actions(t: Theme) -> Image.Image:
    base = render_03_detail(t)
    img = dim_image(base, dim_alpha=100 if t.fruit else 130)
    draw = ImageDraw.Draw(img)

    sh = 186
    sy = H - sh
    mx = 6 if t.fruit else 0
    mw = W - 2 * mx
    if t.fruit:
        draw.rounded_rectangle((mx, sy, mx + mw, H - 4), radius=6, fill=t.sheet_bg, outline=t.sheet_border, width=1)
    else:
        draw.rectangle((0, sy, W, H), fill=t.sheet_bg, outline=t.sheet_border, width=2)

    draw.text((mx + 12, sy + 7), "Actions", font=font_bold(13), fill=t.fg)
    st = "working · 2m"
    sw, _ = text_size(draw, st, font_reg(11))
    draw.text((mx + mw - 12 - sw, sy + 8), st, font=font_reg(11), fill=DOT_GREEN if t.fruit else (126, 224, 140))
    draw.line([(mx + 2, sy + 26), (mx + mw - 2, sy + 26)], fill=t.divider)

    y = sy + 28
    rh = 25
    controls = [
        ("Interrupt turn", "confirm", False),
        ("Compose reply…", "queue", False),
        ("Open on desktop", "", False),
    ]
    for label, right, sel in controls:
        t.draw_list_row(img, y, rh, label, right=right, selected=sel, x0=mx + 2, width=mw - 4, sheet_mode=True)
        y += rh

    draw.line([(mx + 12, y + 1), (mx + mw - 12, y + 1)], fill=t.divider)
    y += 4

    replies = [
        ("Run unit tests", "", True),
        ("Continue", "", False),
        ("Summarize changes", "", False),
    ]
    for label, right, sel in replies:
        t.draw_list_row(img, y, rh, label, right=right, selected=sel, reply_row=True, x0=mx + 2, width=mw - 4, sheet_mode=True)
        y += rh
    return img


def render_07_confirm(t: Theme) -> Image.Image:
    base = render_06_actions(t)
    img = dim_image(base, dim_alpha=95 if t.fruit else 115)
    draw = ImageDraw.Draw(img)

    mw, mh = 276, 118
    mx = (W - mw) // 2
    my = (H - mh) // 2
    if t.fruit:
        draw.rounded_rectangle((mx, my, mx + mw, my + mh), radius=6, fill=t.sheet_bg, outline=t.sheet_border, width=1)
        draw.rectangle((mx + 4, my + 1, mx + mw - 4, my + 4), fill=DOT_FAIL)
    else:
        draw.rectangle((mx, my, mx + mw, my + mh), fill=t.sheet_bg, outline=t.sheet_border, width=2)
        draw.rectangle((mx + 2, my + 2, mx + mw - 2, my + 5), fill=DOT_FAIL)

    draw.text((mx + 14, my + 14), "Interrupt turn?", font=font_bold(15), fill=t.fg)
    draw.text((mx + 14, my + 36), fit_text(draw, "T3 remote UI design · podbox", font_reg(12), mw - 28), font=font_reg(12), fill=t.muted)

    by = my + mh - 46
    bh = 34
    bw = (mw - 28 - 10) // 2
    draw.rounded_rectangle((mx + 14, by, mx + 14 + bw, by + bh), radius=4 if t.fruit else 0, fill=t.code_bg, outline=t.sheet_border, width=1)
    draw_centered(draw, mx + 14 + bw // 2, by + bh // 2, "Cancel", font_med(13), t.fg)

    rx = mx + 14 + bw + 10
    draw.rounded_rectangle((rx, by, rx + bw, by + bh), radius=4 if t.fruit else 0, fill=DOT_RED, outline=(255, 255, 255) if t.scrim else (164, 14, 44), width=1)
    draw_centered(draw, rx + bw // 2, by + bh // 2, "Interrupt", font_bold(13), (255, 255, 255))
    return img


def render_08_compose_review(t: Theme) -> Image.Image:
    base = render_03_detail(t)
    img = dim_image(base, dim_alpha=110 if t.fruit else 135)
    draw = ImageDraw.Draw(img)

    mx, my, mw, mh = 10, 14, W - 20, H - 28
    ky = my + mh - 24
    if t.fruit:
        draw.rounded_rectangle((mx, my, mx + mw, my + mh), radius=6, fill=t.sheet_bg, outline=t.sheet_border, width=1)
        draw.rounded_rectangle((mx + 1, ky - 4, mx + mw - 1, my + mh - 1), radius=5, fill=t.keybar_bg)
        draw.rectangle((mx + 1, ky - 4, mx + mw - 1, ky + 1), fill=t.sheet_bg)
        draw.line([(mx + 1, ky), (mx + mw - 1, ky)], fill=t.keybar_border)
        draw.rectangle((mx + 4, my + 1, mx + mw - 4, my + 4), fill=t.accent)
    else:
        draw.rectangle((mx, my, mx + mw, my + mh), fill=t.sheet_bg, outline=t.sheet_border, width=2)
        draw.line([(mx + 2, ky), (mx + mw - 2, ky)], fill=t.keybar_border)
        draw.rectangle((mx + 2, ky + 1, mx + mw - 2, my + mh - 2), fill=t.keybar_bg)
        draw.rectangle((mx + 2, my + 2, mx + mw - 2, my + 5), fill=t.accent)

    hx, hy = mx + 12, my + 12
    draw.text((hx, hy), "Review reply", font=font_bold(13), fill=t.fg)
    cnt = "48 chars"
    cw, _ = text_size(draw, cnt, font_reg(11))
    draw.text((mx + mw - 12 - cw, hy + 2), cnt, font=font_reg(11), fill=t.muted)

    sub = "To: T3 remote UI design (podbox)"
    draw.text((hx, hy + 20), fit_text(draw, sub, font_reg(11), mw - 24), font=font_reg(11), fill=t.muted)

    bx, by, bw, bh = hx, hy + 38, mw - 24, 96
    draw.rounded_rectangle((bx, by, bx + bw, by + bh), radius=3 if t.fruit else 0, fill=t.code_bg, outline=t.accent, width=1)
    bf = font_reg(13)
    draft = "USE STATIC 16-BYTE SUMMARY AND KEEP AUDIO BUFFER UNTOUCHED"
    lines = wrap_lines(draw, draft, bf, bw - 14, max_lines=5)
    for i, ln in enumerate(lines):
        draw.text((bx + 7, by + 7 + i * 17), ln, font=bf, fill=t.fg)
    if lines:
        last_w, _ = text_size(draw, lines[-1], bf)
        cy = by + 7 + (len(lines) - 1) * 17
        draw.line([(bx + 9 + last_w, cy + 1), (bx + 9 + last_w, cy + 15)], fill=t.accent, width=2)

    ey = by + bh + 9
    draw_key_pair(draw, hx, ey, "RIGHT", "Edit with wheel", t.accent, t.muted)

    draw_key_pair(draw, mx + 14, ky + 4, "SELECT", "Send", t.accent, t.fg)
    draw_key_pair(draw, mx + mw - 98, ky + 4, "MENU", "Cancel", t.accent, t.muted)
    return img


def render_09_now_playing_toast(t: Theme) -> Image.Image:
    if t.fruit:
        img = Image.new("RGB", (W, H), (255, 255, 255))
        t.draw_statusbar(img, "Now Playing", clock="9:43 AM", playing=True, agent_badge=("b", 1))
        draw = ImageDraw.Draw(img)

        draw.text((12, 33), "4 of 12", font=font_reg(11), fill=t.muted)
        ax, ay = 14, 52
        draw.rectangle((ax - 1, ay - 1, ax + 104, ay + 104), outline=(188, 194, 204), width=1)
        img.paste(ALBUM_ART_104, (ax, ay))

        mx = 130
        draw.text((mx, 66), "Midnight Signal", font=font_bold(15), fill=t.fg)
        draw.text((mx, 88), "Helios Drift", font=font_med(13), fill=t.muted)
        draw.text((mx, 107), "AstraPod Sessions", font=font_reg(12), fill=t.faint)

        py = 172
        px0, px1 = 44, W - 46
        draw.rounded_rectangle((px0, py, px1, py + 7), radius=3, fill=(224, 229, 236), outline=(184, 192, 204), width=1)
        fill_x = px0 + int((px1 - px0) * 0.44)
        vgrad(img, (px0 + 1, py + 1, fill_x, py + 6), (88, 168, 250), (24, 108, 218))
        draw.text((12, py - 3), "1:42", font=font_reg(11), fill=t.muted)
        draw.text((W - 38, py - 3), "-2:18", font=font_reg(11), fill=t.muted)

        tx, ty, tw, th = 8, 190, W - 16, 44
        draw.rounded_rectangle((tx, ty, tx + tw, ty + th), radius=5, fill=t.toast_bg, outline=t.toast_border, width=1)
        draw.rectangle((tx + 1, ty + 4, tx + 4, ty + th - 4), fill=DOT_RED)
        draw.text((tx + 11, ty + 5), "Approval needed · podbox", font=font_bold(12), fill=t.fg)
        draw.text((tx + 11, ty + 23), fit_text(draw, 'git commit -m "fix: guard USB…"', font_reg(11), tw - 82), font=font_reg(11), fill=t.muted)
        pill = "SEL+PLAY"
        pw, _ = text_size(draw, pill, font_bold(10))
        draw.rounded_rectangle((tx + tw - pw - 14, ty + 21, tx + tw - 6, ty + 37), radius=3, fill=t.accent)
        draw_centered(draw, tx + tw - pw // 2 - 10, ty + 29, pill, font_bold(10), (255, 255, 255))
        return img
    else:
        img = SCRIM_BG.copy()
        img.paste(ALBUM_ART_240, (80, 0))
        draw = ImageDraw.Draw(img, "RGBA")

        draw_play_icon(draw, 8, 8, (255, 255, 255), h=11)
        draw.rounded_rectangle((36, 5, 74, 22), radius=8, fill=(18, 28, 38, 215), outline=(210, 15, 57, 235), width=1)
        draw_status_dot(img, 47, 13, "b", radius=3, halo=(18, 28, 38))
        draw = ImageDraw.Draw(img, "RGBA")
        draw_centered(draw, 61, 13, "1", font_bold(11), (255, 168, 178))

        draw.rectangle((0, 77, 240, 140), fill=(8, 18, 26, 175))
        draw.text((8, 83), "Midnight Signal", font=font_med(21), fill=(255, 255, 255))
        draw.text((8, 112), "Helios Drift", font=font_reg(14), fill=(186, 216, 232))

        draw.rectangle((0, 140, 240, 145), fill=(255, 255, 255, 96))
        draw.rectangle((0, 140, 106, 145), fill=(108, 204, 238, 255))
        draw.text((8, 150), "04", font=font_reg(11), fill=(208, 232, 244))
        draw.text((44, 150), "2:18", font=font_reg(11), fill=(208, 232, 244))

        bars = [16, 28, 34, 22, 30, 18, 12, 8]
        for i, bh in enumerate(bars):
            bx = i * 10
            draw.rectangle((bx + 1, 240 - bh, bx + 8, 240), fill=(255, 255, 255, 230))
            draw.rectangle((bx + 1, 240 - bh - 4, bx + 8, 240 - bh - 2), fill=(108, 204, 238, 230))

        tx, ty, tw, th = 8, 26, W - 16, 44
        draw.rectangle((tx, ty, tx + tw, ty + th), fill=(14, 22, 30, 238), outline=(108, 204, 238, 210), width=1)
        draw.rectangle((tx + 1, ty + 1, tx + 4, ty + th - 1), fill=DOT_RED + (255,))
        draw.text((tx + 11, ty + 5), "Approval needed · podbox", font=font_bold(12), fill=(255, 255, 255))
        draw.text((tx + 11, ty + 23), fit_text(draw, 'git commit -m "fix: guard USB…"', font_reg(11), tw - 84), font=font_reg(11), fill=(182, 212, 228))
        pill = "SEL+PLAY"
        pw, _ = text_size(draw, pill, font_bold(10))
        draw.rectangle((tx + tw - pw - 14, ty + 21, tx + tw - 6, ty + 37), fill=(108, 204, 238, 255))
        draw_centered(draw, tx + tw - pw // 2 - 10, ty + 29, pill, font_bold(10), (10, 20, 28))
        return img.convert("RGB")


def render_10_offline(t: Theme) -> Image.Image:
    img = t.canvas()
    t.draw_statusbar(img, "Agents", clock="9:44 AM", playing=False, agent_badge=None)
    draw = ImageDraw.Draw(img)

    py = t.status_h + 10
    pill_txt = "USB: Connected  ·  HerdRemote: Ready"
    pf = font_reg(11)
    pw, _ = text_size(draw, pill_txt, pf)
    px = (W - pw - 20) // 2
    draw.rounded_rectangle((px, py, px + pw + 20, py + 20), radius=10 if t.fruit else 0, fill=t.code_bg, outline=t.divider, width=1)
    draw_centered(draw, W // 2, py + 10, pill_txt, pf, t.muted)

    cx, cy = W // 2, 108
    r = 22
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=t.code_bg, outline=t.sheet_border, width=2)
    ic = t.muted
    draw.rounded_rectangle((cx - 12, cy - 8, cx + 12, cy + 8), radius=2, outline=ic, width=2)
    draw.line([(cx - 8, cy - 3), (cx - 4, cy), (cx - 8, cy + 3)], fill=ic, width=2)
    draw.line([(cx - 1, cy + 3), (cx + 5, cy + 3)], fill=ic, width=2)
    draw_status_dot(img, cx + 15, cy + 15, "f", radius=5, halo=t.bg if t.fruit else (16, 28, 38))
    draw = ImageDraw.Draw(img)

    draw_centered(draw, W // 2, 150, "T3 Code is not running", font_bold(15), t.fg)
    draw_centered(draw, W // 2, 172, "Launch T3 Code on your computer", font_reg(12), t.muted)
    draw_centered(draw, W // 2, 190, "Projects will appear automatically.", font_reg(11), t.faint)

    ky = H - 22
    draw.line([(0, ky), (W, ky)], fill=t.keybar_border)
    draw.rectangle((0, ky + 1, W, H), fill=t.keybar_bg)
    draw_key_pair(draw, 14, ky + 4, "SELECT", "Retry", t.accent, t.muted)
    draw_key_pair(draw, 124, ky + 4, "LEFT", "Usage", t.accent, t.muted)
    draw_key_pair(draw, W - 78, ky + 4, "MENU", "Exit", t.accent, t.muted)
    return img


SCREENS = [
    ("01_projects.png", render_01_projects),
    ("02_threads.png", render_02_threads),
    ("03_detail.png", render_03_detail),
    ("04_approval.png", render_04_approval),
    ("05_question.png", render_05_question),
    ("06_actions.png", render_06_actions),
    ("07_confirm.png", render_07_confirm),
    ("08_compose_review.png", render_08_compose_review),
    ("09_now_playing_toast.png", render_09_now_playing_toast),
    ("10_offline.png", render_10_offline),
]


def main() -> None:
    OUT_FRUIT.mkdir(parents=True, exist_ok=True)
    OUT_SCRIM.mkdir(parents=True, exist_ok=True)

    for theme_name, out_dir in [("fruit-company", OUT_FRUIT), ("scrim", OUT_SCRIM)]:
        theme = Theme(theme_name)
        for filename, fn in SCREENS:
            img = fn(theme)
            assert img.size == (W, H), f"{theme_name}/{filename} got {img.size}, expected {(W, H)}"
            img.save(out_dir / filename, format="PNG", optimize=True)
            print(f"wrote {out_dir / filename} ({img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
