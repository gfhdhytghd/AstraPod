/* Host-side FreeType rasterizer and OpenType layout reference. GPL v2+. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_ADVANCES_H
#include <hb.h>
#include <hb-ft.h>

#define HINTING (FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT)

struct source {
    FT_Library library;
    FT_Face face;
    hb_font_t *font;
    hb_buffer_t *buffer;
};

void source_close(struct source *src)
{
    if (src->buffer) hb_buffer_destroy(src->buffer);
    if (src->font) hb_font_destroy(src->font);
    if (src->face) FT_Done_Face(src->face);
    if (src->library) FT_Done_FreeType(src->library);
    free(src);
}

struct source *source_open(const char *file, int index, int px, int weight)
{
    struct source *src = calloc(1, sizeof(*src));
    if (!src)
        return NULL;
    if (FT_Init_FreeType(&src->library) ||
        FT_New_Face(src->library, file, index, &src->face))
        goto error;
    if (FT_HAS_MULTIPLE_MASTERS(src->face))
    {
        FT_Fixed value = (FT_Fixed)weight << 16;
        if (FT_Set_Var_Design_Coordinates(src->face, 1, &value))
            goto error;
    }
    if (FT_Set_Pixel_Sizes(src->face, 0, px))
        goto error;

    src->font = hb_ft_font_create_referenced(src->face);
    hb_ft_font_set_load_flags(src->font, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
    src->buffer = hb_buffer_create();
    return src;

error:
    source_close(src);
    return NULL;
}

int source_advance(struct source *src, uint32_t cp)
{
    FT_Fixed advance;
    if (FT_Get_Advance(src->face, FT_Get_Char_Index(src->face, cp),
                       FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP, &advance))
        return -1;
    return (advance + 512) >> 10;
}

static int raster(struct source *src, uint32_t cp, int phase,
                  int metrics[4], uint8_t *pixels, unsigned capacity,
                  FT_Render_Mode mode)
{
    FT_Face face = src->face;
    if (FT_Load_Char(face, cp, HINTING))
        return -1;
    FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return -1;
    FT_Outline_Translate(&slot->outline, phase, 0);
    if (FT_Render_Glyph(slot, mode))
        return -1;

    FT_Bitmap *bitmap = &slot->bitmap;
    unsigned w = bitmap->width, h = bitmap->rows;
    unsigned channels = mode == FT_RENDER_MODE_LCD ? 3 : 1;
    int pixel_mode = channels == 3 ? FT_PIXEL_MODE_LCD : FT_PIXEL_MODE_GRAY;
    if (w * h > capacity || w % channels ||
        (w && h && bitmap->pixel_mode != pixel_mode))
        return -1;
    metrics[0] = slot->bitmap_left;
    metrics[1] = slot->bitmap_top;
    metrics[2] = w / channels;
    metrics[3] = h;
    for (unsigned y = 0; y < h && w; ++y)
        memcpy(pixels + y * w, bitmap->buffer + (ptrdiff_t)y * bitmap->pitch, w);
    return w * h;
}

int source_raster(struct source *src, uint32_t cp, int phase,
                  int metrics[4], uint8_t *pixels, unsigned capacity)
{
    return raster(src, cp, phase, metrics, pixels, capacity, FT_RENDER_MODE_NORMAL);
}

int source_raster_lcd(struct source *src, uint32_t cp, int phase,
                      int metrics[4], uint8_t *pixels, unsigned capacity)
{
    /* FreeType's Harmony renderer applies its own color-fringe suppression
     * when the library was built without configurable LCD filtering. */
    FT_Error error = FT_Library_SetLcdFilter(src->library, FT_LCD_FILTER_DEFAULT);
    if (error && error != FT_Err_Unimplemented_Feature)
        return -1;
    return raster(src, cp, phase, metrics, pixels, capacity, FT_RENDER_MODE_LCD);
}

void source_metrics(struct source *src, int metrics[2])
{
    FT_Size_Metrics *m = &src->face->size->metrics;
    metrics[0] = (m->ascender + 32) >> 6;
    metrics[1] = (m->height + 32) >> 6;
}

/* Return the origins and final pen from the FreeType-backed OT shaper.
 * Contextual substitutions are disabled for this codepoint/pair format. */
int source_layout(struct source *src, const uint32_t *cp, unsigned n,
                  int script, int kerning, int *origins)
{
    static const hb_feature_t features[] = {
        {HB_TAG('l','i','g','a'), 0, 0, (unsigned)-1},
        {HB_TAG('c','l','i','g'), 0, 0, (unsigned)-1},
        {HB_TAG('c','a','l','t'), 0, 0, (unsigned)-1},
        {HB_TAG('l','o','c','l'), 0, 0, (unsigned)-1},
        {HB_TAG('c','c','m','p'), 0, 0, (unsigned)-1},
        {HB_TAG('m','a','r','k'), 0, 0, (unsigned)-1},
        {HB_TAG('m','k','m','k'), 0, 0, (unsigned)-1},
        {HB_TAG('k','e','r','n'), 0, 0, (unsigned)-1},
    };
    hb_buffer_t *buf = src->buffer;
    hb_buffer_reset(buf);
    hb_buffer_add_utf32(buf, cp, n, 0, n);
    hb_buffer_guess_segment_properties(buf);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    if (script)
        hb_buffer_set_script(buf, script);
    hb_shape(src->font, buf, features, sizeof(features) / sizeof(*features) - !!kerning);

    unsigned count;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
    if (count != n)
        return -1;
    const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, NULL);
    int pen = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        if (info[i].codepoint != FT_Get_Char_Index(src->face, cp[i]))
            return -1;
        origins[i] = pen + pos[i].x_offset;
        pen += pos[i].x_advance;
    }
    origins[n] = pen;
    return 0;
}
