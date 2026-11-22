#!/usr/bin/env python3
"""Stage only the Duke3D source and SDL interfaces used by its SDK port."""
import re
import sys
from pathlib import Path

src, output = map(Path, sys.argv[1:])
source = src.read_text(encoding="latin-1")


def change(old, new, count=1):
    global source
    if source.count(old) != count:
        raise ValueError(f"{src}: expected {count} occurrences of {old!r}")
    source = source.replace(old, new)


def section(first, last, replacement):
    start = source.index(first)
    end = source.index(last, start)
    change(source[start:end], replacement)


if src.name == "SDL_config_rockbox.h":
    change('#include "lib/pluginlib_exit.h"',
           '#include "duke-build.h"\n#include "duke-sdl-names.h"')
    change('#define exit rb_exit', '#define exit duke_exit')
    change('#define rb_atexit rbsdl_atexit', 'int rb_atexit(void (*fn)(void));')
    change('void rb_exit(int rc);', 'void duke_exit(int rc) NORETURN_ATTR;')
    change('#define getenv SDL_getenv', '#define getenv(name) ((char *)NULL)')
    change('#define remove rb->remove', '#define remove duke_remove')
    change('#define sscanf SDL_sscanf', '')
    change('#define getchar() rb->sleep(2*HZ)', '#define getchar() (SDL_Delay(1), EOF)')
    change('#define sprintf sprintf_wrapper',
           '#define sprintf(dst, ...) snprintf(dst, __builtin_object_size(dst, 1), __VA_ARGS__)')
    change('#define fmod fmod_wrapper', '#define fmod duke_fmod')
    change('float fmod(float x, float y);', 'float duke_fmod(float x, float y);')
    change('int sprintf_wrapper(char*, const char*, ...);',
           'int vsprintf_wrapper(char *, const char *, va_list);')
elif src.name == "filesystem.c":
    section("typedef uint8_t grpIndexEntry_t", "void crc32_table_gen",
            '#include "grp.inc"\n\n')
    section("enum fileType_e", "int32_t kread16", "")
    section("int32_t klseek", "/* Internal LZW", "")
    section("    char  fullfilename[512]", "void   setGameDir",
            "    return kopen4load(filename, readfromGRP);\n}\n\n")
    change('return "/.rockbox/duke3d";', 'return DUKE_DIR;')
    change("    return grpSet.archives[grpID].crc32;",
           "    return (unsigned)grpID < (unsigned)grpSet.num ? groupefil_crc32[grpID] : 0;")
    change("    kread(fil, buffer, dasizeof * count);",
           "    if (kread(fil, buffer, dasizeof * count) != dasizeof * count)\n"
           '        Error(EXIT_FAILURE, "Truncated Duke3D save");')
elif src.name == "game.c":
    source = "void duke_tics(short offx, short offy, short color);\n" + source
    source = re.sub(r'^ +rb->splashf\(0, "point [0-9]+"\);\n', '', source,
                    flags=re.M)
    section('const char* const baseDir=', '#ifdef COMBINED_SDL',
            '''static int load_duke3d_groupfile(void)
{
    return initgroupfile(DUKE_DIR "/duke3d.grp") >= 0;
}

''')
    section('        Error(EXIT_SUCCESS, "Could not initialize',
            '\n    }',
            '        Error(EXIT_FAILURE, "Place a valid duke3d.grp in " DUKE_DIR);')
    section('                printf( "The content of your original BASE',
            '\n        }',
            '                Error(EXIT_FAILURE, "Unsupported Duke3D base GRP");')
    change("    totalmemory = Z_AvailHeap();",
           "    totalmemory = Z_AvailHeap();\n    memorycheckoveride = 1;")
    change('    loadpics("tiles000.art", "\\0");',
           '    if (loadpics("tiles000.art", "\\0") < 0)\n'
           '        Error(EXIT_FAILURE, "Duke3D: cannot initialize tile cache");')
    change("loadefs(userconfilename,mymembuf, 0);",
           "loadefs(userconfilename,mymembuf, 1);")
    change("   RTS_Init(ud.rtsname);", "   RTS_Init(NULL);")
    section("    extern int rbaud_underruns,", "    strcat(text, fps);",
            '    sprintf(fps, " %d", savedFps);\n')
    change("    extern bool printf_enabled;\n    printf_enabled = false;\n", "")
    source += """
bool duke_in_menu(void)
{
    return (ps[myconnectindex].gm & MODE_MENU) != 0;
}
"""
elif src.name == "tiles.c":
    # Leave room for the renderer, configuration nodes and mixer.
    change("    cachesize = max(artsize,1048576);",
           "    cachesize = 1280 * 1024;")
elif src.name == "premap.c":
    change("        while(Sound[globalskillsound].lock>=200) rb->yield();",
           """    {
        unsigned long deadline = (unsigned long)*rb->current_tick + HZ;
        while (Sound[globalskillsound].lock >= 200 &&
               TIME_BEFORE((unsigned long)*rb->current_tick, deadline))
            SDL_Delay(1);
    }""")
elif src.name == "engine.c":
    change("    rb->yield();", "    duke_service();")
elif src.name == "rts.c":
    change("if (SafeFileExists(filename))",
           "if (filename && SafeFileExists(filename))")
elif src.name == "global.c":
    section("int setup_homedir (void)", "uint8_t    CheckParm",
            "int setup_homedir(void) { return 0; }\n")
    change('   vprintf(error, argptr);', '''   if (errorType == EXIT_FAILURE)
   {
       char message[160];
       vsnprintf(message, sizeof(message), error, argptr);
       rb->splashf(3 * HZ, "%s", message);
   }''')
elif src.name == "gamedef.c":
    section("\t\t\t\tif(fp <= 0)", '\n\n\t\t\t\tj = kfilelength(fp);',
            '\t\t\t\tif (fp < 0)\n'
            '\t\t\t\t    Error(EXIT_FAILURE, "Missing Duke3D CON: %s", includedconfile);')
    change("\t\t\t\t*(textptr+j) = 0;\n\n\t\t\t\tkread(fp,(uint8_t  *)textptr,j);",
           """                if (j <= 0 || j >= (int)sizeof(hittype) - last_used_size ||
                    kread(fp, textptr, j) != j)
                    Error(EXIT_FAILURE, "Invalid Duke3D CON include");
                textptr[j] = 0;""")
    change("        kread(fp,(uint8_t  *)textptr,fs);",
           """        if (fs <= 0 || fs >= (int)sizeof(hittype) ||
            kread(fp, textptr, fs) != fs)
            Error(EXIT_FAILURE, "Invalid Duke3D CON size");""")
    section("\t\tif(conVersion != 13 &&", "\n\t}\n}\n",
            "")
    change('Error(EXIT_SUCCESS, "ERROR: CON(%s) not found.\\n", filenam);',
           'Error(EXIT_FAILURE, "Missing Duke3D CON: %s", filenam);')
    change('Error(EXIT_SUCCESS, "ERROR in CON(%s)\\n", filenam);',
           'Error(EXIT_FAILURE, "Invalid Duke3D CON: %s", filenam);')
elif src.name == "display.c":
    section("static SDL_Joystick *joystick", "uint8_t  _readlastkeyhit",
            '''void _joystick_init(void) {}
void _joystick_deinit(void) {}
int _joystick_update(void) { return 0; }
int _joystick_axis(int axis) { (void)axis; return 0; }
int _joystick_hat(int hat) { (void)hat; return 0; }
int _joystick_button(int button) { (void)button; return 0; }

''')
    section('SDL_Surface     *image;', '    if (daxdim', '')
    change("SDL_SaveBMP(surface, filename);", "(void)filename;")
    change("\t\thorizlookup2 = (int32_t*)malloc(j);",
           """        horizlookup2 = malloc(j);
        if (!horizlookup || !horizlookup2)
            Error(EXIT_FAILURE, "Duke3D: cannot allocate renderer tables");""")
elif src.name == "config.c":
    change("int32 ScreenHeight = LCD_HEIGHT;", "int32 ScreenHeight = 200;")
    change('   SCRIPT_GetNumber( scripthandle, "Screen Setup", "ScreenWidth",&ScreenWidth);',
           "   ScreenWidth = 320;")
    change('   SCRIPT_GetNumber( scripthandle, "Screen Setup", "ScreenHeight",&ScreenHeight);',
           "   ScreenHeight = 200;")
    change("   MixRate = RB_SAMPR;", "   MixRate = duke_pcm_rate();")
    change('   SCRIPT_GetNumber( scripthandle, "Sound Setup", "MusicDevice",&MusicDevice);',
           "   MusicDevice = NumSoundCards;")
elif src.name == "menues.c":
    change('            x = strget((320>>1),184,&ud.savegame[current_menu-360][0],19, 999 );',
           """            char *name = ud.savegame[current_menu - 360];
            if (!*name)
                snprintf(name, sizeof(ud.savegame[0]), "GAME %d",
                         current_menu - 359);
            x = strget((320>>1),184,name,19,999);""")
    change("    fclose(fil);\n\n    if(ud.multimode < 2)",
           """    if (fclose(fil) < 0)
    {
        ready2send = 1;
        ototalclock = totalclock;
        rb->splash(2 * HZ, "Duke3D: save failed");
        return -1;
    }

    if(ud.multimode < 2)""")
elif src.name == "wrappers.c":
    start = source.index("/* All floating-point numbers can")
    end = source.index("\n\nint\n__fpclassifyf", start)
    change(source[start:end], "#ifndef FP_NAN\n" + source[start:end] + "\n#endif\n")
    change("#define fpclassify(x)", "#undef fpclassify\n#define fpclassify(x)")
    section("int printf_wrapper(", "int sprintf_wrapper(",
            '''int printf_wrapper(const char *format, ...)
{
    (void)format;
    duke_service();
    return 0;
}

''')
elif src.name == "stdio_compat.c":
    change('#include "stdio_compat.h"', '')
    change("    f->unget_char = -1;\n\n    return f;",
           "    f->unget_char = -1;\n    f->error = 0;\n\n    return f;")
    change("return ret / size;", "return ret > 0 && size ? ret / size : 0;", 2)
    change('rb->splashf(HZ, "lseek() failed: %d fd:%d", errno, stream->fd);',
           'rb->splashf(HZ, "Duke3D: seek failed");')
    change("for (int i=0; i<size; i++)", "for (int i=0; i<size-1; i++)")
    change("return _fwrite_(buf, 1, len, stream);",
           "return len >= 0 && len < (int)sizeof(buf) ?\n"
           "        (int)_fwrite_(buf, 1, len, stream) : -1;")

# Stock source resolves these calls through the SDK and the Duke namespace.
if "/progs/duke3d/" in str(src) and src.suffix == ".c":
    source = re.sub(r'\brb->open\(', 'duke_open(', source)
    source = source.replace("rb->file_exists(", "duke_file_exists(")
    source = '#include "SDL.h"\n#include "duke-redirect.h"\n' + source
    if src.name == "nomusic.c":
        source = source.replace("unsigned long", "uint32_t")
        change("MUSIC_PlaySong(unsigned char *song", "MUSIC_PlaySong(char *song")
        source += '\nvoid PlayMusic(char *name) { (void)name; }\n'
if src.name == "wrappers.c":
    source = '#include "SDL.h"\n#include "duke-redirect.h"\n' + source

payload = source.encode("latin-1")
if not output.exists() or output.read_bytes() != payload:
    output.write_bytes(payload)
