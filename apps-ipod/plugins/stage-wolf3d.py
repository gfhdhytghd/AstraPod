#!/usr/bin/env python3
"""Stage stock SDL and Wolf3D changes at their SDK boundaries."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
code = source.read_text()


def replace(old, new):
    global code
    if code.count(old) != 1:
        raise SystemExit(f"{source}: expected one match for {old!r}")
    code = code.replace(old, new)


def span(start, end, new):
    global code
    first = code.index(start)
    last = code.index(end, first + len(start))
    code = code[:first] + new + code[last:]


match source.name:
    case "SDL_config_rockbox.h":
        replace("#include <tlsf.h>", '#include <tlsf.h>\n#include "lib/stdio_compat.h"\n#include "wolf3d-bridge.h"')
        for old, new in (
            ("#define atexit rb_atexit\n", ""),
            ("#define exit rb_exit\n", ""),
            ("#define rb_atexit rbsdl_atexit\n", ""),
            ("#define calloc tlsf_calloc", "#define calloc wolf_calloc"),
            ("#define free tlsf_free", "#define free wolf_free"),
            ("#define malloc tlsf_malloc", "#define malloc wolf_malloc"),
            ("#define realloc tlsf_realloc", "#define realloc wolf_realloc"),
            ("#define WAV_MUSIC\n", ""),
            ("#define MID_MUSIC\n", ""),
            ("#define USE_TIMIDITY_MIDI\n", ""),
            ("#define FILENAME_MAX MAX_PATH", "#undef FILENAME_MAX\n#define FILENAME_MAX MAX_PATH"),
            ("#define strstr rb->strstr", "#undef strstr\n#define strstr rb->strstr"),
            ("char *strcpy_wrapper(char *dest, const char *src);",
             "char *strdup_wrapper(const char *src);\nchar *strcpy_wrapper(char *dest, const char *src);"),
            ("float atof_wrapper (char *str);", "float atof_wrapper (const char *str);"),
            ("(*(ptr+3) << 24)", "((uint32_t)*(ptr+3) << 24)"),
        ):
            replace(old, new)
    case "pluginlib_bmp.c":
        code = code[:code.index('#include "wrappers.h"')]
    case "stdio_compat.c":
        replace('#include "errno.h"\n', "")
        replace('        rb->splashf(HZ, "lseek() failed: %d fd:%d", errno, stream->fd);\n', "")
        replace("    ssize_t ret = rb->read(stream->fd, (char *)ptr, size*nmemb);",
                "    if (!size || nmemb > SIZE_MAX / size)\n        return 0;\n"
                "    ssize_t ret = rb->read(stream->fd, ptr, size * nmemb);")
        replace("    return ret / size;\n}", "    return ret < 0 ? 0 : (size_t)ret / size;\n}")
    case "wrappers.c":
        replace("float atof_wrapper (char *str)", "float atof_wrapper (const char *str)")
        span("/* stolen from doom */", "int vprintf(", """
int printf_wrapper(const char *fmt, ...)
{
    /* Stock diagnostics can also run inside the mixer worker. */
    (void)fmt;
    return 0;
}

""")
        span("    rb->splash(HZ, \"FATAL\");", "    exit(1);", """    va_start(ap, fmt);
    char buf[80];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wolf_error(buf);
""")
    case "SDL_sysmutex.c":
        replace("#include \"SDL_systhread_c.h\"", "")
    case "SDL_syssem.c":
        replace("timeout / (1000 / HZ)", "timeout == SDL_MUTEX_MAXWAIT ?\n"
                "                          TIMEOUT_BLOCK : (int)(timeout / (1000 / HZ))")
    case "SDL_rockboxvideo.c":
        span("static void rb_release", "static SDL_VideoDevice *ROCKBOX_CreateDevice", """
void ROCKBOX_WarpWMCursor(_THIS, Uint16 x, Uint16 y) {}
void ROCKBOX_MoveWMCursor(_THIS, int x, int y) {}
int ROCKBOX_ShowWMCursor(_THIS, WMcursor *cursor) { return 0; }

void ROCKBOX_PumpEvents(_THIS)
{
    wolf_service();
}

""")
        span("static void init_keymap", "SDL_Surface *ROCKBOX_SetVideoMode", "")
        replace("    if(bpp != LCD_DEPTH)", "    if (bpp != LCD_DEPTH || width != 320 || height != 200)")
        replace("    init_keymap();\n\n", "")
        span("    /* rotate 90deg", "/*      printf", "")
        span("#if LCD_PIXELFORMAT == RGB565SWAPPED\nstatic void flip_pixels", "int ROCKBOX_SetColors(_THIS, int firstcolor", """
static void ROCKBOX_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
    (void)numrects;
    (void)rects;
    if (this->screen->pixels)
    {
        struct bitmap input = {
            .width = this->screen->w, .height = this->screen->h,
            .data = this->screen->pixels,
        };
        struct bitmap output = {
            .width = LCD_WIDTH, .height = LCD_HEIGHT, .data = (void *)lcd_fb,
        };
        simple_resize_bitmap(&input, &output);
    }
    rb->lcd_update();
}

""")
        replace("    struct viewport *vp_main", "    rb->lcd_set_viewport(NULL);\n"
                "    struct viewport *vp_main")
    case "wl_main.c":
        replace("    VL_MungePic (signon,320,200);\n"
                "    VL_MemToScreen (signon,320,200,0,0);", "")
        replace("    atexit(SDL_Quit);\n", "")
        replace("    int i = 0;\n    for (map = wolfdigimap;", "    for (map = wolfdigimap;")
        replace('        rb->splashf(0, "Preparing sounds (%d/%d)", ++i, ARRAYLEN(wolfdigimap)/3);\n', "")
        replace("        SD_PrepareSound(map[1]);\n        rb->yield();", "")
        replace("    InitGame();\n\n    DemoLoop();",
                "    InitGame();\n    wolf_allow_config_save();\n\n    DemoLoop();")
        span("void Quit (const char *errorStr, ...)", "//===========================================================================", """
void Quit (const char *errorStr, ...)
{
    if (errorStr)
    {
        char error[256];
        va_list args;
        va_start(args, errorStr);
        vsnprintf(error, sizeof(error), errorStr, args);
        va_end(args);
        wolf_error(error);
    }
    wolf_service();
    if (wolf_can_save_config())
        WriteConfig();
    exit(0);
}

""")
    case "wl_menu.c":
        replace('snprintf(configdir, sizeof(configdir), "%s" WOLFDIR, homedir);',
                'snprintf(configdir, sizeof(configdir), "%s", WOLFDIR);')
        span("void *my_stat", "\nvoid\nCheckForEpisodes", """
static int my_stat(const char *path, int *buf)
{
    (void)buf;
    return rb->file_exists(path) || rb->dir_exists(path) ? 0 : -1;
}
""")
    case "id_sd.c":
        replace("    if(Mix_OpenAudio(param_samplerate, AUDIO_S16, 2, param_audiobuffer))",
                "    if (!wolf_audio_rate())\n        return;\n"
                "    if(Mix_OpenAudio(param_samplerate, AUDIO_S16SYS, 2, param_audiobuffer))")
        replace("    if(!(OPL_ptr = YM3812Init((void*)1,3579545,param_samplerate)))\n"
                "    {\n        //printf(\"Unable to create virtual OPL!!\\n\");\n    }",
                "    if(!(OPL_ptr = YM3812Init((void*)1,3579545,param_samplerate)))\n"
                "        Quit(\"Unable to allocate the OPL synthesizer\");")
        replace("static Mix_Chunk *SoundChunks[ STARTMUSIC - STARTDIGISOUNDS];\n"
                "static byte      *SoundBuffers[STARTMUSIC - STARTDIGISOUNDS];",
                "static Mix_Chunk *channel_chunks[MIX_CHANNELS];\n"
                "static int channel_sounds[MIX_CHANNELS];")
        span("void SD_PrepareSound(int which)", "int SD_PlayDigitized",
             '#include "wolf3d-digitized.inc"\n\n')
        span("    int channel = SD_GetChannelForDigi(which);",
             "    return channel;\n#endif", """    int channel = SD_GetChannelForDigi(which);
    if (channel < 0 || channel >= MIX_CHANNELS)
        return -1;
    Mix_Chunk *sample = channel_chunks[channel];
    if (!sample || channel_sounds[channel] != which)
    {
        Mix_FreeChunk(sample);
        channel_chunks[channel] = sample = wolf_decode_digi(which);
        channel_sounds[channel] = which;
    }
    if (!sample)
        return -1;
    SD_SetPosition(channel, leftpos, rightpos);
    DigiPlaying = true;
    if (Mix_PlayChannel(channel, sample, 0) < 0)
        return -1;

""")
        replace("            return channel + 1;",
                "            if (channel >= 0)\n                return channel + 1;")
        replace("        }\n\n        return(true);\n    }\n\n    if (SoundMode",
                "        }\n    }\n\n    if (SoundMode")
        replace("    return (int) (sqHackPtr-sqHack);",
                "    return sqHack ? (int)(sqHackPtr - sqHack) : 0;")
        replace("    if ((mode == sdm_AdLib) && !AdLibPresent)",
                "    if (!AdLibPresent)\n        mode = sdm_Off;\n"
                "    if ((mode == sdm_AdLib) && !AdLibPresent)")
        replace("    DigiList = (digiinfo *) malloc(NumDigi * sizeof(digiinfo));",
                "    DigiList = calloc(NumDigi, sizeof(*DigiList));\n"
                "    if (NumDigi) { CHECKMALLOCRESULT(DigiList); }")
        replace("if((int) DigiList[i].startpage >= ChunksInFile - 1)",
                "if(DigiList[i].startpage >= (unsigned)(ChunksInFile - 1 - PMSoundStart))")
        span("    for(int i = 0; i < STARTMUSIC - STARTDIGISOUNDS; i++)",
             "    free(DigiList);", """    for (int i = 0; i < MIX_CHANNELS; ++i)
        Mix_FreeChunk(channel_chunks[i]);

""")
    case "fmopl_gpl.c":
        span("static TIMER_CALLBACK( cymfile_callback )",
             "/* lock/unlock for common table */", "")
        # These unused legacy callbacks declare an int context.
        for field in ("IRQParam", "UpdateParam"):
            code = code.replace(f"(OPL->{field},", f"((int)(intptr_t)OPL->{field},")
    case "id_ca.c":
        span("    char str[30];", "\n}", '    Quit("Cannot open %s", string);')
        replace("static void CAL_HuffExpand(byte *source, byte *dest, int32_t length, huffnode *hufftable)",
                "static void CAL_HuffExpand(const byte *source, size_t bytes, byte *dest,\n"
                "                           int32_t length, huffnode *hufftable)")
        replace("    byte val = *source++;\n    byte mask = 1;",
                "    const byte *source_end = source + bytes;\n"
                "    byte val = 0;\n    byte mask = 0;")
        replace("    while(1)\n    {\n        if(!(val & mask))",
                "    while(1)\n    {\n        if (!mask)\n        {\n"
                "            if (source == source_end)\n"
                "                Quit(\"Truncated VGAGRAPH chunk\");\n"
                "            val = *source++;\n            mask = 1;\n        }\n"
                "        if(!(val & mask))")
        span("        if(mask==0x80)", "        if(nodeval<256)", "        mask <<= 1;\n\n")
        replace("            huffptr = hufftable + (nodeval - 256);",
                "            if (nodeval >= 256 + 255)\n"
                "                Quit(\"Invalid VGADICT node\");\n"
                "            huffptr = hufftable + (nodeval - 256);")
        replace("    compseg=(byte *) malloc(chunkcomplen);",
                "    if (chunkcomplen <= 0)\n"
                "        Quit(\"Invalid VGAGRAPH picture table\");\n"
                "    compseg=(byte *) malloc(chunkcomplen);")
        replace("    read (grhandle,compseg,chunkcomplen);",
                "    if (read(grhandle, compseg, chunkcomplen) != chunkcomplen)\n"
                "        Quit(\"Truncated VGAGRAPH picture table\");")
        replace("CAL_HuffExpand(compseg, (byte*)pictable,",
                "CAL_HuffExpand(compseg, chunkcomplen, (byte*)pictable,")
        replace("void CAL_ExpandGrChunk (int chunk, int32_t *source)",
                "static void CAL_ExpandGrChunk(int chunk, int32_t *source, int32_t compressed)")
        replace("        expanded = *source++;",
                "        if (compressed <= (int32_t)sizeof(*source))\n"
                "            Quit(\"Truncated VGAGRAPH chunk header\");\n"
                "        compressed -= sizeof(*source);\n"
                "        expanded = *source++;")
        replace("    grsegs[chunk]=(byte *) malloc(expanded);",
                "    if (expanded <= 0)\n        Quit(\"Invalid VGAGRAPH decoded size\");\n"
                "    grsegs[chunk]=(byte *) malloc(expanded);")
        replace("CAL_HuffExpand((byte *) source, grsegs[chunk]",
                "CAL_HuffExpand((byte *) source, compressed, grsegs[chunk]")
        replace("    CAL_ExpandGrChunk (chunk,source);",
                "    CAL_ExpandGrChunk(chunk, source, compressed);")
        replace("        read(grhandle,bufferseg,compressed);",
                "        if (compressed <= 0 || read(grhandle, bufferseg, compressed) != compressed)\n"
                "            Quit(\"Invalid VGAGRAPH chunk extent\");")
        replace("        read(grhandle,source,compressed);",
                "        if (read(grhandle, source, compressed) != compressed)\n"
                "            Quit(\"Truncated VGAGRAPH chunk\");")
        replace("    bigbufferseg=malloc(compressed);\n    CHECKMALLOCRESULT(bigbufferseg);\n"
                "    read(grhandle,bigbufferseg,compressed);",
                "    if (pos < 0 || compressed <= (int32_t)sizeof(*source))\n"
                "        Quit(\"Invalid VGAGRAPH screen extent\");\n"
                "    bigbufferseg=malloc(compressed);\n    CHECKMALLOCRESULT(bigbufferseg);\n"
                "    if (read(grhandle, bigbufferseg, compressed) != compressed)\n"
                "        Quit(\"Truncated VGAGRAPH screen\");")
        replace("    byte *pic = (byte *) malloc(64000);",
                "    if (expanded != 320 * 200)\n"
                "        Quit(\"Invalid VGAGRAPH screen size\");\n"
                "    byte *pic = (byte *) malloc(expanded);")
        replace("CAL_HuffExpand((byte *) source, pic, expanded, grhuffman);",
                "CAL_HuffExpand((byte *) source, compressed - sizeof(*source),\n"
                "                   pic, expanded, grhuffman);")
        replace("    read(handle, grhuffman, sizeof(grhuffman));",
                "    if (read(handle, grhuffman, sizeof(grhuffman)) != sizeof(grhuffman))\n"
                "        Quit(\"Truncated VGADICT\");")
        replace("    byte data[lengthof(grstarts) * 3];\n    read(handle, data, sizeof(data));",
                "    byte data[lengthof(grstarts) * 3];\n"
                "    memset(data, 0xff, sizeof(data));\n"
                "    if (headersize < 0 || headersize > (long)sizeof(data) ||\n"
                "        headersize % 3 || read(handle, data, headersize) != headersize)\n"
                "        Quit(\"Invalid VGAHEAD extent\");")
        replace("    read(handle, tinf, length);",
                "    if (read(handle, tinf, length) != length)\n"
                "        Quit(\"Truncated MAPHEAD\");")
        replace("        if (pos<0)", "        if (pos <= 0)")
        replace("        lseek(maphandle,pos,SEEK_SET);\n"
                "        read (maphandle,(memptr)mapheaderseg[i],sizeof(maptype));",
                "        wolf_service();\n"
                "        if (lseek(maphandle, pos, SEEK_SET) != pos ||\n"
                "            read(maphandle, mapheaderseg[i], sizeof(maptype)) != sizeof(maptype))\n"
                "            Quit(\"Truncated GAMEMAPS header\");")
        replace("    mapon = mapnum;",
                "    if (mapnum < 0 || mapnum >= NUMMAPS || !mapheaderseg[mapnum] ||\n"
                "        mapheaderseg[mapnum]->width != 64 || mapheaderseg[mapnum]->height != 64)\n"
                "        Quit(\"Invalid GAMEMAPS map\");\n"
                "    mapon = mapnum;")
        replace("    void* ptr;\n    if (!CA_LoadFile(fname, &ptr))\n        CA_CannotOpen(fname);\n"
                "    audiostarts = (int32_t*)ptr;",
                "    size_t bytes = (NUMSNDCHUNKS + 1) * sizeof(*audiostarts);\n"
                "    int header = open(fname, O_RDONLY | O_BINARY);\n"
                "    if (header < 0)\n        CA_CannotOpen(fname);\n"
                "    audiostarts = malloc(bytes);\n    CHECKMALLOCRESULT(audiostarts);\n"
                "    if (read(header, audiostarts, bytes) != (ssize_t)bytes)\n"
                "        Quit(\"Truncated AUDIOHED\");\n    close(header);")
        replace("    if (audiohandle == -1)\n        CA_CannotOpen(fname);",
                "    if (audiohandle == -1)\n        CA_CannotOpen(fname);\n"
                "    off_t end = rb->ffilesize(audiohandle);\n"
                "    for (int i = 0; i <= NUMSNDCHUNKS; ++i)\n"
                "        if (audiostarts[i] < 0 || audiostarts[i] > end ||\n"
                "            (i && audiostarts[i] < audiostarts[i - 1]))\n"
                "            Quit(\"Invalid AUDIOHED offset\");")
    case "id_pm.c":
        replace("    fread(&ChunksInFile, sizeof(word), 1, file);",
                "    if (fread(&ChunksInFile, sizeof(word), 1, file) != 1 ||\n"
                "        ChunksInFile < 2)\n        Quit(\"Invalid VSWAP header\");")
        replace("    fread(&PMSoundStart, sizeof(word), 1, file);",
                "    if (fread(&PMSoundStart, sizeof(word), 1, file) != 1 ||\n"
                "        PMSpriteStart <= 0 || PMSoundStart < PMSpriteStart ||\n"
                "        PMSoundStart >= ChunksInFile - 1)\n"
                "        Quit(\"Invalid VSWAP page ranges\");")
        replace("    fread(pageOffsets, sizeof(uint32_t), ChunksInFile, file);",
                "    if (fread(pageOffsets, sizeof(uint32_t), ChunksInFile, file) !=\n"
                "        (size_t)ChunksInFile)\n        Quit(\"Truncated VSWAP offsets\");")
        replace("    fread(pageLengths, sizeof(word), ChunksInFile, file);",
                "    if (fread(pageLengths, sizeof(word), ChunksInFile, file) !=\n"
                "        (size_t)ChunksInFile)\n        Quit(\"Truncated VSWAP lengths\");")
        span("    long pageDataSize", "    // Calculate total", """    uint32_t dataStart = pageOffsets[0];
    if (fileSize <= 0 || dataStart < 6u + 6u * ChunksInFile ||
        dataStart >= (unsigned long)fileSize || !pageOffsets[ChunksInFile - 1])
        Quit("Invalid VSWAP data extent");

    size_t pageDataSize = fileSize - dataStart;
    pageOffsets[ChunksInFile] = fileSize;
    uint32_t end = dataStart;
    int i;

    for (i = 0; i < ChunksInFile; i++)
    {
        uint32_t offset = pageOffsets[i];
        if (!offset)
            continue;
        if (offset < end || offset >= (unsigned long)fileSize ||
            pageLengths[i] > (unsigned long)fileSize - offset)
            Quit("Invalid VSWAP page %d", i);
        end = offset + pageLengths[i];
    }

""")
        replace("        fseek(file, pageOffsets[i], SEEK_SET);\n        fread(ptr, 1, size, file);",
                "        if (size > PMPageDataSize - (size_t)(ptr - (uint8_t *)PMPageData) ||\n"
                "            fseek(file, pageOffsets[i], SEEK_SET) ||\n"
                "            fread(ptr, 1, size, file) != size)\n"
                "            Quit(\"Invalid VSWAP page payload\");\n"
                "        wolf_service();")

if not output.exists() or output.read_text() != code:
    output.write_text(code)
