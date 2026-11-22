#!/usr/bin/env python3
"""Stage stock PDa with the local input, file, and PCM contract."""
from pathlib import Path
import sys

unit, source, target = sys.argv[1:]
code = Path(source).read_text()


def replace(old, new):
    global code
    if code.count(old) != 1:
        raise SystemExit(f"{unit}: review PDBox source anchor {old!r}")
    code = code.replace(old, new)


def section(first, next_section, replacement):
    start = code.index(first)
    end = code.index(next_section, start)
    replace(code[start:end], replacement)


if unit == "pdbox.h":
    code = code.replace("struct event_queue", "struct pdbox_mailbox")
    replace("/* Prototypes of network functions. */\nvoid net_init(void);\n"
            "void net_destroy(void);", "struct pdbox_mailbox;")
    code = code.replace("send_datagram", "pdbox_send_datagram")
    code = code.replace("receive_datagram", "pdbox_receive_datagram")
    replace("char data[MAX_DATAGRAM_SIZE];", "char data[MAX_DATAGRAM_SIZE + 1];")
    replace("#define strstr rb->strcasestr", "#undef strstr\n#define strstr rb->strcasestr")
    replace("unsigned int pd_gui_load_patch(struct pd_widget* wg, unsigned int max_widgets);", "")
    replace("bool pd_gui_parse_buttons(unsigned int widgets);",
            "bool pd_gui_parse_buttons(unsigned int widgets, long button);")
elif unit == "pdbox-gui.c":
    section("/* Load PD patch. */", "/* Draw standard",
            "")
    replace("extern char* filename;\nextern bool quit;\n", "")
    replace("bool pd_gui_parse_buttons(unsigned int widgets)",
            "bool pd_gui_parse_buttons(unsigned int widgets, long bv)")
    replace("    long bv = rb->button_get(false);\n", "")
    section("    /* Query whether we have to quit. */", "    /* Check the action",
            "")
    replace("            if(widgets == 0)\n                action_on = true;",
            "            action_on = true;")
    replace("            if(widgets == 0)\n                action_on = false;",
            "            action_on = false;")
    replace("if(diff_bv & PDPOD_WHEELRIGHT)", "if(bv & PDPOD_WHEELRIGHT)")
    replace("if(diff_bv & PDPOD_WHEELLEFT)", "if(bv & PDPOD_WHEELLEFT)")
    replace("    if(diff_bv == 0)", "    if(diff_bv == 0 &&\n"
            "       !(bv & (PDPOD_WHEELLEFT | PDPOD_WHEELRIGHT)))")
    replace("    if(argument != NULL)\n        argvalue = atof(argument);",
            "    if(!object || !argument)\n        return;\n"
            "    argvalue = atof(argument);\n"
            "    if(!(argvalue >= -1000000 && argvalue <= 1000000))\n"
            "        return;")
    replace("if(strncmp(object, wg->name, strlen(wg->name)) == 0)",
            "if(wg->id != PD_TEXT && strcmp(object, wg->name) == 0)")
    replace("            case PD_NUMBER:\n",
            "            case PD_NUMBER:\n"
            "                rb->lcd_set_foreground(bgcolor);\n"
            "                rb->lcd_fillrect(wg->x, wg->y, wg->w + 1, wg->h + 1);\n"
            "                rb->lcd_set_foreground(fgcolor);\n")
elif unit == "pdbox-func.c":
    replace("extern bool quit;", "extern void pdbox_request_stop(void);")
    replace("quit = true;", "pdbox_request_stop();")
elif unit == "PDa/src/m_binbuf.c":
    section("int binbuf_read(t_binbuf *b,", "int binbuf_read_via_path",
            "/* Only the foreground's validated snapshot can enter PDa. */\n"
            "extern int pdbox_read_patch(t_binbuf *, const char *, const char *);\n"
            "int binbuf_read(t_binbuf *b, char *filename, char *dirname, int crflag)\n"
            "{\n"
            "    return crflag ? 1 : pdbox_read_patch(b, filename, dirname);\n"
            "}\n\n")
    # GNU/hosted builds otherwise try to import perror from libc.
    replace("#if !defined(ROCKBOX) || (defined(ROCKBOX) && defined(SIMULATOR))",
            "#ifndef ROCKBOX")
elif unit == "PDa/src/m_obj.c":
    # Keep PDa's outlet guard inside the 64 KiB DSP worker stack.
    replace("#define STACKSIZE 1000000", "#define STACKSIZE (32 * 1024)")
elif unit == "PDa/src/s_print.c":
    # The desktop's libc is outside the plugin ABI, including on SDL.
    code = code.replace("defined(ROCKBOX) && defined (SIMULATOR)", "0")
    code = code.replace("#ifdef SIMULATOR", "#if 0")
Path(target).write_text(code)
