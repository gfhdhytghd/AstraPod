#!/usr/bin/env python3
"""Stage the foreground image and Sokoban adapters. GPL version 2 or later."""
from pathlib import Path
import sys


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"expected one upstream match for {old!r}")
    return source.replace(old, new)


def rockpaint(s):
    s = replace(s, '#include "lib/pluginlib_bmp.h"',
                "int write_bmp_fd(int fd, struct bitmap *bm);")
    s = replace(s, "static bool audio_buf = false;\n", "")
    start = s.index("char bbuf[MAX_PATH];")
    end = s.index("/***********************************************************************\n * HSVRGB", start)
    s = s[:start] + '''static bool check_extention(const char *filename, const char *ext)
{
    const char *suffix = rb->strrchr(filename, '.');
    return suffix && !rb->strcasecmp(suffix, ext);
}

''' + s[end:]
    s = replace(s, "int x, int y, int ofs, const unsigned char *str )",
                "int x, int y, int ofs, const unsigned char *str, int font )")
    s = replace(s, "struct font *pf = rb->font_get( FONT_UI );",
                '''struct font *pf = rb->font_get(font);
#ifdef HAVE_PODBOX_FONT
    if (pf && pf->pbf.file_size)
        pf = rb->font_get(FONT_SYSFIXED);
#endif''')
    s = replace(s, '''            case ROCKPAINT_DRAW:
                return color;
        }''', '''            case ROCKPAINT_DRAW:
                return color;
            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    return color;
                break;
        }''')
    s = replace(s, '''            case ROCKPAINT_TOOLBAR2:
                return;
        }
        if( quit ) return;''', '''            case ROCKPAINT_TOOLBAR2:
                return;
            case ROCKPAINT_MENU:
                goto_menu();
                return;
            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    return;
                break;
        }
        if (quit || rb->plugin_usb_taken())
            return;''')
    s = replace(s, "int current_font_id = rb->global_status->font_id[SCREEN_MAIN];",
                "int current_font_id = -1;")
    s = replace(s, '''                if (current_font_id != rb->global_status->font_id[SCREEN_MAIN])
                    rb->font_unload(current_font_id);
                if(browse_fonts( buffer->text.font, MAX_PATH ) )
                {
                    current_font_id = rb->font_load(buffer->text.font );
                    rb->lcd_setfont(current_font_id);
                }''', '''                if (rb->pick_file(buffer->text.font, MAX_PATH, FONT_DIR,
                                  "Choose Font", ".fnt", 0, true) > 0)
                {
                    rb->lcd_setfont(FONT_SYSFIXED);
                    rb->font_unload(current_font_id);
                    current_font_id = rb->font_load(buffer->text.font);
                    if (current_font_id >= 0)
                        rb->lcd_setfont(current_font_id);
                    else
                        rb->splash(HZ, "Cannot load font");
                }''')
    s = replace(s, "                                  buffer->text.text );",
                "                                  (const unsigned char *)buffer->text.text,\n"
                "                                  current_font_id >= 0\n"
                "                                  ? current_font_id : FONT_SYSFIXED);")
    start = s.index("                if( buffer->text.font[0] )")
    end = s.index("                return;", start)
    s = s[:start] + '''                rb->lcd_setfont(FONT_UI);
                rb->font_unload(current_font_id);
''' + s[end:]
    s = replace(s, "    int multi;\n", "    int multi;\n    char path[MAX_PATH];\n")
    s = replace(s, "if( browse( filename, MAX_PATH, \"/\" ) )",
                'if (rb->pick_file(path, sizeof(path), "/", "Load Bitmap",\n'
                '                                  ".bmp", 0, false) > 0)')
    s = replace(s, "if( load_bitmap( filename ) <= 0 )",
                "if (load_bitmap(path) <= 0)")
    s = replace(s, '''                        rb->splashf( 1*HZ, "Error while loading %s",
                                    filename );
                        clear_drawing();''', '''
                        rb->splashf(HZ, "Cannot load %s", path);'''.lstrip("\n"))
    s = replace(s, '''                        rb->splashf( 1*HZ, "Image loaded (%s)", filename );''',
                '''                        rb->strlcpy(filename, path, sizeof(filename));
                        rb->splashf(1 * HZ, "Image loaded (%s)", filename);''')
    start = s.index("                if (!filename[0])")
    end = s.index("                break;", start)
    s = s[:start] + '''                rb->strlcpy(path, filename[0] ? filename : "/", sizeof(path));
                if (!rb->kbd_input(path, sizeof(path), NULL))
                {
                    if (!check_extention(path, ".bmp") &&
                        rb->strlcat(path, ".bmp", sizeof(path)) >= sizeof(path))
                    {
                        rb->splash(HZ, "Filename too long");
                        break;
                    }
                    if (rb->file_exists(path))
                    {
                        const char *lines[] = {"Replace bitmap?", path};
                        const struct text_message message = {lines, 2};
                        if (rb->gui_syncyesno_run(&message, NULL, NULL) != YESNO_YES)
                            break;
                    }
                    if (save_bitmap(path) > 0)
                    {
                        rb->strlcpy(filename, path, sizeof(filename));
                        rb->splashf(HZ, "File saved (%s)", filename);
                    }
                    else
                        rb->splash(HZ, "Cannot save bitmap");
                }
''' + s[end:]
    s = replace(s, '''                if (!audio_buf)
                    playback_control( NULL );
                else
                    rb->splash(HZ, "Cannot restart playback");''',
                "                playback_control(NULL);")
    # The stock loop transports USB as a plugin_status, which is wider than bool.
    s = replace(s, "static bool rockpaint_loop( void )",
                "static enum plugin_status rockpaint_loop(void)")
    s = replace(s, "    while (!quit) {", "    while (!quit && !rb->plugin_usb_taken()) {")
    start = s.index("static int load_bitmap( const char *file )")
    end = s.index("enum plugin_status plugin_start", start)
    s = s[:start] + '#include "rockpaint-image.inc"\n\n' + s[end:]
    start = s.index("    if (buffer_size < sizeof(*buffer) + 3)")
    end = s.index("    buffer = (union buf*)", start)
    s = s[:start] + '''    if (!temp || buffer_size < sizeof(*buffer) + 3)
    {
        rb->splash(HZ, "Not enough memory");
        return PLUGIN_ERROR;
    }
''' + s[end:]
    s = replace(s, "            rb->strcpy( filename, parameter );",
                "            rb->strlcpy(filename, parameter, sizeof(filename));")
    return replace(s, "    return rockpaint_loop();", '''
    enum plugin_status result = rockpaint_loop();
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : result;'''.lstrip("\n"))


def bmp_writer(s):
    s = s[:s.index("/**\n   Very simple image scale")]
    s = replace(s, '#include "pluginlib_bmp.h"', '#include "plugin.h"')
    s = replace(s, "int save_bmp_file( char* filename, struct bitmap *bm )",
                "int write_bmp_fd(int fd, struct bitmap *bm)")
    start = s.index("    int fh;")
    s = s[:start] + '''    if (bm->format != FORMAT_NATIVE || bm->width < 1 ||
        bm->width > LCD_WIDTH || bm->height < 1 || bm->height > LCD_HEIGHT)
        return -1;
    if (rb->write(fd, header, sizeof(header)) != sizeof(header))
        return -1;
    unsigned char line[LCD_WIDTH * 3 + 3];
    for (int y = bm->height - 1; y >= 0; y--)
    {
        const fb_data *pixels = (const fb_data *)bm->data + y * bm->width;
        for (int x = 0; x < bm->width; x++)
        {
            line[3 * x] = FB_UNPACK_BLUE(pixels[x]);
            line[3 * x + 1] = FB_UNPACK_GREEN(pixels[x]);
            line[3 * x + 2] = FB_UNPACK_RED(pixels[x]);
        }
        rb->memset(line + bm->width * 3, 0, padsize);
        if (rb->write(fd, line, line_width) != line_width)
            return -1;
    }
    return 1;
}
#endif
'''
    return s


def sliding_puzzle(s):
    start = s.index("#ifdef HAVE_ALBUMART\n#include")
    end = s.index('#include "pluginbitmaps/sliding_puzzle.h"', start)
    s = s[:start] + s[end:]
    s = replace(s, '''#ifdef HAVE_ALBUMART
static char albumart_path[MAX_PATH+1];
#endif
''', "")
    start = s.index("#ifdef HAVE_ALBUMART\nstatic const char * get_albumart_bmp_path")
    end = s.index("static bool load_resize_bitmap", start)
    s = s[:start] + s[end:]
    s = replace(s, '''        case PICMODE_ALBUM_ART:
            filename = get_albumart_bmp_path();
            break;''', '''        case PICMODE_ALBUM_ART:
        {
            struct bitmap bitmap = {
                .width = IMAGE_WIDTH,
                .height = IMAGE_HEIGHT,
                .data = img_buf,
            };
            img_buf_path[0] = '\\0';
            if (!img_buf || rb->load_plugin_art(&bitmap, img_buf_len) <= 0)
                return false;
            puzzle_bmp_ptr = (const fb_data *)img_buf;
            return true;
        }''')
    s = replace(s, "    if( filename != NULL )",
                "    if (img_buf && filename && rb->strlen(filename) < sizeof(img_buf_path))")
    s = replace(s, '''        rc = READ_IMAGE( filename, &main_bitmap,
                         img_buf_len,
                         FORMAT_NATIVE|FORMAT_RESIZE|FORMAT_DITHER,
                         NULL);''', '''
        img_buf_path[0] = '\\0';
        rc = rb->read_plugin_bmp(filename, &main_bitmap, img_buf_len, true);'''.lstrip("\n"))
    s = replace(s, "rb->lcd_getstringsize((unsigned char *)\"Sliding Puzzle\", &w, &h);",
                'rb->lcd_getstringsize("Sliding Puzzle", &w, &h);')
    s = replace(s, '(unsigned char *)"Sliding Puzzle");', '"Sliding Puzzle");')
    return s


def sokoban(s):
    s = replace(s, "static char buf[ROWS*(COLS + 1)];",
                "static char buf[MAX_PATH + ROWS * (COLS + 1)];")
    start = s.index("static bool read_levels(bool initialize)")
    end = s.index("static void load_level", start)
    s = s[:start] + '#include "sokoban-file.inc"\n\n' + s[end:]
    s = replace(s, '''    int start;                 /* Index of first buffered board */
    int end;                   /* Index of last buffered board */
    short prebuffered_boards;  /* Number of boards before current to store */
''', "")
    s = replace(s, '''    buffered_boards.start = 0;
    buffered_boards.end = 0;
    buffered_boards.prebuffered_boards = 0;

''', "")
    s = replace(s, "int index = current_info.level.index - buffered_boards.start;",
                "int index = current_info.level.index;")
    s = s.replace("read_levels(true)", "read_levels()")
    start = s.index("    /* Get the buffered board index")
    end = s.index("    level = ", start)
    s = s[:start] + '''    if (index < 0 || index >= current_info.max_level)
        return;
''' + s[end:]
    s = replace(s, "            if (c > 3) {",
                "            if (c > 0) {")
    s = replace(s, "for (c = 0; current_info.board[r][c] == ' ' && c < COLS; c++)",
                "for (c = 0; c < COLS && current_info.board[r][c] == ' '; c++)")
    s = replace(s, '''for (r = 0; (current_info.board[r][c] == ' ' ||
             current_info.board[r][c] == 'X') && r < ROWS; r++)''', '''for (r = 0; r < ROWS && (current_info.board[r][c] == ' ' ||
             current_info.board[r][c] == 'X'); r++)''')
    s = replace(s, '''for (r = ROWS - 1; (current_info.board[r][c] == ' ' ||
             current_info.board[r][c] == 'X') && r >= 0; r--)''', '''for (r = ROWS - 1; r >= 0 && (current_info.board[r][c] == ' ' ||
             current_info.board[r][c] == 'X'); r--)''')
    start = s.index("static bool save(")
    end = s.index("static bool load(", start)
    s = s[:start] + s[end:]
    s = replace(s, '''    int i, n;
    int len;
    int button;
    bool play_solution;''', '''    int i, n = 0;
    int len = 0;
    int button;
    bool play_solution = false;''')
    s = replace(s, "    rb->read_line(fd, buf, sizeof(buf));",
                '''    int first_line = rb->read_line(fd, buf, sizeof(buf));
    if (first_line <= 0 || rb->strlen(buf) >= sizeof(buf) - 1)
    {
        rb->close(fd);
        return false;
    }''')
    start = s.index("        /* Read filename of levelset */")
    end = s.index("        if (!read_levels())", start)
    s = s[:start] + '''        bool ok = read_sokoban_save(fd, &play_solution, &n, &len);
        if (rb->close(fd) < 0 || !ok)
        {
            if (!silent)
                rb->splash(HZ * 2, "Invalid Sokoban save");
            return false;
        }
''' + s[end:]
    s = replace(s, '''        if (current_info.level.boxes_to_go == 0 ||
            current_info.player.row == 0 || current_info.player.col == 0)''',
                "        if (current_info.level.boxes_to_go == 0)")
    s = replace(s, "                save(SOKOBAN_SAVE_FILE, false);\n                rb->reload_directory();", '''
                if (!save(SOKOBAN_SAVE_FILE, false))
                    menu_quit = false;
                break;

            case MENU_ATTACHED_USB:
                return MENU_ATTACHED_USB;'''.lstrip("\n"))
    s = replace(s, "static bool sokoban_loop(void)", "static enum plugin_status sokoban_loop(void)")
    s = replace(s, '"[SELECT+MENU] Menu"', '"Long [SELECT] Menu"')
    s = s.replace("                switch (sokoban_menu()) {",
                  "                switch (sokoban_menu()) {\n"
                  "                    case MENU_ATTACHED_USB:\n"
                  "                        return PLUGIN_USB_CONNECTED;")
    s = replace(s, '''                        case SOKOBAN_QUIT_REPLAY:''',
                '''                        case SYS_USB_CONNECTED:
                            rb->default_event_handler(button);
                            return false;
                        case SOKOBAN_QUIT_REPLAY:''')
    s = replace(s, "            rb->button_clear_queue();\n            rb->lcd_clear_display();",
                '''            if (i != n)
                return false;
            rb->button_clear_queue();
            rb->lcd_clear_display();''')
    # Use a separate object for the solution path; save() formats buf.
    s = replace(s, "                        if (!rb->kbd_input(buf, MAX_PATH, NULL))\n"
                   "                            save(buf, true);",
                '''                        char path[MAX_PATH];
                        if (rb->strlcpy(path, buf, sizeof(path)) < sizeof(path) &&
                            !rb->kbd_input(path, sizeof(path), NULL))
                            save(path, true);''')
    return replace(s, "    return sokoban_loop();",
                   "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : sokoban_loop();")


adapters = {
    "rockpaint": rockpaint,
    "pluginlib_bmp": bmp_writer,
    "sliding_puzzle": sliding_puzzle,
    "sokoban": sokoban,
}

source, output = map(Path, sys.argv[1:3])
output.write_text(adapters[source.stem](source.read_text()))
