#!/usr/bin/env python3
"""Stage the ZXBox core with bounded snapshot, audio and input adapters."""
from pathlib import Path
import sys
import re

src, dst = map(Path, sys.argv[1:])
code = src.read_text()


def replace(before, after):
    global code
    if code.count(before) != 1:
        raise ValueError(f"{src}: expected one instance of {before!r}")
    code = code.replace(before, after)


def cut(start, end, body=""):
    global code
    begin = code.index(start)
    code = code[:begin] + body + code[code.index(end, begin):]


if src.name == "helpers.c":
    replace("if ( rb->read(fd, &c, 1) )", "if (rb->read(fd, &c, 1) == 1)")
    replace("offset = rb->plugin_get_audio_buffer(&totalSize);",
            """offset = rb->plugin_claim_scratch(192 * 1024, SIZE_MAX,
                                         &totalSize);
        if (!offset)
            return NULL;""")
    replace("if (size + 4 > totalSize)", "if (size > totalSize || totalSize - size < 4)")
elif src.name == "snapshot.c":
    replace("static void snsh_sna_load(SNFILE *fp)", "static bool snsh_sna_load(SNFILE *fp)")
    cut("  if(!read_header(&sna, sna_size, fp)) return;",
        "  LOAD_NORMAL_REGS(sna);", """  if (!read_header(&sna, sna_size, fp))
    return false;
  unsigned int stack = ((unsigned int)sna.sph << 8) | sna.spl;
  if (sna.im > 2 || sna.border > 7 || stack < 0x4000 || stack == 0xffff ||
      snread(z80_proc.mem + 0x4000, 0xc000, fp) != 0xc000)
    return false;

""")
    cut("  z80_proc.iff1 = z80_proc.iff2 = sna.iff2", "static void save_snapshot_file_type",
            """  z80_proc.iff1 = z80_proc.iff2 = sna.iff2 ? 1 : 0;
  z80_proc.it_mode = sna.im;
  z80_proc.haltstate = 0;
  sp_init_screen_mark();
  return true;
}

bool zx_load_sna(const char *path)
{
  int fd = rb->open(path, O_RDONLY);
  if (fd < 0)
    return false;

  SNFILE file = { .isfile = 1, .fd = fd };
  bool ok = rb->ffilesize(fd) == sna_size + 0xc000 && snsh_sna_load(&file);
  return rb->close(fd) == 0 && ok;
}

""")
elif src.name == "spsound.c":
    cut("#include <stdio.h>", "#ifndef VOLREDUCE",
        """int bufframes = 1;
int sound_avail;
int sound_on = 1;
int sound_to_autoclose;
static int last_not_played;

""")
    code = code[:code.index("void autoclose_sound(void)")]
    code += Path(__file__).with_name("zxbox-stream.inc").read_text()
elif src.name == "spmain.c":
    cut("#define GLOBALCFG", "#define SHOW_OFFS",
        """static bool zxbox_menu(void)
{
    MENUITEM_STRINGLIST(menu, "ZXBox", NULL, "Resume", "Quit");
    int selected = 0;
    rb->rock_stream_pause(true);
    int result = rb->do_menu(&menu, &selected, NULL, false);
    rb->rock_stream_pause(false);
    return result == 1 || result == MENU_ATTACHED_USB;
}

""")
    cut("          if (zxbox_menu()){", "          exit_requested = 0;",
        """          if (rb->plugin_usb_taken() || zxbox_menu())
              return;
""")
    replace("      play_tape();\n", "")
    cut("    if(SPNM(load_trapped)) {", "    z80_interrupt(0xFF);",
        "    SPNM(load_trapped) = 0;\n")
    replace("   if(!evenframe) rec_tape();\n", "")
    replace("      if(!sound_avail) spti_wait();", "      spti_wait();")
    code = code[:code.index("void check_params")]
    code += """extern bool zx_load_sna(const char *path);

bool start_spectemu(const void *parameter)
{
    spti_init();
    if (!zx_load_sna(parameter))
    {
        rb->splash(2 * HZ, "Unsupported or incomplete 48K SNA");
        return false;
    }
    init_spect_scr();
    init_spect_sound();
    init_spect_key();
    run_singlemode();
    return true;
}
"""
elif src.name == "zxbox.c":
    replace('#include "zxconfig.h"', '#include "zxconfig.h"\n#include "usb.h"')
    replace("    if (!parameter)\n        return PLUGIN_ERROR;", """    char path[MAX_PATH];
    if (!parameter)
    {
        if (rb->pick_file(path, sizeof(path), "/", "Choose 48K SNA snapshot",
                          ".sna", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        parameter = path;
    }
    settings.kempston = 1;
    settings.sound = 1;""")
    replace("    start_spectemu(parameter);", "    bool started = start_spectemu(parameter);")
    replace("""    rb->audio_stop();
    rb->mixer_set_frequency(HW_SAMPR_DEFAULT);

return PLUGIN_OK;""", """    rb->rock_stream_close();
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    return started ? PLUGIN_OK : PLUGIN_ERROR;""")
    cut("        int ki;", "        if ( buttons == previous_state )",
        """        int ki;
        int buttons = rb->button_status();
        long event;
        while ((event = rb->button_get(false)) != BUTTON_NONE)
        {
            if (IS_SYSEVENT(event) &&
                rb->default_event_handler(event) == SYS_USB_CONNECTED)
            {
                exit_requested = 1;
                return;
            }
            if (event == (BUTTON_MENU | BUTTON_REL))
                exit_requested = 1;
            else if (event == BUTTON_SCROLL_BACK || event == BUTTON_SCROLL_FWD)
                buttons |= event;
        }
""")
elif src.name == "spmain.h":
    replace("void start_spectemu", "bool start_spectemu")
elif src.name == "zxvid_16bpp.c":
    replace("        struct viewport *vp_main = rb->lcd_set_viewport(NULL);",
            """        rb->lcd_set_viewport(NULL);
        struct viewport *vp_main = *rb->screens[SCREEN_MAIN]->current_viewport;""")
elif src.name == "keymaps.h":
    code = re.sub(r"#define ZX_UP +BUTTON_MENU\b",
                  "#define ZX_UP BUTTON_SCROLL_BACK", code, count=1)
    code = re.sub(r"#define ZX_DOWN +BUTTON_PLAY\b",
                  "#define ZX_DOWN BUTTON_SCROLL_FWD", code, count=1)

dst.write_text(code)
