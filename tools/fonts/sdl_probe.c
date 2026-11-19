/* Included by check.py in a scratch build, after the SDL firmware starts. */
#include "bidi.h"
#include "draw/viewport.h"
#include "system/strutil.h"

static void font_probe(void)
{
    int input = open(ROCKBOX_DIR "/font-probe-input.txt", O_RDONLY);
    if (input < 0)
        return;
    int trace = open(ROCKBOX_DIR "/font-probe-output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    int id = font_load_ex(FONT_DIR "/18-noto-sans.pbf", 0, 256);
    if (id < 0)
        id = font_load_ex(FONT_DIR "/18-noto-sans.fnt", 0, 256);
    if (id < 0 || trace < 0)
        abort();

    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = vp.y = 0;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = 0;
    lcd_set_viewport(&vp);
    lcd_setfont(id);
    lcd_set_background(LCD_RGBPACK(18, 22, 30));
    lcd_set_foreground(LCD_WHITE);
    lcd_set_drawmode(DRMODE_FG);
    lcd_clear_viewport();

    char text[2048];
    int lines = 0;
    struct font *font = font_get(id);
    fdprintf(trace, "font %d %d %d\n", font->height, font->ascent, (int)font->buffer_size);
    while (read_line(input, text, sizeof(text)) > 0)
    {
        if (!*text)
            continue;
        unsigned elapsed = USEC_TIMER;
        lcd_putsxy(3, 3 + (lines % 9) * 26, text);
        elapsed = USEC_TIMER - elapsed;
        int width = lcd_getstringsize(text, NULL, NULL);
        font_lock(id, true);
        font = font_get(id);
#ifdef HAVE_PODBOX_FONT
        struct font_run run = {0};
#endif
        int final_pen = 0;
        fdprintf(trace, "%d %d %u", lines, width, elapsed);
        for (const ucschar_t *cp = bidi_l2v((const unsigned char *)text, 1); *cp; ++cp)
        {
#ifdef HAVE_PODBOX_FONT
            struct font_glyph glyph;
            font_run_next(font, &run, *cp, &glyph);
            fdprintf(trace, " %x,%d,%d,%u,%d", (unsigned)*cp, glyph.origin,
                     glyph.x, glyph.phase, glyph.missing);
            final_pen = run.pen;
#else
            fdprintf(trace, " %x,%d,%d,0,0", (unsigned)*cp, final_pen, final_pen / 64);
            final_pen += font_get_width(font, *cp) * 64;
#endif
        }
        fdprintf(trace, " :%d\n", final_pen);
        font_lock(id, false);

        if (++lines == 9)
        {
            extern SDL_Surface *lcd_surface;
            lcd_update();
            if (SDL_SaveBMP(lcd_surface, "font-probe.bmp") != 0)
                abort();
        }
    }

    const unsigned repeat = 10000;
    const unsigned char *hot = (const unsigned char *)"AVATAR Toyota WAVE 0123";
    lcd_putsxy(3, 3, hot);
    unsigned time = USEC_TIMER;
    for (unsigned i = 0; i < repeat; ++i)
        lcd_putsxy(3, 3, hot);
    fdprintf(trace, "warm %u %u\n", repeat * (unsigned)strlen((const char *)hot), (unsigned)(USEC_TIMER - time));
    close(trace);
    close(input);
    exit(0);
}
