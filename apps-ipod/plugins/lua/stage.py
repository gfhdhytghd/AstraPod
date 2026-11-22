#!/usr/bin/env python3
"""Stage the owned Lua services around the unmodified Rockbox VM."""
from pathlib import Path
import re
import sys


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one Lua source match: {old[:80]}")
    return text.replace(old, new)


def rockconf(text):
    text = replace(
        text,
        '#define LUA_PATH_DEFAULT  "$/?.lua;" "$/?/init.lua;" VIEWERS_DIR"/lua/?.lua;" VIEWERS_DIR"/lua/?/init.lua;"',
        '#define LUA_PATH_DEFAULT VIEWERS_DIR "/lua/?.lua"',
    )
    return replace(text, '#include "plugin.h"', """#include "plugin.h"
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include <errno.h>
#undef errno
#define errno EIO
#endif""")



def rockaux(text):
    start = text.index("extern const char *strpbrk_n")
    native = text.index("#if (CONFIG_PLATFORM & PLATFORM_NATIVE)", start)
    screen = text.index("/* splash string", native)
    power = text.index("long rb_pow", screen)
    text = text[:start] + text[native:screen] + text[power:]
    text = replace(text, '#if (CONFIG_PLATFORM & PLATFORM_NATIVE)\nint errno = 0;\n#endif\n', '')
    return replace(text, '    return NULL;\n}', '    return "I/O error";\n}')


def graphics(text):
    # Keep the upstream image arithmetic and only the drawing wrappers used
    # by the shipped games and their Lua helpers.
    text = replace(text, '#include "rocklib_img.h"',
                   '#include "rocklib_img.h"\n#include "draw/bmp.h"')
    text = replace(text, '    rb->viewport_set_buffer(vp, &fb, SCREEN_MAIN); /* not multiscreen aware yet */',
                   '    vp->buffer = &fb;')
    text = replace(text, 'rb->screens[SCREEN_MAIN]->set_viewport(&img_vp)',
                   'rb->lcd_set_viewport(&img_vp)')
    text = replace(text, 'rb->screens[SCREEN_MAIN]->set_viewport(oldvp)',
                   'rb->lcd_set_viewport(oldvp)')
    text = replace(text, '    pixel_to_native(width, height, &w_native, &h_native);',
                   '''    if (width <= 0 || width > LCD_WIDTH ||
        height <= 0 || height > LCD_HEIGHT)
        luaL_error(L, "Invalid image dimensions");
    pixel_to_native(width, height, &w_native, &h_native);''')
    required = {
        "lcd_update", "lcd_clear_display", "lcd_putsxy", "lcd_puts_scroll",
        "lcd_scroll_stop", "font_getstringsize", "lcd_framebuffer",
        "lcd_set_foreground", "lcd_set_background", "lcd_get_foreground",
        "lcd_get_background", "lcd_update_rect", "lcd_drawrect", "lcd_drawline",
        "lcd_fillrect", "lcd_hline", "lcd_vline", "lcd_drawpixel",
        "lcd_rgbpack", "lcd_rgbunpack",
    }
    begin = text.index("#if defined NB_SCREENS", text.index("#define RB_WRAP"))
    end = text.index("LUALIB_API int luaopen_rock_img", begin)
    body = text[begin:end]
    helpers = (
        re.search(r"static const unsigned char \* lcd_putshelper.*?\n}\n", body, re.S)[0]
        + re.search(r"static void checkint_arr.*?\n}\n", body, re.S)[0]
    )
    functions = dict(re.findall(r"^RB_WRAP\((\w+)\)\n(\{.*?^\})", body, re.M | re.S))
    functions["lcd_framebuffer"] = """{
    rb->lcd_set_viewport(NULL);
    struct viewport vp;
    rb->viewport_set_defaults(&vp, SCREEN_MAIN);
    rli_wrap(L, vp.buffer->data, LCD_WIDTH, LCD_HEIGHT);
    return 1;
}"""
    functions["font_getstringsize"] = """{
    const unsigned char *str = luaL_checkstring(L, 1);
    int font = luaL_optint(L, 2, FONT_UI);
    int w, h;
    if (font == FONT_UI)
        font = rb->screens[SCREEN_MAIN]->getuifont();
    int length = rb->font_getstringsize(str, &w, &h, font);
    lua_pushinteger(L, length);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 3;
}"""
    wrappers = "\n".join(f"RB_WRAP({name})\n{functions[name]}\n"
                         for name in sorted(required))
    wrappers = re.sub(r"RB_SCREENS\(L, [0-9]+, (\w+), ",
                      r"rb->lcd_\1(", wrappers)
    wrappers = re.sub(r"RB_SCREENS\(L, [0-9]+, (\w+)\)",
                      r"rb->lcd_\1()", wrappers)
    # The optional screen argument has no effect on the iPod's single LCD.
    methods = "\n".join(f'    {{"{name}", rock_{name}}},' for name in sorted(required))
    table = (
        "static const luaL_Reg rocklib_img[] = {\n" + methods +
        '\n    {"new_image", rli_new},\n    {NULL, NULL},\n};\n\n'
    )
    return text[:begin] + helpers + wrappers + table + text[end:]


def iolib(text):
    text = replace(text, '#include "llimits.h"', '#include "llimits.h"\n#include "rock-lua-file.h"\n#undef open\n#define open lua_file_open')
    text = replace(text, '  int ok = (close(*p) == 0);',
                   '''  int ok = (lua_file_close(*p, true) == 0);
  if (!ok) {
    *p = -1;
    return luaL_error(L, "Could not close Lua game file");
  }''')
    text = replace(text, '  if (*f >= 0)\n    aux_close(L);',
                   '  if (*f >= 0)\n    lua_file_close(*f, false);\n  *f = -1;')
    text = replace(text, '  return pushresult(L, status, NULL);',
                   '  if (!status) lua_file_failed(*f);\n  return pushresult(L, status, NULL);')
    return text


def oslib(text):
    text = replace(text, '#include "rocklibc.h" /* ROCKLUA ADDED */',
                   '#include "rocklibc.h"\n#include "rock-lua-file.h"')
    text = replace(text, 'remove(filename) == 0', 'lua_file_remove(filename) == 0')
    start = text.index("static int os_rename")
    end = text.index("/*", start)
    text = text[:start] + text[end:]
    text = replace(text, '  {"rename",    os_rename},\n', '')
    return text


def luadir(text):
    start = text.index("static int make_dir")
    end = text.index("/*", start)
    text = text[:start] + text[end:]
    return replace(text, '    {"mkdir", make_dir},\n    {"rmdir", remove_dir},\n', '')


def file_compat(text):
    text = replace(text, '#define ftruncate(fd,l) rb->ftruncate(fd,l)\n', '')
    return replace(text, 'extern int errno;\n', '')


src, dst = map(Path, sys.argv[1:])
filters = {
    "rockconf.h": rockconf,
    "rockaux.c": rockaux,
    "rocklib_img.c": graphics,
    "liolib.c": iolib,
    "loslib.c": oslib,
    "luadir.c": luadir,
    "rocklibc.h": file_compat,
}
contents = src.read_text()
if src.name in filters:
    contents = filters[src.name](contents)
dst.write_text(contents)
