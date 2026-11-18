/***************************************************************************
 * Original code from RockBox
 * was: apps/logfdisp.c
 * Copyright (C) 2005 Daniel Stenberg
 * GNU General Public License (version 2+)
 *
 * On-device viewer and dumper for the circular logf() buffer. Only built
 * when ROCKBOX_HAS_LOGF is set.
 ****************************************************************************/
#include "config.h"
#include "logfile.h"

#ifdef ROCKBOX_HAS_LOGF
#include <file.h>
#include <timefuncs.h>
#include <string.h>
#include <kernel.h>
#include "rbpaths.h"
#include "input/action.h"

#include <lcd.h>
#include <font.h>
#include "widgets/menu.h"
#include "logf.h"
#include "settings/settings.h"
#include "log_viewer.h"
#include "widgets/splash.h"
#include "system/strutil.h"
#include "system/shutdown.h"   /* default_event_handler */
#include "draw/viewport.h"
#include "ui/retained.h"
#include "ui/scene.h"
int compute_nb_lines(int w, struct font* font)
{
    int i, nb_lines;
    int cur_x, delta_x;

    if(logfindex == 0 && !logfwrap)
        return 0;

    if(logfwrap)
        i = logfindex;
    else
        i = 0;
    
    cur_x = 0;
    nb_lines = 0;
        
    do {
        if(logfbuffer[i] == '\0')
        {
            cur_x = 0;
            nb_lines++;
        }
        else
        {
            /* does character fit on this line ? */
            delta_x = font_get_width(font, logfbuffer[i]);
            
            if(cur_x + delta_x > w)
            {
                cur_x = 0;
                nb_lines++;
            }
            
            /* update pointer */
            cur_x += delta_x;
        }

        i++;
        if(i >= MAX_LOGF_SIZE)
            i = 0;
    } while(i != logfindex);
    
    return nb_lines;
}

struct log_view {
    int line, font, line_height;
};

static void paint_log(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct log_view *log = view->data;
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = log->font,
        .drawmode = DRMODE_SOLID, .buffer = &lcd_framebuffer_default,
        .fg_pattern = LCD_BLACK, .bg_pattern = LCD_WHITE,
    };
    struct viewport *last = lcd_set_viewport(&vp);
    lcd_clear_viewport();

    if (logfindex != 0 || logfwrap)
    {
        int next = logfwrap ? logfindex : 0;
        int line = 0, x = 0, y = 0;
        struct font *font = font_get(log->font);

        do
        {
            unsigned char byte = logfbuffer[next];
            int width = byte ? font_get_width(font, byte) : 0;
            if (!byte || x + width > vp.width)
            {
                if (line >= log->line)
                    y += log->line_height;
                line++;
                x = 0;
            }
            if (byte && line >= log->line && y < vp.height)
            {
                unsigned char text[] = {byte, 0};
                lcd_putsxy(x, y, text);
            }
            x += width;
            if (++next == MAX_LOGF_SIZE)
                next = 0;
        } while (next != logfindex && y < vp.height);
    }
    lcd_set_viewport(last);
}

static const struct ui_view_ops log_ops = { .paint = paint_log };

bool log_viewer_show(void)
{
    const struct ui_rect bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    struct log_view log = { .font = screens[SCREEN_MAIN].getuifont() };
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    bool to_root = false;

    font_getstringsize("A", NULL, &log.line_height, log.font);
    log.line = compute_nb_lines(LCD_WIDTH, font_get(log.font))
               - LCD_HEIGHT / log.line_height - 1;
    ui_scene_open_page(&scene, "log-viewer-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    ui_view_init(&view, bounds, &log_ops, &log);
    ui_page_init(&page, &view);

    while (true)
    {
        if (log.line < 0)
            log.line = 0;
        ui_page_invalidate(&page, bounds);
        ui_page_draw(&page);

        int action = get_action(CONTEXT_STD, HZ);
        if (action == ACTION_STD_CANCEL)
            break;
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            log.line++;
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            log.line--;
        else if (action == ACTION_STD_OK)
            log.line = 0;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            to_root = true;
            break;
        }
    }
    ui_scene_close_modal(&scene, !to_root, "log-viewer-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return to_root;
}

bool log_viewer_dump(void)
{
    int fd;

    splash(HZ, "Log File Dumped");

    /* nothing to print ? */
    if(logfindex == 0 && !logfwrap)
        /* nothing is logged just yet */
        return false;

    logfenabled = false;

    fd = logfile_open(ROCKBOX_DIR "/logf.txt");
    if(-1 != fd) {
        int i;

        if(logfwrap)
            i = logfindex;
        else
            i = 0;

        do {
            if(logfbuffer[i]=='\0')
                logfile_printf(fd, "\n");
            else
                logfile_printf(fd, "%c", logfbuffer[i]);

            i++;
            if(i >= MAX_LOGF_SIZE)
                i = 0;
        } while(i != logfindex);

        logfile_close(fd);
    }

    logfenabled = true;

    return false;
}

#endif /* ROCKBOX_HAS_LOGF */
