/* Foreground host loop for the stock Quake engine. GPL-2.0-or-later. */
#include "quakedef.h"
#include "quake_heap.h"

qboolean isDedicated;
int enable_printf;
cvar_t sys_nostdout = {.name = "sys_nostdout", .string = "0"};
char *basedir = QUAKE_BASE;
char *cachedir;

static jmp_buf finish;
static long quake_start_tick;
static enum plugin_status result;
static bool shutting_down;

void quake_quit(enum plugin_status status)
{
    result = status;
    longjmp(finish, 1);
    __builtin_unreachable();
}

int quake_vsnprintf(char *dst, size_t bytes, const char *fmt, va_list args)
{
    if (bytes == SIZE_MAX)
        Sys_Error("Unknown Quake string capacity");
    return rb->vsnprintf(dst, bytes, fmt, args);
}

int quake_snprintf(char *dst, size_t bytes, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = quake_vsnprintf(dst, bytes, fmt, args);
    va_end(args);
    return n;
}

int quake_log(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void Sys_Printf(char *format, ...)
{
    (void)format;
}

/* Shows a two-line notice on the bare LCD for three seconds. */
static void show_message(const char *line1, const char *line2)
{
    quake_audio_stop();
    if (rb->plugin_usb_taken())
        return;
    rb->lcd_set_viewport(NULL);
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_setfont(FONT_SYSFIXED);
    rb->lcd_clear_display();
    rb->lcd_putsxy(8, 8, "Quake");
    rb->lcd_putsxy(8, 24, line1);
    if (line2)
        rb->lcd_putsxy(8, 40, line2);
    rb->lcd_update();
    for (int i = 0; i < 3 * HZ; i++)
    {
        quake_check_events();
        rb->sleep(1);
    }
}

void Sys_Error(char *format, ...)
{
    char message[128];
    va_list args;
    va_start(args, format);
    quake_vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    show_message(message, NULL);
    quake_quit(PLUGIN_ERROR);
}

void Sys_Quit(void)
{
    quake_quit(PLUGIN_OK);
}

void Sys_DebugLog(char *file, char *format, ...)
{
    (void)file;
    (void)format;
}

double Sys_FloatTime(void)
{
    return ((unsigned long)*rb->current_tick - (unsigned long)quake_start_tick)
        / (double)HZ;
}

void Sys_Sleep(void)
{
    rb->sleep(1);
}

void Sys_LowFPPrecision(void) {}
void Sys_HighFPPrecision(void) {}

enum plugin_status rock_quake_run(void)
{
    static char *argv[] = {"quake", "-nomouse", NULL};
    static quakeparms_t parms = {
        .basedir = QUAKE_BASE,
        .memsize = ROCK_QUAKE_HEAP_BYTES
    };

    if (!setjmp(finish))
    {
        switch (quake_check_data())
        {
            case QUAKE_DATA_OK:
                break;
            case QUAKE_DATA_MISSING:
                show_message("Game data not found. Put pak0.pak in",
                             QUAKE_DATA);
                quake_quit(PLUGIN_OK);
            case QUAKE_DATA_INVALID:
                show_message("Invalid or truncated pak file in",
                             QUAKE_DATA);
                quake_quit(PLUGIN_OK);
        }
        parms.membase = rb->quake_heap_acquire();
        if (!parms.membase)
        {
            show_message("Quake needs 12 MiB of free memory.",
                         "Stop music playback and try again.");
            quake_quit(PLUGIN_OK);
        }

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        rb->cpu_boost(true);
#endif
        quake_start_tick = *rb->current_tick;
        COM_InitArgv(2, argv);
        parms.argc = com_argc;
        parms.argv = com_argv;
        Host_Init(&parms);
        double old = Sys_FloatTime();
        for (;;)
        {
            quake_check_events();
            quake_audio_pump();
            double now = Sys_FloatTime();
            Host_Frame(now - old);
            old = now;
            quake_audio_pump();
            rb->sleep(1);
        }
    }

    quake_audio_stop();
    if (result == PLUGIN_OK && host_initialized && !shutting_down)
    {
        shutting_down = true;
        Host_Shutdown();
    }
    quake_sdl_quit();
    quake_close_files();
    return result;
}
