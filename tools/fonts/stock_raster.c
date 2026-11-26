/* The stock firmware's glyph rasterizer, for stock.py. GPL v2+.
 *
 * Built against FreeType 2.3.5, the release the stock firmware carries: the
 * later grey rasterizer flattens curves differently and moves the coverage
 * of curved edges by up to 11 levels. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

struct stock_face {
    FT_Library library;
    FT_Face face;
};

void stock_close(struct stock_face *src)
{
    if (src->face)
        FT_Done_Face(src->face);
    if (src->library)
        FT_Done_FreeType(src->library);
    free(src);
}

/* The size is FT_Set_Char_Size's, in 1/64 px at 72 dpi. */
struct stock_face *stock_open(const char *file, int size)
{
    struct stock_face *src = calloc(1, sizeof(*src));
    if (!src)
        return NULL;
    if (FT_Init_FreeType(&src->library) ||
        FT_New_Face(src->library, file, 0, &src->face) ||
        FT_Set_Char_Size(src->face, 0, size, 72, 72))
    {
        stock_close(src);
        return NULL;
    }
    return src;
}

/* Unhinted outline, 8-bit grey coverage at a whole-pixel origin. The metrics
 * are left and top bearing, width, height and the advance in 1/64 px. */
int stock_glyph(struct stock_face *src, uint32_t cp, int metrics[5],
                uint8_t *pixels, unsigned capacity)
{
    FT_Face face = src->face;
    unsigned index = FT_Get_Char_Index(face, cp);
    if (!index || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
        return -1;
    FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE ||
        FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
        return -1;
    FT_Bitmap *bitmap = &slot->bitmap;
    unsigned w = bitmap->width, h = bitmap->rows;
    if (w * h > capacity || (w && h && bitmap->pixel_mode != FT_PIXEL_MODE_GRAY))
        return -1;
    metrics[0] = slot->bitmap_left;
    metrics[1] = slot->bitmap_top;
    metrics[2] = w;
    metrics[3] = h;
    metrics[4] = slot->advance.x;
    for (unsigned y = 0; y < h && w; ++y)
        memcpy(pixels + y * w, bitmap->buffer + (ptrdiff_t)y * bitmap->pitch, w);
    return w * h;
}
