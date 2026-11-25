#include "theme_art.h"

const struct ui_art *ui_theme_art(const struct ui_theme *theme,
                                  enum ui_art_role role)
{
    if (!theme || theme->style[UI_STYLE_ART] != UI_ART_PLASTIC)
        return NULL;
    return &plastic_art[role];
}

void ui_art_draw(const struct ui_art *art, int x, int y, int width)
{
    if (width > art->width)
        width = art->width;
    if (width <= 0)
        return;
    x += art->x;
    y += art->y;
    if (!art->pixels)
        lcd_alpha_bitmap_part(art->alpha, 0, 0, art->width, x, y, width,
                              art->height);
    else if (!art->alpha)
        lcd_bitmap_part(art->pixels, 0, 0, art->width, x, y, width,
                        art->height);
    else
        lcd_alpha_bitmap_part_img(art->pixels, art->alpha, 0, 0, x, y, width,
                                  art->height, art->width, art->width);
}
