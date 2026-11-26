/* 8-bit coverage glyphs of the PBF fonts. GPL v2+.
 *
 * Included from lcd-16bit-common.c. The stock iPod firmware's text is an
 * 8-bit grey coverage mask blended per channel over the background,
 * d * (255 - a) / 255 + s * a / 255 truncated, and the screen shows that
 * result truncated to RGB565. Here the destination is widened from RGB565,
 * blended at 8 bits and truncated once, so text over the stock art matches
 * the stock screen wherever the background's own low bits do not matter.
 * Text on stock's UI layer over its picture layer is blended by the LCD
 * controller, which widens the RGB565 picture with zero low bits; the layer
 * mode does the same. */

static uint32_t glyph8_palette[32];
static unsigned glyph8_palette_size;
static bool glyph8_layer;

void lcd_set_glyph_palette(const uint32_t *rgb, unsigned count)
{
    glyph8_palette_size = MIN(count, ARRAYLEN(glyph8_palette));
    memcpy(glyph8_palette, rgb, glyph8_palette_size * sizeof(*rgb));
}

void lcd_set_glyph_layer(bool layer)
{
    glyph8_layer = layer;
}

#if LCD_PIXELFORMAT == RGB565 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
/* The registered 24-bit colour that packs to the foreground, else the
 * foreground widened. */
static uint32_t glyph8_foreground(unsigned fg)
{
    for (unsigned i = 0; i < glyph8_palette_size; ++i)
    {
        uint32_t rgb = glyph8_palette[i];
        if (LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255) == fg)
            return rgb;
    }
    unsigned r = fg >> 11, g = (fg >> 5) & 63, b = fg & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static unsigned glyph8_mix(unsigned d, unsigned s, unsigned a)
{
    unsigned x = d * (255 - a) + s * a;
    return ((x + 1) * 257) >> 16; /* x / 255, exact for every x here */
}
#endif

/* The draw modes other than foreground and solid take the 4-bit blitter
 * one row at a time. */
static void glyph8_rows(const struct font_glyph *glyph, int skip, int x, int y,
                        int width)
{
    unsigned char row[128 + 8];
    for (int r = 0; r < glyph->height; ++r)
    {
        const unsigned char *src = glyph->bits + r * glyph->width + skip;
        memset(row, 0xff, sizeof(row));
        for (int i = 0; i < width; ++i)
        {
            unsigned alpha = 15 - (src[i] * 15 + 127) / 255;
            row[i >> 1] &= (i & 1) ? (alpha << 4) | 0x0f : alpha | 0xf0;
        }
        lcd_alpha_bitmap_part(row, 0, 0, width, x, y + r, width, 1);
    }
}

static void lcd_font_glyph8(const struct font_glyph *glyph, int skip, int x, int y)
{
    struct viewport *vp = lcd_current_viewport;
    int width = glyph->width - skip;
#if LCD_PIXELFORMAT == RGB565 && LCD_STRIDEFORMAT == HORIZONTAL_STRIDE
    if (vp->drawmode == DRMODE_FG || vp->drawmode == DRMODE_SOLID)
    {
        int src_x = skip, src_y = 0, height = glyph->height;
        if (!clip_viewport_rect(vp, &x, &y, &width, &height, &src_x, &src_y))
            return;

        uint32_t fg = glyph8_foreground(vp->fg_pattern);
        unsigned red = fg >> 16, green = (fg >> 8) & 255, blue = fg & 255;
        bool foreground_only = vp->drawmode == DRMODE_FG;
        fb_data *dst = FBADDR(x, y);
        for (int row = 0; row < height; ++row, dst += ROW_INC)
        {
            const unsigned char *src = glyph->bits + (src_y + row) * glyph->width + src_x;
            for (int col = 0; col < width; ++col)
            {
                unsigned a = src[col];
                if (!a && foreground_only)
                    continue;
                fb_data *pixel = dst + col;
                unsigned bg = foreground_only ? *pixel
                    : lcd_backdrop ? *PTR_ADD(pixel, lcd_backdrop_offset) : vp->bg_pattern;
                unsigned r = bg >> 11, g = (bg >> 5) & 63, b = bg & 31;
                if (glyph8_layer)
                {
                    r = glyph8_mix(r << 3, red, a);
                    g = glyph8_mix(g << 2, green, a);
                    b = glyph8_mix(b << 3, blue, a);
                }
                else
                {
                    r = glyph8_mix(r << 3 | r >> 2, red, a);
                    g = glyph8_mix(g << 2 | g >> 4, green, a);
                    b = glyph8_mix(b << 3 | b >> 2, blue, a);
                }
                *pixel = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
            }
        }
        return;
    }
#endif
    glyph8_rows(glyph, skip, x, y, width);
}
