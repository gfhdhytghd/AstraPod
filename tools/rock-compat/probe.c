/* Freestanding ipod6g mapping test. qemu-arm supplies only the exit syscall. */
#include "rock_compat_input.h"
#include "rock_compat_sync.h"

static struct user_settings host;
static struct rock_compat_settings view, baseline;
static unsigned char *native_text[LANG_LAST_INDEX_IN_ARRAY];
static unsigned char *plugin_text[ROCK_COMPAT_LANG_COUNT];

static int test(void)
{
    host.playback_log = 2;
    host.album_art = AA_PREFER_CACHE;
    host.dithering_enabled = DITHER_AUTO;
    rock_compat_begin(&view, &baseline, &host);
    if (view.playback_log || view.dithering_enabled ||
        view.album_art != AA_PREFER_EMBEDDED)
        return 1;
    host.brightness = 9; /* native update while plugin is active */
    view.pitch_mode_semitone = true;
    view.playlist_shuffle = true;
    view.font_file[0] = 'A';
    rock_compat_flush(&view, &baseline, &host);
    if (host.brightness != 9 || host.album_art != AA_PREFER_CACHE ||
        host.playback_log != 2 || !host.playlist_shuffle || host.font_file[0] != 'A' ||
        view.pitch_mode_semitone || view.brightness != 9 ||
        host.dithering_enabled != DITHER_AUTO)
        return 2;
    view.playback_log = true;
    view.dithering_enabled = true;
    rock_compat_flush(&view, &baseline, &host);
    if (host.playback_log != 1 || host.dithering_enabled != DITHER_ALWAYS)
        return 3;
    native_text[LANG_CANCEL] = (unsigned char *)"translated";
    rock_compat_lang_init(plugin_text, native_text);
    if (plugin_text[LANG_CANCEL] != native_text[LANG_CANCEL] ||
        rock_compat_widget_text(ID2P(LANG_COUNTDOWN_TIMER_SET), plugin_text) !=
            plugin_text[LANG_COUNTDOWN_TIMER_SET] ||
        rock_compat_widget_text(ID2P(LANG_CANCEL), plugin_text) != ID2P(LANG_CANCEL) ||
        memcmp(plugin_text[LANG_COUNTDOWN_TIMER_SET], "SET TIMER", sizeof("SET TIMER")))
        return 4;
    if (rock_compat_stock_action(ROCK_STOCK_ACTION_WPS_PLAY) != ACTION_WPS_PLAY ||
        rock_compat_stock_context(ROCK_STOCK_CONTEXT_KEYBOARD) != CONTEXT_KEYBOARD ||
        rock_compat_stock_context(ROCK_STOCK_CONTEXT_PITCHSCREEN) != -1 ||
        rock_compat_stock_action(ROCK_STOCK_ACTION_PS_RESET) != -1)
        return 5;
    return 0;
}

void _start(void)
{
    register int result asm("r0") = test();
    register int syscall asm("r7") = 1;
    asm volatile("svc 0" : : "r"(result), "r"(syscall) : "memory");
    __builtin_unreachable();
}
