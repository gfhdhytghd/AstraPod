#!/usr/bin/env python3
"""Stage AstraPod puzzle adapters. GPL-2.0-or-later."""
from pathlib import Path
import sys


def replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"expected one upstream match for {before!r}")
    return source.replace(before, after, 1)


def sudoku(source):
    source = replace(source, "    size_t n;\n", "    ssize_t n = -1;\n")
    source = replace(source, "    unsigned int i;\n    int valid=0;",
                     "    ssize_t i;\n    int valid=0;")
    source = replace(source, """    n=rb->read(fd,buf,500);
    if (n <= 0) {
        return(false);
    }
    rb->close(fd);""", """    off_t size = rb->ffilesize(fd);
    if (size > 0 && size <= (off_t)sizeof(buf))
        n = rb->read(fd, buf, size);
    if (rb->close(fd) < 0 || n <= 0 || n != size)
        return false;""")
    source = replace(source, """                if (valid) {
                    r++;
                    valid=0;
                }""", """                if (valid) {
                    if (c != SIZE)
                        return false;
                    r++;
                    valid=0;
                }""")
    source = replace(source, """                    state->possiblevals[r][d]
                        = (((buf[i]-'a') * 26 + buf[i+1]-'a')<<1);""",
                     """                    if (d >= SIZE)
                        return false;
                    state->possiblevals[r][d]
                        = (((buf[i]-'a') * 26 + buf[i+1]-'a')<<1);""")
    source = replace(source, "    /* Check that the board is valid",
                     """    if (r != SIZE && !(r == SIZE - 1 && c == SIZE))
        return false;

    /* Check that the board is valid""")

    source = replace(source, "    fd=rb->open(state->filename, O_WRONLY|O_CREAT, 0666);",
                     """    char temp[MAX_PATH];
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", state->filename);
    if (len < 0 || (size_t)len >= sizeof(temp))
        return false;

    bool valid = true;
    fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);""")
    source = replace(source, """            rb->write(fd,line,sizeof(line));
            if ((r==2) || (r==5)) {
                rb->write(fd,sep,sizeof(sep));
            }
        }
        /* Add a blank line at end */
        rb->write(fd,"\\r\\n",2);
        rb->close(fd);
        rb->reload_directory();""", """            valid = valid && rb->write(fd, line, sizeof(line)) == sizeof(line);
            if (r == 2 || r == 5)
                valid = valid && rb->write(fd, sep, sizeof(sep)) == sizeof(sep);
        }
        valid = valid && rb->write(fd, "\\r\\n", 2) == 2 && rb->file_sync(fd) == 0;
        if (rb->close(fd) < 0)
            valid = false;
        if (!valid || rb->rename(temp, state->filename) < 0) {
            rb->remove(temp);
            return false;
        }
        rb->reload_directory();""")
    source = replace(source, """        case SM_SAVE:
            save_sudoku(state);
            break;""", """        case SM_SAVE:
            if (!save_sudoku(state))
                rb->splash(2 * HZ, "Save failed");
            break;""")
    source = replace(source, """        case SM_QUIT:
            save_sudoku(state);
            break;""", """        case SM_QUIT:
            if (!save_sudoku(state))
                rb->splash(2 * HZ, "Save failed");
            break;""")
    source = replace(source, "            rb->kbd_input(state->filename,MAX_PATH, NULL);",
                     """            if (rb->kbd_input(state->filename, MAX_PATH, NULL) < 0)
                break;""")
    source = replace(source, """        if (!load_sudoku(&state,GAME_FILE)) {
            /* No previous game saved, use the default */
            default_state(&state);
        }""", """        if (!rb->file_exists(GAME_FILE)) {
            default_state(&state);
        } else if (!load_sudoku(&state, GAME_FILE)) {
            rb->splash(HZ * 2, "Cannot load saved game");
            return PLUGIN_ERROR;
        }""")
    source = replace(source, "    while(!exit) {", "    while (!exit && !rb->plugin_usb_taken()) {")
    return replace(source, "    return rc;\n}", "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : rc;\n}")


def fractal(source):
    return replace(source, "    *button = rb->button_get(false);",
                   """    *button = rb->button_get(false);
    if (IS_SYSEVENT(*button))
        return 1;""")


def reversi(source):
    source = replace(source, "    /* Initialize Font Width and height */",
                     """    /* Keep the legend inside the stock board margins. */
    rb->lcd_setfont(FONT_SYSFIXED);
    /* Initialize Font Width and height */""")
    source = replace(source, "    while (!exit && !quit_plugin) {",
                     "    while (!exit && !quit_plugin && !rb->plugin_usb_taken()) {")
    source = replace(source, "        if(cur_strategy->is_robot && !game_finished) {",
                   """        if(cur_strategy->is_robot && !game_finished) {
            int event = rb->button_get(false);
            if (IS_SYSEVENT(event) &&
                rb->default_event_handler(event) == SYS_USB_CONNECTED)
                return PLUGIN_USB_CONNECTED;
            rb->yield();""")
    source = replace(source, "        button = rb->button_get(true);",
                     """        button = rb->button_get(true);
        if (IS_SYSEVENT(button)) {
            if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                return PLUGIN_USB_CONNECTED;
            continue;
        }""")
    return replace(source, "    return PLUGIN_OK;\n}",
                   "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n}")


def convert(source, before, after):
    if before not in source:
        raise ValueError(f"expected upstream matches for {before!r}")
    return source.replace(before, after)


def solo(source):
    source = replace(source, "    if (params->c < 2)",
                     "    if (params->c < 2 || params->r < 1)")
    return replace(source, """    if ((params->c * params->r) > 31)
        return "Unable to support more than 31 distinct symbols in a puzzle";""",
                   """    if ((params->c * params->r) > 16)
        return "This player supports Solo boards up to 16 by 16";""")


def tatham_allocator(source):
    start = source.index("/* We don't load as an overlay anymore")
    end = source.index("void *smalloc", start)
    source = source[:start] + "bool audiobuf_available = true;\n\n" + source[end:]
    source = replace(source, """        if(grab_audiobuf())
            return smalloc(size);
""", "")
    return replace(source, """        if(grab_audiobuf())
            return srealloc(p, size);
""", "")


def tatham_frontend(source):
    source = replace(source, "#define FONT_CACHING\n", "")
    source = replace(source, "static int thread = -1;\n", "")
    source = replace(source, "    rb->splash(HZ * 2, buf);",
                     '    rb->splashf(HZ * 2, "%s", buf);')
    source = replace(source, """    if(rb->thread_self() == thread)
        rb->thread_exit();
    else
        exit(PLUGIN_ERROR);""", "    exit(PLUGIN_ERROR);")
    source = replace(source, "        if(loaded_fonts[i].status > 0)",
                     "        if(loaded_fonts && loaded_fonts[i].status >= 0)")
    source = replace(source, """    access_counter = -1;
    rb->lcd_setfont(cur_font = FONT_UI);""", """    n_fonts = 0;
    access_counter = -1;
    rb->lcd_setfont(cur_font = FONT_UI);""")
    source = replace(source, "#define MAX_FONTS (MAXUSERFONTS - 2)",
                     "#define MAX_FONTS 1")
    source = convert(source, 'FONT_DIR "/', 'PLUGIN_GAMES_DATA_DIR "/.sgt-fonts/')
    source = replace(source,
                     "                if((loaded_fonts[i].status >= 0 && loaded_fonts[i].last_use < oldest_use) || oldest_use < 0)",
                     """                if (loaded_fonts[i].status >= 0 &&
                    (oldest_use < 0 || loaded_fonts[i].last_use < oldest_use))""")
    source = replace(source, "    struct viewport *vp_main = rb->lcd_set_viewport(NULL);",
                     """    rb->lcd_set_viewport(NULL);
    struct viewport *vp_main = *rb->screens[SCREEN_MAIN]->current_viewport;""")
    source = replace(source, """            clip_rect.width  = MIN(LCD_WIDTH, w);
            clip_rect.height = MIN(LCD_HEIGHT, h);""", """            clip_rect.width = MAX(0, MIN(LCD_WIDTH, x + w) - clip_rect.x);
            clip_rect.height = MAX(0, MIN(LCD_HEIGHT, y + h) - clip_rect.y);""")
    source = replace(source, """    /* Allocating the framebuffer will mostly likely grab the
     * audiobuffer, which will make impossible to load new fonts, and
     * lead to zoomed puzzles being drawn with the default fallback
     * fonts. As a semi-workaround, we go ahead and load the biggest available
     * monospace and proportional fonts. */
    rb_setfont(FONT_FIXED, BUNDLE_MAX);
    rb_setfont(FONT_VARIABLE, BUNDLE_MAX);

    zoom_fb = smalloc(zoom_w * zoom_h * sizeof(fb_data));""",
                     """    if (zoom_w < LCD_WIDTH || zoom_h < LCD_HEIGHT) {
        rb->splash(HZ, "This board does not fill the zoom display.");
        fix_size();
        return;
    }
    zoom_fb = tlsf_malloc((size_t)zoom_w * zoom_h * sizeof(fb_data));""")
    source = replace(source, """        rb->splash(HZ, "Out of memory. Cannot zoom in.");
        return;""", """        rb->splash(HZ, "Out of memory. Cannot zoom in.");
        fix_size();
        return;""")
    source = replace(source, """    int fd = (int) ptr;
    return rb->read(fd, buf, len) == len;""", """    int fd = (int)(intptr_t)ptr;
    return rb->read(fd, buf, len) == len;""")
    source = replace(source, """static void write_wrapper(void *ptr, const void *buf, int len)
{
    int fd = (int) ptr;
    rb->write(fd, buf, len);
}

""", "")
    source = replace(source, "(void*) load_fd", "(void *)(intptr_t)load_fd")

    start = source.index("/* expects a totally free me* pointer */")
    end = source.index("\nstatic int mainmenu_cb(", start)
    save = Path(__file__).with_name("tatham-save.inc").read_text()
    source = source[:start] + save + source[end:]

    source = replace(source, 'rb->splash(HZ, msg);', 'rb->splashf(HZ, "%s", msg);')
    source = replace(source, 'rb->splash(HZ, err);', 'rb->splashf(HZ, "%s", err);')
    source = replace(source, """    rb->splash(0, quick_help_text);
    rb->button_get(true);""", """    int result = rb->view_text("Quick Help", quick_help_text);
    if (result == PLUGIN_USB_CONNECTED)
        exit(PLUGIN_USB_CONNECTED);""")
    source = replace(source, """    display_text(help_text_words, word_ptrs, help_text_style, NULL, true);""",
                     """    if (display_text(help_text_words, word_ptrs, help_text_style,
                     NULL, true))
        exit(PLUGIN_USB_CONNECTED);""")
    source = replace(source, """        int button = rb->button_get_w_tmo(timer_on ? TIMER_INTERVAL : -1);
        switch(button)""", """        int button = rb->button_get_w_tmo(timer_on ? TIMER_INTERVAL : -1);
        exit_on_usb(button);
        switch(button)""")
    source = replace(source, "        int button = rb->button_get(true);\n",
                     """        int button = rb->button_get(true);
        exit_on_usb(button);
""")
    start = source.index('    if(!strcmp(thegame.name, "Solo"))',
                         source.index("enum plugin_status plugin_start"))
    end = source.index("\n    return PLUGIN_OK;", start)
    source = source[:start] + "    puzzles_main();\n" + source[end:]
    source = replace(source, """    giant_buffer = rb->plugin_get_buffer(&giant_buffer_len);
    init_tlsf();""", """    giant_buffer = rb->plugin_get_buffer(&giant_buffer_len);
    if (!giant_buffer || giant_buffer_len < 128 * 1024)
        return PLUGIN_ERROR;
    init_tlsf();""")
    # Native dialogs may transfer storage ownership at any nesting depth.
    source = convert(source, """        switch(rb->do_menu(&menu, &sel, NULL, false))
        {""", """        switch(rb->do_menu(&menu, &sel, NULL, false))
        {
        case MENU_ATTACHED_USB:
            exit(PLUGIN_USB_CONNECTED);""")
    source = convert(source, "        int button = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);\n",
                     """        int button = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        exit_on_usb(button);
""")
    source = replace(source, "        if(rb->kbd_input(newstr, MAX_STRLEN, NULL) < 0)",
                     """        int input = rb->kbd_input(newstr, MAX_STRLEN, NULL);
        if (rb->plugin_usb_taken())
            exit(PLUGIN_USB_CONNECTED);
        if (input < 0)""")
    source = replace(source, "        rb->set_bool(cfg->name, &res);",
                     """        rb->set_bool(cfg->name, &res);
        if (rb->plugin_usb_taken())
            exit(PLUGIN_USB_CONNECTED);""")
    return source


if __name__ == "__main__":
    src, out = map(Path, sys.argv[1:])
    adapters = {"sudoku": sudoku, "reversi-gui": reversi, "fractal": fractal,
                "rockbox": tatham_frontend, "rbmalloc": tatham_allocator, "solo": solo}
    out.write_text(adapters[src.stem](src.read_text()))
