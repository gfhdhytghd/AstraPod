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
    source = replace(source, "#define SAMPLECOUNT     128",
                     "#define SAMPLECOUNT (ROCK_STREAM_BLOCK_BYTES / 4)")
    source = replace(source, "#define NUM_CHANNELS    24",
                     "#define NUM_CHANNELS    32")
    source = replace(source, "signed short *mixbuffer;",
                     "static signed short mixbuffer[MIXBUFFERSIZE];")
    source = replace(source, "static unsigned short samplerate;",
                     "static unsigned short samplerate;\nstatic bool stream_active;")
    source = replace(source, "   int id;", "   int id;\n   int lump;")
    source = replace(source, "int   *steptable; // Pitch to stepping lookup. (Not setup properly right now)\n", "")
    source = cut(source, "void* getsfx(", "static void stopchan", "")
    source = replace(source, "    channelinfo[i].data=NULL;",
                     """   if (channelinfo[i].data)
      W_UnlockLumpNum(channelinfo[i].lump);
   channelinfo[i].data = NULL;""")
    source = cut(source, "   // We will handle the new SFX.",
                 "   channelinfo[channel].stepremainder", """   int lump = S_sfx[sfxid].lumpnum;
   if (!stream_active)
      return channel;
   int len = W_LumpLength(lump);
   if (len <= 9)
      return channel;
   const unsigned char *data = W_CacheLumpNum(lump);
   unsigned int rate = data[2] | ((unsigned int)data[3] << 8);
   uint32_t samples = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                      ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
   if (data[0] != 3 || data[1] || rate < 4000 || rate > 48000 ||
       samples < 2 || samples > (uint32_t)(len - 8))
   {
      W_UnlockLumpNum(lump);
      return channel;
   }

   channelinfo[channel].lump = lump;
   channelinfo[channel].data = data + 8;
   channelinfo[channel].enddata = data + 8 + samples - 1;
   channelinfo[channel].samplerate = rate;
""")
    source = replace(source, "   int step = steptable[pitch];", "")
    source = cut(source, "   // Set stepping", "   // Separation",
                 """   unsigned int step = (channelinfo[slot].samplerate << 16)
                       / samplerate;
   channelinfo[slot].step = pitched_sounds ?
       step * (unsigned int)(128 + pitch) / 256 : step;

""")
    source = replace(source, "   int*  steptablemid = steptable + 128;\n", "")
    source = cut(source, "   // This table provides step widths", "   // Generates", "")
    source = replace(source, "   return W_GetNumForName(namebuf);",
                     "   return W_CheckNumForName(namebuf);")
    source = cut(source, "            channelinfo[chan].data +=",
                 "         }\n", """            unsigned int advance = channelinfo[chan].stepremainder >> 16;
            channelinfo[chan].stepremainder &= 0xffff;
            if (advance >= (size_t)(channelinfo[chan].enddata -
                                    channelinfo[chan].data))
               stopchan(chan);
            else
               channelinfo[chan].data += advance;
""")
    return cut(source, "void get_more(", "//\n// MUSIC API",
               Path(__file__).with_name("doom-stream.inc").read_text())


def defaults(source):
    return replace(source, '{"enable_sound",{(void *)&enable_sound, NULL},{0, NULL}',
                   '{"enable_sound",{(void *)&enable_sound, NULL},{1, NULL}')


def game_loop(source):
    source = replace(source, "   I_SubmitSound();\n\n", "")
    return replace(source, "      // Update display, next frame, with current state.",
                   "      I_SubmitSound();\n\n      // Update display, next frame, with current state.")

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
    source = replace(source, '      rb->splash(HZ*2, "Missing Base WAD!");',
                     """      rb->splash(HZ * 4,
          "Place rockdoom.wad and a base WAD in /.rockbox/doom/");""")
    source = replace(source, '   int mod = (rb->sound_max(SOUND_VOLUME)-rb->sound_min(SOUND_VOLUME))/15;',
                     '   int mod = MAX(1, (rb->sound_max(SOUND_VOLUME) - rb->sound_min(SOUND_VOLUME)) / 15);')
    source = replace(source, '   if(mod == 0)\n       mod = rb->global_status->volume;\n', '')
    return source


def menu(source):
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
    "i_system.c": system, "i_sound.c": sound, "d_main.c": game_loop, "i_video.c": video,
    "rockdoom.c": rockdoom, "m_menu.c": menu, "m_misc.c": defaults, "z_zone.c": zone,
}
input_path, out = map(Path, sys.argv[1:])
source = input_path.read_text()
if input_path.name in adapters:
    source = adapters[input_path.name](source)
out.write_text(source)
