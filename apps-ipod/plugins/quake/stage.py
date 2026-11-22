#!/usr/bin/env python3
"""Apply checked platform changes to build copies of the stock Quake sources."""
import re
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
text = source.read_text()


def replace(old, new, count=1):
    global text
    if text.count(old) != count:
        raise SystemExit(f"{source}: expected {count} occurrence(s) of {old!r}")
    text = text.replace(old, new)


if source.name == "snd_mix_arm.S":
    start = text.index("        .text")
    text = text[:start] + re.sub(r";+ *(.*)", r"/* \1 */", text[start:])
    replace("bgt 1b", "bge 1b", count=2)
elif source.name == "wrappers.c":
    # The stock ARM math implementations have no SDL or libc dependency.
    trig = text[text.index("static unsigned long lastphase"):text.index("void fatal")]
    trig += text[text.index("double sin_wrapper"):text.index("// Total hack.")]
    math = text[text.index("/* A union which permits"):]
    names = {
        "sin_wrapper": "quake_sin", "cos_wrapper": "quake_cos",
        "tan_wrapper": "quake_tan", "atan_wrapper": "quake_atan",
        "atan2_wrapper": "quake_atan2", "sqrt_wrapper": "quake_sqrt",
        "fabs_wrapper": "quake_fabs", "pow_wrapper": "quake_pow",
        "floor_wrapper": "quake_floor", "ceil_wrapper": "quake_ceil",
        "rb_log": "quake_logarithm", "rb_exp": "quake_exp",
    }
    text = '#include "fixedpoint.h"\n#include "quake_libc.h"\n' + trig + math
    replace("float fmod(float x, float y)", "static float quake_fmod(float x, float y)")
    replace("return floor_wrapper(n) + 1.0;", "return -floor_wrapper(-n);")
    for old, new in names.items():
        text = re.sub(rf"\b{old}\b", new, text)
else:
    text = text.replace('#include "SDL.h"', '#include "quake_sdl.h"')
    text = text.replace('#include "SDL_byteorder.h"', '')
    text = text.replace('#include <setjmp.h>', '')
    text = text.replace('#include <math.h>', '')
    text = text.replace('#include <stdio.h>', '')
    text = text.replace('#include <stdlib.h>', '')
    text = text.replace('#include <string.h>', '')
    text = text.replace('#include <ctype.h>', '')
    text = text.replace('#include <malloc.h>', '')
    text = text.replace('#include "lib/stdio_compat.h"', '')
    text = text.replace('#include "stdio_compat.h"', '')

    if source.name in {"model.c", "snd_mem.c", "zone.c"}:
        # The converter only sees fixed PCM arrays. The engine and its
        # allocators no longer run inside an SDL audio callback.
        text = re.sub(r"(?m)^[ \t]*rb->mutex_(?:lock|unlock|init)\(.*\);\n", "", text)
        text = re.sub(r"(?m)^[ \t]*if\(!snd_mutex_init\)\n[ \t]*\{\n"
                      r"[ \t]*snd_mutex_init = 1;\n[ \t]*\}\n", "", text)
        text = re.sub(r"(?m)^[ \t]*(?:(?:extern|static) )?(?:struct mutex "
                      r"(?:snd_mutex|zone_mutex)|int snd_mutex_init[^;]*);\n", "", text)

if source.name == "quakedef.h":
    # PocketQuake fixed-point expressions are sensitive to native long width.
    replace("#define FIXEDPOINT_OPT", "#if (CONFIG_PLATFORM & PLATFORM_NATIVE)\n"
            "#define FIXEDPOINT_OPT")
    replace("#define USE_PQ_OPT5\n#endif", "#define USE_PQ_OPT5\n#endif\n#endif")
    replace('#include "world.h"', '#include "world.h"\n'
            'qboolean SV_RecursiveHullCheck(hull_t *, int, float, float,'
            ' vec3_t, vec3_t, trace_t *);')
elif source.name == "mathlib.h":
    replace("    long i;", "    uint32_t i;")
    replace("i  = * ( long * ) &y;", "memcpy(&i, &y, sizeof(i));")
    replace("y  = * ( float * ) &i;", "memcpy(&y, &i, sizeof(y));")
elif source.name == "progs.h":
    replace("extern\tchar\t\t\t*pr_strings;",
            "extern char *pr_strings;\nchar *quake_pr_string(int value);\n"
            "int quake_pr_string_id(const char *text);")
    replace("(pr_strings + *(string_t *)&pr_globals[o])",
            "quake_pr_string(*(string_t *)&pr_globals[o])")
    replace("(pr_strings + *(string_t *)&((float*)&e->v)[o])",
            "quake_pr_string(*(string_t *)&((float*)&e->v)[o])")
elif source.name == "common.c":
    start = text.index("/*\n * This function was NOT originally thread-safe")
    end = text.index("/*", text.index("return buf;", start))
    text = text[:start] + """/* The foreground owns engine file reads and cache allocations. */
byte *COM_LoadStackFile(char *path, void *buffer, int bufsize)
{
    loadbuf = buffer;
    loadsize = bufsize;
    return COM_LoadFile(path, 4);
}

""" + text[end:]
    replace("dpackfile_t             info[MAX_FILES_IN_PACK];",
            "static dpackfile_t info[MAX_FILES_IN_PACK];")
    replace("if (Sys_FileOpenRead (packfile, &packhandle) == -1)",
            "int extent = Sys_FileOpenRead(packfile, &packhandle);\n"
            "\tif (extent < 0)")
    replace("Sys_FileRead (packhandle, (void *)&header, sizeof(header));",
            "if (Sys_FileRead(packhandle, &header, sizeof(header)) != sizeof(header))\n"
            "\t\tSys_Error(\"Truncated PACK header\");")
    replace("if (numpackfiles > MAX_FILES_IN_PACK)",
            "if (header.dirofs < sizeof(header) || header.dirofs > extent ||\n"
            "\t    header.dirlen <= 0 || header.dirlen > extent - header.dirofs ||\n"
            "\t    header.dirlen % sizeof(dpackfile_t) || numpackfiles > MAX_FILES_IN_PACK)")
    replace("Sys_FileRead (packhandle, (void *)info, header.dirlen);",
            "if (Sys_FileRead(packhandle, info, header.dirlen) != header.dirlen)\n"
            "\t\tSys_Error(\"Truncated PACK directory\");")
    replace("strcpy (newfiles[i].name, info[i].name);", "")
    replace("newfiles[i].filelen = LittleLong(info[i].filelen);",
            "newfiles[i].filelen = LittleLong(info[i].filelen);\n"
            "\t\tif (newfiles[i].filepos < sizeof(header) ||\n"
            "\t\t    newfiles[i].filepos > extent ||\n"
            "\t\t    newfiles[i].filelen < 0 || newfiles[i].filelen > QUAKE_MAX_FILE ||\n"
            "\t\t    newfiles[i].filelen > extent - newfiles[i].filepos ||\n"
            "\t\t    (newfiles[i].filepos < header.dirofs + header.dirlen &&\n"
            "\t\t     newfiles[i].filepos + newfiles[i].filelen > header.dirofs) ||\n"
            "\t\t    !memchr(info[i].name, 0, sizeof(info[i].name)))\n"
            "\t\t\tSys_Error(\"Invalid PACK entry\");\n"
            "\t\tstrcpy(newfiles[i].name, info[i].name);")
    replace("if (*file)\n\t\t\t\t\t\t\tfseek (*file, pak->files[i].filepos, SEEK_SET);",
            "if (quake_fslice(*file, pak->files[i].filepos, pak->files[i].filelen))"
            "\n\t\t\t\t\t\t\t(void)0;")
    replace("if(len < 1)\n            rb->splashf(HZ, \"suspicious length %d\", len);",
            "if (len < 0 || len > QUAKE_MAX_FILE)\n"
            "\t\tSys_Error(\"Invalid asset size: %s\", path);")
    replace("Sys_FileRead (h, buf, len);",
            "if (Sys_FileRead(h, buf, len) != len)\n"
            "\t\tSys_Error(\"Truncated asset: %s\", path);")
elif source.name == "wad.c":
    replace("header = (wadinfo_t *)wad_base;",
            "if (com_filesize < (int)sizeof(*header))\n"
            "\t\tSys_Error(\"Truncated gfx.wad header\");\n"
            "\theader = (wadinfo_t *)wad_base;")
    replace("wad_lumps = (lumpinfo_t *)(wad_base + infotableofs);",
            "if (infotableofs < (int)sizeof(*header) || infotableofs % 4 ||\n"
            "\t    infotableofs > com_filesize || wad_numlumps <= 0 ||\n"
            "\t    wad_numlumps > (com_filesize - infotableofs) / (int)sizeof(lumpinfo_t))\n"
            "\t\tSys_Error(\"Invalid gfx.wad directory\");\n"
            "\twad_lumps = (lumpinfo_t *)(wad_base + infotableofs);")
    replace("if (lump_p->type == TYP_QPIC)\n\t\t\tSwapPic ( (qpic_t *)(wad_base + lump_p->filepos));",
            """if (lump_p->filepos < (int)sizeof(*header) ||
            lump_p->filepos > infotableofs || lump_p->size < 0 ||
            lump_p->size > infotableofs - lump_p->filepos)
            Sys_Error("Invalid gfx.wad lump");
        if (!strcmp(lump_p->name, "conchars") && lump_p->size < 128 * 128)
            Sys_Error("Truncated gfx.wad font");
        if (lump_p->type == TYP_QPIC)
        {
            if (lump_p->filepos % 4 || lump_p->size <= 8)
                Sys_Error("Invalid gfx.wad picture");
            qpic_t *pic = (qpic_t *)(wad_base + lump_p->filepos);
            SwapPic(pic);
            if (pic->width <= 0 || pic->height <= 0 ||
                pic->width > (lump_p->size - 8) / pic->height)
                Sys_Error("Truncated gfx.wad picture");
        }""")
elif source.name == "net_main.c":
    for name in ("Slist_Send", "Slist_Poll"):
        replace(f"{name}(void)", f"{name}(void *context)", count=2)
        replace(f"{name}(void *context)\n{{", f"{name}(void *context)\n{{\n"
                "\t(void)context;")
elif source.name == "cl_demo.c":
    replace("getc(cls.demofile)", "fgetc(cls.demofile)")
    start = text.index("\twhile ((c = fgetc(cls.demofile))")
    end = text.index("// ZOID", start)
    text = text[:start] + """    for (int digits = 0; ; digits++)
    {
        c = fgetc(cls.demofile);
        if (c == '\\n')
            break;
        if (c == '-' && digits == 0)
            neg = true;
        else if (digits < 9 && c >= '0' && c <= '9')
            cls.forcetrack = cls.forcetrack * 10 + c - '0';
        else
        {
            CL_StopPlayback();
            return;
        }
    }
    if (neg)
        cls.forcetrack = -cls.forcetrack;
""" + text[end:]
    replace("fread (&net_message.cursize, 4, 1, cls.demofile);",
            "if (fread(&net_message.cursize, 4, 1, cls.demofile) != 1)\n"
            "\t\t{\n\t\t\tCL_StopPlayback();\n\t\t\treturn 0;\n\t\t}")
    replace("r = fread (&f, 4, 1, cls.demofile);",
            "if (fread(&f, 4, 1, cls.demofile) != 1)\n"
            "\t\t\t{\n\t\t\t\tCL_StopPlayback();\n\t\t\t\treturn 0;\n\t\t\t}")
    replace("if (net_message.cursize > MAX_MSGLEN)",
            "if (net_message.cursize < 0 || net_message.cursize > MAX_MSGLEN)")
elif source.name == "host.c":
    # The launcher has a fixed argv and does not expose command-line VCR I/O.
    start = text.index("void Host_InitVCR")
    end = text.rindex("/*", start, text.index("void Host_Init (", start))
    text = text[:start] + text[end:]
    replace("\tHost_InitVCR (parms);", "")
    replace("if (!host_basepal)", "if (!host_basepal || com_filesize != 768)")
    replace("if (!host_colormap)", "if (!host_colormap || com_filesize < 8196)")
elif source.name == "r_bsp.c":
    replace("void R_RecursiveWorldNode (mnode_t *node, int clipflags)\n{",
            "void R_RecursiveWorldNode (mnode_t *node, int clipflags)\n"
            "{\n\tquake_check_events();")
    replace("btofpoly_t\tbtofpolys[", "static btofpoly_t btofpolys[")
    replace("mvertex_t\tbverts[", "static mvertex_t bverts[")
    replace("bedge_t\t\tbedges[MAX_BMODEL_EDGES], *pbedge;",
            "static bedge_t bedges[MAX_BMODEL_EDGES];\n\tbedge_t *pbedge;")
elif source.name == "r_alias.c":
    replace("finalvert_t\t\tfinalverts[", "static finalvert_t finalverts[")
    replace("auxvert_t\t\tauxverts[", "static auxvert_t auxverts[")
elif source.name == "d_polyse.c":
    replace("spanpackage_t\tspans[", "static spanpackage_t spans[", count=2)
elif source.name == "r_main.c":
    replace("edge_t\tledges[", "static edge_t ledges[")
    replace("surf_t\tlsurfs[", "static surf_t lsurfs[")
    replace("byte\twarpbuffer[", "static byte warpbuffer[")
    start = text.index("\tint\t\tdelta;", text.index("void R_RenderView ("))
    end = text.index("\tif ( Hunk_LowMark()", start)
    text = text[:start] + text[end:]
elif source.name == "r_edge.c":
    replace("byte\tbasespans[", "static byte basespans[")
    replace("current_iv = iv;", "if ((iv & 7) == 0)\n"
            "\t\t\tquake_check_events();\n\t\tcurrent_iv = iv;", count=2)
elif source.name == "d_edge.c":
    replace("if (!s->spans)", "quake_check_events();\n\t\t\tif (!s->spans)",
            count=2)
elif source.name == "sbar.c":
    replace("sprintf (&scoreboardtext[i][1],", "quake_snprintf(&scoreboardtext[i][1],"
            " sizeof(scoreboardtext[i]) - 1,")
elif source.name == "snd_mix.c":
    replace("(end - paintedtime) * sizeof(int16_t)",
            "2 * (end - paintedtime) * sizeof(int16_t)")
    replace("    paintbuffer = shm->buffer;", "")
    replace("    end = endtime;",
            "    end = endtime;\n    if (end <= paintedtime)\n        return;")
    replace("memset(paintbuffer,", "memset(shm->buffer,")
    replace("                if (sc->width == 1)",
            "                paintbuffer = (int16_t *)shm->buffer +\n"
            "                    2 * (ltime - paintedtime);\n"
            "                if (sc->width == 1)")
    replace("        if (!sc)\n", "        if (!sc || sc->length <= 0 || sc->loopstart >= sc->length)\n")
elif source.name == "snd_mix_generic.c":
    replace("__ARM_ARCH__5TEJ__", "__ARM_ARCH_5TEJ__")
    replace("i<count ; i+=2", "i<count ; i++")
elif source.name == "vid_sdl.c":
    # No heap/stack growth for dirty rectangles and clip the direct disc icon.
    start = text.index("    SDL_Rect *sdlrects;")
    end = text.index("\n}", start)
    text = text[:start] + """    for (vrect_t *rect = rects; rect; rect = rect->pnext)
        SDL_UpdateRect(screen, rect->x, rect->y, rect->width, rect->height);""" + text[end:]
    replace("    offset = (Uint8 *)screen->pixels + y*screen->pitch + x;",
            "    if (x < 0 || y < 0 || width > screen->w - x ||\n"
            "        height > screen->h - y) return;\n"
            "    offset = (Uint8 *)screen->pixels + y*screen->pitch + x;")
elif source.name in {"host_cmd.c", "menu.c"}:
    text = re.sub(r'fscanf\s*\(f, "%s\\n",\s*(\w+)\);',
                  lambda match: f'quake_scan_line(f, "%s\\n", {match[1]}, sizeof({match[1]}));', text)
    # Numeric fscanf calls in this port also read complete lines.
    if source.name == "host_cmd.c":
        replace("if (!sv.active)\n\t{\n\t\tCon_Printf (\"Couldn't load map\\n\");",
                "if (!sv.active)\n\t{\n\t\tfclose(f);\n"
                "\t\tCon_Printf (\"Couldn't load map\\n\");", count=2)

if source.name in {"pr_cmds.c", "pr_edict.c", "pr_exec.c", "sv_main.c",
                    "sv_phys.c", "host_cmd.c"}:
    # QuakeC strings are 32-bit offsets in progs.dat. Negative values name
    # live engine strings, which can be outside the heap on a 64-bit host.
    text = re.sub(r"(ED_NewString \(s\)|[A-Za-z_][\w.>\-]*) - pr_strings",
                  r"quake_pr_string_id(\1)", text)
    text = re.sub(r"pr_strings\s*\+\s*((?:\w+->_?v\.|\w+->)(?:classname|netname|"
                  r"weaponmodel|message|model|string))\b",
                  r"quake_pr_string(\1)", text)
    text = re.sub(r"pr_strings\[((?:\w+->v\.)?\w+->string|\w+->v\.model)\]",
                  r"*quake_pr_string(\1)", text)
    text = text.replace("pr_strings + val->string", "quake_pr_string(val->string)")
    text = text.replace("pr_strings+sv.edicts->v.message",
                        "quake_pr_string(sv.edicts->v.message)")
    text = text.replace("sprintf (message, quake_pr_string(sv.edicts->v.message));",
                        'sprintf (message, "%s", quake_pr_string(sv.edicts->v.message));')
    if source.name == "sv_main.c":
        replace("sprintf (localmodels[i],", "quake_snprintf(localmodels[i],"
                " sizeof(localmodels[i]),")
    if source.name == "pr_edict.c":
        replace("char\t\t\t*pr_strings;", """char *pr_strings;
static const char *engine_strings[8192];
static int engine_strings_used;

char *quake_pr_string(int value)
{
    if (value >= 0 && value < progs->numstrings)
        return pr_strings + value;
    if (value < 0 && value >= -engine_strings_used)
        return (char *)engine_strings[-value - 1];
    Sys_Error("Invalid QuakeC string");
    return NULL;
}

int quake_pr_string_id(const char *value)
{
    uintptr_t offset = (uintptr_t)value - (uintptr_t)pr_strings;
    if (offset < (unsigned)progs->numstrings)
        return offset;

    for (int i = 0; i < engine_strings_used; i++)
        if (engine_strings[i] == value)
            return -i - 1;

    if (engine_strings_used == ARRAYLEN(engine_strings))
        Sys_Error("QuakeC string table exhausted");
    engine_strings[engine_strings_used++] = value;
    return -engine_strings_used;
}""")
        replace("pr_strings = (char *)progs + progs->ofs_strings;",
                "engine_strings_used = 0;\n\t"
                "pr_strings = (char *)progs + progs->ofs_strings;")
        replace("sizeof(void *)/4", "1")
        replace("pr_edict_size = progs->entityfields * 4 + sizeof (edict_t) - sizeof(entvars_t);",
                "if (progs->entityfields < (int)sizeof(entvars_t) / 4 ||"
                " progs->entityfields > 4096)\n"
                "\t\tSys_Error(\"Invalid QuakeC entity size\");\n"
                "\tpr_edict_size = ALIGN_UP(offsetof(edict_t, v) + progs->entityfields * 4,"
                " __alignof__(edict_t));")

if not output.exists() or output.read_text() != text:
    output.write_text(text)
