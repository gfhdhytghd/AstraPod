#!/usr/bin/env python3
"""Stage the Rockboy core with owned input, scratch and PCM. GPL-2.0-or-later."""
from pathlib import Path
import sys

src, output = map(Path, sys.argv[1:])
code = src.read_text()


def replace(before, after):
    global code
    if code.count(before) != 1:
        raise ValueError(f"{src}: expected one instance of {before!r}")
    code = code.replace(before, after)


if src.name == "rockboy.c":
    code = code[:code.index("int shut,cleanshut;")]
    code += Path(__file__).with_name("rockboy-compat.inc").read_text()
elif src.name == "rbsound.c":
    code = code[:code.index("struct pcm pcm")]
    code += Path(__file__).with_name("rockboy-pcm.inc").read_text()
elif src.name == "loader.c":
    # Defer cartridge sidecars until their loaders and write paths are scoped.
    code = code[:code.index("static const char *romfile;")]
    code += r"""
void loader_init(const char *path)
{
    unsigned char header[0x150];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        die("Cannot open Game Boy ROM");
        return;
    }

    off_t size = rb->ffilesize(fd);
    bool valid = size >= 32768 && size <= 2 * 1024 * 1024 &&
        read(fd, header, sizeof(header)) == sizeof(header);
    if (valid)
    {
        unsigned int type = header[0x147];
        valid = (type == 0 || mbc_table[type]) &&
                header[0x148] < 8 && header[0x149] < ARRAYLEN(ramsize_table) &&
                size == (32768L << header[0x148]);
    }
    if (!valid)
    {
        close(fd);
        die("Unsupported or incomplete Game Boy ROM");
        return;
    }

    mbc.romsize = romsize_table[header[0x148]];
    mbc.ramsize = ramsize_table[header[0x149]];
    mbc.type = mbc_table[header[0x147]] & ~(MBC_BAT | MBC_RTC);
    mbc.rombank = 1;
    hw.cgb = (header[0x143] & 0x80) != 0;
    rom.bank = malloc(size);
    ram.sbank = mbc.ramsize ? malloc(8192u * mbc.ramsize) : NULL;
    valid = rom.bank && (!mbc.ramsize || ram.sbank) &&
            lseek(fd, 0, SEEK_SET) == 0 && read(fd, rom.bank, size) == size;
    if (close(fd) < 0 || !valid)
    {
        die("Cannot load Game Boy ROM");
        return;
    }
    if (ram.sbank)
        memset(ram.sbank, 0, 8192u * mbc.ramsize);
}
"""
elif src.name == "sys_rockbox.c":
    replace("    unsigned int btn;", "    long btn;")
    replace("        btn = rb->button_get(false);", """        btn = rb->button_get(false);
        if (IS_SYSEVENT(btn))
        {
            if (rb->default_event_handler(btn) == SYS_USB_CONNECTED)
            {
                shut = 1;
                return;
            }
            continue;
        }""")
    start = code.index("#if CONFIG_KEYPAD == IPOD_4G_PAD\n        if(rb->button_hold())")
    stop = code.index("#ifndef HAVE_WHEEL_POSITION", start)
    code = code[:start] + code[stop:]
    start = code.index("#if defined(HAVE_SCROLLWHEEL) && !defined(ROCKBOY_SCROLLWHEEL)")
    stop = code.index("#endif", start) + len("#endif")
    code = code[:start] + code[stop:]
    replace("        buttons |= btn;",
            """        if (btn == (BUTTON_MENU | BUTTON_REL))
        {
            rb->rock_stream_pause(true);
            if (do_user_menu() == USER_MENU_QUIT)
                shut = 1;
            else
                rb->rock_stream_pause(false);
            return;
        }
        buttons |= btn & BUTTON_MAIN;""")
    for name in ("B", "SELECT"):
        replace(f"pressed & options.{name})",
                f"(pressed & options.{name}) && "
                f"(buttons & options.{name}) == (unsigned)options.{name})")
        replace(f"released & options.{name})",
                f"(released & options.{name}) && "
                f"((buttons | released) & options.{name}) == (unsigned)options.{name})")
elif src.name == "emu.c":
    replace("    while(!shut)", "    long next_frame = *rb->current_tick;\n    unsigned int frame_clock = 0;\n    while(!shut)")
    replace("        if (options.sound || !plugbuf)",
            "        if (options.sound && pcm.buf)")
    replace("        frames++;", """        if (!shut)
        {
            frame_clock += HZ;
            next_frame += frame_clock / 60;
            frame_clock %= 60;
            long delay = next_frame - *rb->current_tick;
            if (delay > 0)
                rb->sleep(delay);
            else if (delay < -HZ / 2)
                next_frame = *rb->current_tick;
        }
        frames++;""")
elif src.name == "sound.c":
    replace("snd.quality=44100 / pcm.hz;", "snd.quality = MAX(1, 44100 / pcm.hz);")
    replace("            if (pcm.pos >= pcm.len)\n                rockboy_pcm_submit();",
            "            if (pcm.pos >= pcm.len && !rockboy_pcm_submit())\n                break;")

output.write_text(code)
