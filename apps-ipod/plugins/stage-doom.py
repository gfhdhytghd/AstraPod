#!/usr/bin/env python3
"""Stage the RockDoom foreground compatibility adapters. GPL-2.0-or-later."""
from pathlib import Path
import sys


def replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"expected one upstream match for {before!r}")
    return source.replace(before, after, 1)


def cut(source, start, finish, body):
    before, rest = source.split(start, 1)
    _, after = rest.split(finish, 1)
    return before + body + finish + after


def zone(source):
    return cut(source, "   zonebase=rb->plugin_get_audio_buffer", "   rover = zone;",
               """   zonebase = rb->plugin_get_buffer(&size);
   /* Keep the same zone budget on native and hosted builds. */
   size = MIN(size, 2 * 1024 * 1024);
   uintptr_t start = ((uintptr_t)zonebase + CACHE_ALIGN - 1)
                     & ~(uintptr_t)(CACHE_ALIGN - 1);
   size_t padding = start - (uintptr_t)zonebase;
   if (!zonebase || size <= padding + HEADER_SIZE + CHUNK_SIZE)
      I_Error("Doom needs plugin scratch space");

   size = (size - padding - HEADER_SIZE) & ~(size_t)(CHUNK_SIZE - 1);
   zonebase_size = size;
   zone = (memblock_t *)start;
""")


def system(source):
    source = cut(source, "#if defined(HAVE_LCD_COLOR)",
                 "//\n// I_Init\n", """int I_GetTime(void)
{
   unsigned long tick = *rb->current_tick;
   return (tick / HZ) * TICRATE + (tick % HZ) * TICRATE / HZ;
}

""")
    source = replace(source, """#if defined(HAVE_LCD_COLOR) && !defined(SIMULATOR) && !defined(RB_PROFILE)
   rb->timer_register(1, NULL, TIMER_FREQ/TICRATE, doomtime IF_COP(, CPU));
#endif
""", "")
    source = replace(source, """#if defined(HAVE_LCD_COLOR) && !defined(SIMULATOR) && !defined(RB_PROFILE)
   rb->timer_unregister();
#endif
""", "")
    return replace(source, """   I_Quit();
   rb->sleep(HZ*2);
""", """   rb->splash(HZ * 2, p_buf);
   exit(EXIT_FAILURE);
""")


def sound(source):
    source = cut(source, "void get_more(", "//\n// MUSIC API",
                 """void I_SubmitSound(void) {}
void I_ShutdownSound(void) {}

void I_InitSound(void)
{
   enable_sound = false;
}

""")
    return replace(source, "void I_UpdateSound( void )\n{",
                   "void I_UpdateSound( void )\n{\n   if (!enable_sound)\n      return;")


def video(source):
    source = replace(source, '#include "rockmacros.h"',
                     '#include "rockmacros.h"\n#include "usb.h"')
    # Use the regular RGB565 drawing lane on hardware and SDL.
    source = replace(source, '#include "usb.h"',
                     '#include "usb.h"\n#undef HAVE_LCD_MODES')
    source = replace(source, "    unsigned int btn;", "    long btn;")
    source = replace(source, "   int button; /* move me */", "   long button;")
    source = cut(source, "   static unsigned int holdbutton", "#if (CONFIG_KEYPAD == IPOD_4G_PAD)",
                 """   static bool holdbutton;
   bool held = rb->button_hold();
   if (held != holdbutton)
   {
      event.type = held ? ev_keydown : ev_keyup;
""")
    source = replace(source, "   holdbutton=rb->button_hold();",
                     "   holdbutton = held;")
    source = replace(source, "        btn = rb->button_get_w_tmo(0);",
                     """        btn = rb->button_get_w_tmo(0);
        if (IS_SYSEVENT(btn) &&
            rb->default_event_handler(btn) == SYS_USB_CONNECTED)
            exit(PLUGIN_USB_CONNECTED);""")
    source = replace(source, "   button = rb->button_get(false);",
                     """   button = rb->button_get(false);
   if (IS_SYSEVENT(button) &&
       rb->default_event_handler(button) == SYS_USB_CONNECTED)
      exit(PLUGIN_USB_CONNECTED);""")
    source = replace(source, """    static fb_data *lcd_fb = NULL;
    if (!lcd_fb)
    {
        struct viewport *vp_main = *(rb->screens[SCREEN_MAIN]->current_viewport);
        lcd_fb = vp_main->buffer->fb_ptr;
    }""", """    static fb_data lcd_fb[LCD_WIDTH * LCD_HEIGHT];""")
    return replace(source, "    rb->lcd_update();\n#else /* !HAVE_LCD_COLOR */",
                   """    rb->lcd_bitmap(lcd_fb, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    rb->lcd_update();
#else /* !HAVE_LCD_COLOR */""")


def rockdoom(source):
    source = replace(source, '#include "lib/helper.h"',
                     '#include "lib/helper.h"\n#include "usb.h"')
    source = replace(source, """   /* Disable all talking before initializing IRAM */
   rb->talk_disable(true);

""", "")
    source = replace(source, "       rb->talk_disable(false);\n", "")
    source = replace(source, "   rb->talk_disable(false);\n", "")
    source = replace(source, """   while(fpoint>0)
   {
#if (CONFIG_PLATFORM & PLATFORM_HOSTED)
      close(filearray[fpoint]);
#else
      rb->close(filearray[fpoint]);
#endif
      fpoint--;
   }""", """   /* The foreground runtime closes all still-owned files on teardown. */""")
    source = replace(source, "   while(filearray[i]!=id && i<8)",
                     "   while(i<fpoint && filearray[i]!=id)")
    source = replace(source, "   if(i==8)", "   if(i==fpoint)")
    source = replace(source, "   int i=0;\n   if(id<0)", "   int i=1;\n   if(id<0)")
    source = replace(source, "while((dptr=rb->readdir(filedir)))\n   {",
                     "while(filedir && i < limit && (dptr=rb->readdir(filedir)))\n   {")
    source = replace(source, "   i++;\n   temp=malloc(i*sizeof(char *));",
                     "   const int limit = i + 1;\n   temp=malloc(limit*sizeof(char *));")
    source = replace(source, "malloc(strlen(dptr->d_name)*sizeof(char))",
                     "malloc(strlen(dptr->d_name) + 1)")
    source = replace(source, """    /* Clean out the button Queue */
    while (rb->button_get(false) != BUTTON_NONE)
        rb->yield();""", """   rb->button_clear_queue();""")
    source = replace(source, "         case 5: /* Quit */",
                     """         case MENU_ATTACHED_USB:
            exit(PLUGIN_USB_CONNECTED);
         case GO_TO_PREVIOUS:
         case 5: /* Quit */""")
    source = replace(source, """        else if (result > 0)
            rb->set_option""", """        else if (result == 1)
            rb->splash(HZ, "Game audio is unavailable");
        else if (result > 1)
            rb->set_option""")
    return source


def menu(source):
    source = replace(source, "   M_SetupNextMenu(&SoundDef);",
                     '   M_StartMessage("Game audio is unavailable", NULL, false);')
    source = replace(source, """   if (ch == -1)
      return false;""", """   if (menuactive && !saveStringEnter && ev->type == ev_scroll)
      ch = ev->data1 < 0 ? KEY_UPARROW : KEY_DOWNARROW;
   if (ch == -1)
      return false;""")
    return cut(source, "void M_SystemVol(int choice)\n{", "//\n// M_DrawMainMenu",
               """void M_SystemVol(int choice)
{
   (void)choice;
}

""")


adapters = {
    "i_system.c": system, "i_sound.c": sound, "i_video.c": video,
    "rockdoom.c": rockdoom, "m_menu.c": menu, "z_zone.c": zone,
}
input_path, out = map(Path, sys.argv[1:])
source = input_path.read_text()
if input_path.name in adapters:
    source = adapters[input_path.name](source)
out.write_text(source)
