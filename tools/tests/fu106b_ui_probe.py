#!/usr/bin/env python3
"""Instrument an isolated iPod 6G SDL source copy for deterministic frame checks.

Run this on a disposable copy, then configure and build the SDL simulator.
Open a list with at least three visible rows, and flip an album with a cover
visible on each side. The simulator log contains the probe results.
"""
import argparse
from pathlib import Path


def replace(source, old, new):
    assert source.count(old) == 1, old
    return source.replace(old, new)


PUSH_PROBE = r"""
#ifdef SIMULATOR
static void probe_push(const struct ui_scene *scene)
{
    static bool checked;
    static fb_data back[PAGE_PIXELS], front[PAGE_PIXELS], out[PAGE_PIXELS];
    if (checked || !scene->target_front || scene->row_count < 3)
        return;
    checked = true;
    memset16(front, LCD_WHITE, PAGE_PIXELS);
    unsigned frames = (24 + 3 * (scene->row_count - 1) + 1) / 2;
    unsigned bad = scene->tween.frames != frames, total = 0;
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        compose_push(scene, out, back, front, frame, LCD_WIDTH);
        for (int row = 0; row < scene->row_count; ++row)
        {
            int y = scene->row_y + row * scene->row_pitch;
            if (y < scene->push_top || y >= LCD_HEIGHT)
                continue;
            int age = MAX(0, (int)frame * 2 - row * 3);
            unsigned ink = MIN(age * 256 / 12, 256);
            unsigned remain = 1024 - MIN(1024, age * 1024 / 24);
            unsigned shift = MAX(2, ((PUSH_ROW_OFFSET * (remain * remain * remain / 1048576) + 1023)
                                      / 1024) & ~1);
            fb_data white = LCD_WHITE, black = 0, expected;
            ui_blit_mix(&expected, &white, &black, 1, ink, 256 - ink);
            for (unsigned x = 0; x < LCD_WIDTH; ++x)
            {
                fb_data pixel = age >= 24 || x >= shift ? expected : 0;
                bad += out[y * LCD_WIDTH + x] != pixel;
                total++;
            }
        }
    }
    fprintf(stderr, "FU106B push frames=%u rows=%d pixels=%u mismatches=%u\n",
            frames, scene->row_count, total, bad);
}
#endif
"""

FLIP_PROBE = r"""
#ifdef SIMULATOR
/* Render each nominal pose twice, starting with the same framebuffer. The
 * reference blends the channels independently with the same quantization. */
static void probe_flip(FACE_PARAMETER)
{
    static bool checked[2];
    bool opening = panel.to > panel.from;
    if (!panel.moving || checked[opening])
        return;
    checked[opening] = true;
    static pix_t before[LCD_WIDTH * LCD_HEIGHT], expected[LCD_WIDTH * LCD_HEIGHT];
    unsigned total = 0, bad = 0, empty = 0;
    unsigned frames = panel.first + panel.second + 1;
    memcpy(before, LCD_BUF, sizeof(before));
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        int pos = flip_position(frame);
        probe_reference = true;
        render_album_flip(pos FACE_ARGUMENT);
        memcpy(expected, LCD_BUF, sizeof(expected));
        memcpy(LCD_BUF, before, sizeof(before));
        probe_reference = false;
        render_album_flip(pos FACE_ARGUMENT);
        unsigned visible[2] = {0, 0};
        for (int y = pf_vp_y; y < pf_vp_y + pf_height; ++y)
            for (int x = 0; x < LCD_WIDTH; ++x)
            {
                /* Side regions leave the central face out of the comparison. */
                if (x >= LCD_WIDTH / 4 && x < LCD_WIDTH * 3 / 4)
                    continue;
                int k = y * LCD_WIDTH + x;
                visible[x >= LCD_WIDTH / 2] += expected[k] != pf_bg_color;
                bad += LCD_BUF[k] != expected[k];
                total++;
            }
        empty += !visible[0] || !visible[1];
        memcpy(LCD_BUF, before, sizeof(before));
    }
    fprintf(stderr, "FU106B flip %s frames=%u pixels=%u mismatches=%u empty=%u\n",
            opening ? "open" : "close", frames, total, bad, empty);
}
#endif
"""

SCALAR_FADE = r"""
#ifdef SIMULATOR
static bool probe_reference;
static unsigned probe_weight;
static pix_t probe_bg;

static pix_t scalar_fade(pix_t c, unsigned light)
{
    unsigned r = (((c >> 11) & 31) * light + ((probe_bg >> 11) & 31) * probe_weight) / 256;
    unsigned g = (((c >> 5) & 63) * light + ((probe_bg >> 5) & 63) * probe_weight) / 256;
    unsigned b = ((c & 31) * light + (probe_bg & 31) * probe_weight) / 256;
    return (r << 11) | (g << 5) | b;
}
#endif
"""


def instrument(root):
    path = root / "apps-ipod/ui/scene.c"
    scene = path.read_text()
    scene = replace(scene, "#include <string.h>", "#include <string.h>\n#ifdef SIMULATOR\n"
                    "#include <stdio.h>\n#endif")
    scene = replace(scene, "static void complete_transition(", PUSH_PROBE +
                    "\nstatic void complete_transition(")
    scene = replace(scene, "    scene->capturing = false;\n    scene->tween.start_clock",
                    "#ifdef SIMULATOR\n    if (scene->staggered)\n        probe_push(scene);\n"
                    "#endif\n    scene->capturing = false;\n    scene->tween.start_clock")
    path.write_text(scene)

    path = root / "apps-ipod/screens/covers/carousel.c"
    covers = path.read_text()
    covers = replace(covers, "static inline void fade_prepare(", SCALAR_FADE +
                     "\nstatic inline void fade_prepare(")
    covers = replace(covers, "    f->a  = a * pf_neighbor_light / 256;",
                     "    f->a  = a * pf_neighbor_light / 256;\n#ifdef SIMULATOR\n"
                     "    probe_weight = (256 - a) * pf_neighbor_light / 256;\n"
                     "    probe_bg = pf_bg_color;\n#endif")
    covers = replace(covers, "unsigned int fg)\n{",
                     "unsigned int fg)\n{\n#ifdef SIMULATOR\n"
                     "    if (probe_reference)\n        return scalar_fade(c, fa);\n#endif")
    covers = replace(covers, "    unsigned int bg = *pixel;",
                     "    unsigned int bg = *pixel;\n#ifdef SIMULATOR\n"
                     "    unsigned saved_weight = probe_weight;\n    pix_t saved_bg = probe_bg;\n"
                     "    probe_weight = 256 - coverage;\n    probe_bg = bg;\n#endif")
    covers = replace(covers, "                        (bg & 0x07e0) * (256 - coverage));",
                     "                        (bg & 0x07e0) * (256 - coverage));\n#ifdef SIMULATOR\n"
                     "    probe_weight = saved_weight;\n    probe_bg = saved_bg;\n#endif")
    uses_texture = "render_album_flip(int position, const pix_t *texture)" in covers
    probe = FLIP_PROBE.replace("FACE_PARAMETER", "const pix_t *texture" if uses_texture else "void")
    probe = probe.replace(" FACE_ARGUMENT", ", texture" if uses_texture else "")
    covers = replace(covers, "static void draw_album_flip(void)", probe +
                     "\nstatic void draw_album_flip(void)")
    args = ", texture" if uses_texture else ""
    render = f"    render_album_flip(panel.position{args});"
    covers = replace(covers, render, "#ifdef SIMULATOR\n"
                     f"    probe_flip({'texture' if uses_texture else ''});\n"
                     "#endif\n" + render)
    if not uses_texture:
        covers = replace(covers, "static void paint_album_panel(void)\n{",
                         "static void paint_album_panel(void)\n{\n#ifdef SIMULATOR\n"
                         '    fprintf(stderr, "FU106B panel text flat=%d\\n", panel.position == FLIP_OPEN);\n'
                         "#endif")
    path.write_text(covers)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disposable_source", type=Path)
    instrument(parser.parse_args().disposable_source)
