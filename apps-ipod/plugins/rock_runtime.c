/* AstraPod foreground plugin runtime. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "usb.h"
#include "plugins/rock_files.h"
#include "plugins/rock_widgets.h"
#include "plugins/sdk/plugin.h"
#include "rock-compat/rock_fingerprint.h"
#include "core_alloc.h"
#include "root_menu.h"
#include "sound.h"
#include "timer.h"
#include "speech/language.h"
#include "lang.h"
#include "system/app_buffer.h"
#include "widgets/folder_select.h"
#include "widgets/splash.h"

#ifdef ROCK_COMPAT_GENERATED
#include "rock_compat_sync.h"
static struct rock_compat_settings settings_view, settings_baseline;
static unsigned char *plugin_language[ROCK_COMPAT_LANG_COUNT];
#define ROCK_SETTINGS_VIEW (&settings_view)
#define ROCK_LANGUAGE_VIEW plugin_language
#else
#define ROCK_SETTINGS_VIEW (&global_settings)
#define ROCK_LANGUAGE_VIEW language_strings
#endif

#if CONFIG_BINFMT == BINFMT_ROCK
#include "lc-rock-checked.h"
#else
bool rock_host_header_valid(const struct plugin_header *header);
#endif

#define ROCK_BUFFER_ALIGNMENT 8

static const unsigned char fingerprint[] = ROCK_FINGERPRINT;
static const char arena_owner[] = "Rockbox plugin";
static const char *filename;
static unsigned char *tail;
static size_t tail_size;
static bool usb_taken;
static bool backlight_overridden;

/* Serialize handoff with the audio and USB threads. The timer and mixer
 * synchronize independently with their interrupt/callback contexts. */
static struct mutex media_mutex;
static bool media_initialized;
static bool media_disabled;
static bool click_requested;
static bool timer_owned;
static unsigned int click_rate;
static bool volume_overridden;

static void rock_ticks_stop_locked(void)
{
    if (timer_owned)
    {
        timer_release_ticks();
        timer_owned = false;
    }
}

static void rock_media_stop_locked(void)
{
    media_disabled = true;
    click_requested = false;
    rock_ticks_stop_locked();
    if (click_rate)
        mixer_click_stop();
    click_rate = 0;
    if (volume_overridden)
    {
        sound_set_transient_volume(global_status.volume);
        volume_overridden = false;
    }
}

void rock_media_shutdown(void)
{
    if (!media_initialized)
        return;
    mutex_lock(&media_mutex);
    rock_media_stop_locked();
    mutex_unlock(&media_mutex);
}

void rock_playback_takeover(void)
{
    if (!media_initialized)
        return;
    mutex_lock(&media_mutex);
    if (click_requested)
        rock_media_stop_locked();
    mutex_unlock(&media_mutex);
}

static bool click_available(void)
{
#ifdef USB_ENABLE_AUDIO
    if (usb_audio_get_active())
        return false;
#endif
    return !media_disabled && !(audio_status() &
        (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE |
         AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD));
}

static unsigned int rock_click_begin(void)
{
    /* The audio thread can service a takeover while audio_stop waits. */
    mutex_lock(&media_mutex);
    bool allowed = !usb_taken && !media_disabled && !click_requested &&
        !(audio_status() & (AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD));
#ifdef USB_ENABLE_AUDIO
    if (usb_audio_get_active())
        allowed = false;
#endif
    if (allowed)
        click_requested = true;
    mutex_unlock(&media_mutex);
    if (!allowed)
        return 0;
    audio_stop();
    mutex_lock(&media_mutex);
    if (click_available())
        click_rate = mixer_get_frequency();
    click_requested = click_rate != 0;
    unsigned int rate = click_rate;
    mutex_unlock(&media_mutex);
    return rate;
}

static bool rock_click_active(void)
{
    mutex_lock(&media_mutex);
    bool active = click_rate && click_rate == mixer_get_frequency();
    mutex_unlock(&media_mutex);
    return active;
}

static void rock_click_play(const void *samples, size_t size)
{
    mutex_lock(&media_mutex);
    if (click_rate)
        mixer_click_play(samples, size, click_rate);
    mutex_unlock(&media_mutex);
}

static void rock_click_volume(int value)
{
    mutex_lock(&media_mutex);
    if (click_rate)
    {
        sound_set_transient_volume(value);
        volume_overridden = true;
    }
    mutex_unlock(&media_mutex);
}

static int rock_click_min_volume(void)
{
    return sound_min(SOUND_VOLUME);
}

static int rock_click_max_volume(void)
{
    return MIN(sound_max(SOUND_VOLUME), global_settings.volume_limit);
}

static bool rock_ticks_start(void)
{
    mutex_lock(&media_mutex);
    if (click_rate && click_rate == mixer_get_frequency() && !timer_owned)
        timer_owned = timer_reserve_ticks(TIMER_FREQ / ROCK_CLICK_TICK_HZ);
    bool owned = timer_owned;
    mutex_unlock(&media_mutex);
    return owned;
}

static unsigned int rock_ticks_take(void)
{
    mutex_lock(&media_mutex);
    unsigned int ticks = timer_owned ? timer_take_ticks() : 0;
    mutex_unlock(&media_mutex);
    return ticks;
}

static void rock_ticks_stop(void)
{
    mutex_lock(&media_mutex);
    rock_ticks_stop_locked();
    if (click_rate)
        mixer_click_stop();
    mutex_unlock(&media_mutex);
}

static struct mp3entry *rock_audio_current_track(void)
{
    static struct mp3entry snapshot;
    return !usb_taken && audio_current_track_snapshot(&snapshot) ? &snapshot : NULL;
}

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static bool plugin_boosted;

#ifdef CPU_BOOST_LOGGING
static void rock_cpu_boost(bool enabled, char *location, int line)
#else
static void rock_cpu_boost(bool enabled)
#endif
{
    if (plugin_boosted == enabled || (enabled && usb_taken))
        return;

    plugin_boosted = enabled;
#ifdef CPU_BOOST_LOGGING
    cpu_boost_(enabled, location, line);
#else
    cpu_boost(enabled);
#endif
}
#endif
#ifdef HAVE_SW_POWEROFF
static bool original_sw_poweroff;
#endif

bool rock_usb_taken(void)
{
    return filename && usb_taken;
}

const unsigned char *rock_widget_text(const unsigned char *text)
{
#ifdef ROCK_COMPAT_GENERATED
    if (filename)
        return rock_compat_widget_text(text, plugin_language);
#endif
    return text;
}

void rock_flush_settings(void)
{
#ifdef ROCK_COMPAT_GENERATED
    if (!usb_taken)
        rock_compat_flush(&settings_view, &settings_baseline, &global_settings);
#endif
}

void rock_begin_settings(void)
{
#ifdef ROCK_COMPAT_GENERATED
    rock_compat_begin(&settings_view, &settings_baseline, &global_settings);
    rock_compat_lang_init(plugin_language, language_strings);
#endif
}

static void restore_backlight(void)
{
    if (!backlight_overridden)
        return;

    backlight_overridden = false;
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
    backlight_set_on_button_hold(global_settings.backlight_on_button_hold);
    backlight_set_brightness(global_settings.brightness);
}

static void restore_input_settings(void)
{
    restore_backlight();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (plugin_boosted)
        cpu_boost(false);
    plugin_boosted = false;
#endif
#ifdef HAVE_SW_POWEROFF
    button_set_sw_poweroff_state(original_sw_poweroff);
#endif
}

void rock_usb_takeover(void)
{
    if (!filename || usb_taken)
        return;
    rock_media_shutdown();
    rock_flush_settings();
    restore_input_settings();
    usb_taken = true;
    rock_close_files();
}

static long rock_button_get(bool block)
{
    return usb_taken ? SYS_USB_CONNECTED : button_get(block);
}

static long rock_button_get_w_tmo(int timeout)
{
    return usb_taken ? SYS_USB_CONNECTED : button_get_w_tmo(timeout);
}

static int rock_get_action(int context, int timeout)
{
    return usb_taken ? SYS_USB_CONNECTED : get_action(context, timeout);
}

static int rock_get_custom_action(int context, int timeout,
                                  const struct button_mapping *(*map)(int))
{
    return usb_taken ? SYS_USB_CONNECTED :
                       get_custom_action(context, timeout, map);
}

#ifdef HAVE_SW_POWEROFF
static void rock_button_set_sw_poweroff_state(bool enabled)
{
    if (!usb_taken)
        button_set_sw_poweroff_state(enabled);
}
#endif

#define ROCK_BACKLIGHT_SETTER(name) \
    static void rock_##name(int value) \
    { \
        if (!usb_taken) \
        { \
            backlight_overridden = true; \
            name(value); \
        } \
    }
ROCK_BACKLIGHT_SETTER(backlight_set_timeout)
ROCK_BACKLIGHT_SETTER(backlight_set_timeout_plugged)
ROCK_BACKLIGHT_SETTER(backlight_set_on_button_hold)
ROCK_BACKLIGHT_SETTER(backlight_set_brightness)
#undef ROCK_BACKLIGHT_SETTER

static void *rock_get_buffer(size_t *size)
{
    *size = tail_size;
    return tail_size ? tail : NULL;
}

static size_t rock_reserve_buffer(size_t size)
{
    size = size > tail_size ? tail_size : ALIGN_UP(size, ROCK_BUFFER_ALIGNMENT);
    tail += size;
    tail_size -= size;
    return tail_size;
}

static const char *rock_get_current_filename(void)
{
    return filename;
}

static int rock_do_menu(const struct menu_item_ex *menu, int *selected,
                        struct viewport *parent, bool hide_theme)
{
    if (usb_taken)
        return MENU_ATTACHED_USB;
    struct rock_native_scope scope = rock_modal_begin();
    int result = do_menu(menu, selected, parent, hide_theme);
    if (result == MENU_ATTACHED_USB)
        rock_usb_takeover();
    rock_modal_end(&scope);
    return result;
}

static bool rock_set_int(const unsigned char *title, const char *unit, int voice_unit,
                         const int *value, void (*changed)(int), int step, int min,
                         int max, const char *(*format)(char *, size_t, int, const char *))
{
    if (usb_taken)
        return true;
    struct rock_native_scope scope = rock_modal_begin();
    bool result = set_int(title, unit, voice_unit, value, changed, step, min, max, format);
    rock_modal_end(&scope);
    return result;
}

static bool rock_set_option(const char *title, const void *value, enum optiontype type,
                            const struct opt_items *options, int count, void (*changed)(int))
{
    if (usb_taken)
        return true;
    struct rock_native_scope scope = rock_modal_begin();
    bool result = set_option(title, value, type, options, count, changed);
    rock_modal_end(&scope);
    return result;
}

static bool rock_set_bool_options(const char *title, const bool *value,
                                  const char *yes, int yes_voice,
                                  const char *no, int no_voice,
                                  void (*changed)(bool))
{
    if (usb_taken)
        return true;
    struct rock_native_scope scope = rock_modal_begin();
    bool result = set_bool_options(title, value, yes, yes_voice,
                                   no, no_voice, changed);
    rock_modal_end(&scope);
    return result;
}

static bool rock_set_bool(const char *title, const bool *value)
{
    return rock_set_bool_options(title, value,
                                 (const char *)STR(LANG_SET_BOOL_YES),
                                 (const char *)STR(LANG_SET_BOOL_NO), NULL);
}

static bool rock_simplelist_show(struct simplelist_info *info)
{
    if (usb_taken)
        return true;
    struct rock_native_scope scope = rock_modal_begin();
    /* Native simplelists also return true for the Menu key. */
    simplelist_show_list(info);
    rock_modal_end(&scope);
    return usb_taken;
}

static const struct settings_list *rock_find_setting(const void *variable)
{
#ifdef ROCK_COMPAT_GENERATED
    /* Only fields with matching native types, units and choices can expose
     * the native registry entry. Converted or fixed fields have no such entry. */
#define ROCK_FIND_MEMBER(member) \
    if (variable == &settings_view.member) \
        return find_setting(&global_settings.member)
    ROCK_FIND_MEMBER(playlist_shuffle);
    ROCK_FIND_MEMBER(repeat_mode);
    ROCK_FIND_MEMBER(brightness);
    ROCK_FIND_MEMBER(backlight_timeout);
    ROCK_FIND_MEMBER(backlight_timeout_plugged);
    ROCK_FIND_MEMBER(backlight_on_button_hold);
#undef ROCK_FIND_MEMBER
#endif
    return find_setting(variable);
}

static bool rock_option_screen(const struct settings_list *setting,
                               struct viewport *parent, bool temporary,
                               const unsigned char *title)
{
    if (usb_taken)
        return true;
    if (!setting)
        return false;
    struct rock_native_scope scope = rock_modal_begin();
    bool result = option_screen(setting, parent, temporary, title);
    rock_modal_end(&scope);
    return result;
}

static int rock_settings_save(void)
{
    if (usb_taken)
        return -1;
    rock_flush_settings();
    return settings_save();
}

static void rock_settings_apply(bool read_disk)
{
    if (usb_taken)
        return;
    struct rock_native_scope scope = rock_modal_begin();
    settings_apply(read_disk);
    backlight_overridden = false;
    rock_modal_end(&scope);
}

static long rock_event_ex(long event, void (*cleanup)(void *), void *parameter)
{
    if (usb_taken)
        return SYS_USB_CONNECTED;
#ifdef HAVE_USB_CONFIRM
    /* A retained list can read raw buttons. Its start key's release may
     * already be consumed when the USB dialog changes the action context. */
    if (event == SYS_USB_CONFIRM && !rock_ui_raw())
        action_handoff_enter();
#endif
    if (!IS_SYSEVENT(event) || (!rock_ui_raw() && event != SYS_USB_CONNECTED))
        return default_event_handler_ex(event, cleanup, parameter);

    struct rock_native_scope scope = rock_modal_begin();
    long result = default_event_handler_ex(event, cleanup, parameter);
    rock_modal_end(&scope);
    return usb_taken ? SYS_USB_CONNECTED : result;
}

static long rock_event(long event)
{
    return rock_event_ex(event, NULL, NULL);
}

static bool rock_action_userabort(int timeout)
{
    if (usb_taken)
        return true;
    int action = get_action(CONTEXT_STD, timeout);
    return action == ACTION_STD_CANCEL || rock_event(action) == SYS_USB_CONNECTED;
}

static void rock_progress_set_delay(long delay)
{
    if (usb_taken)
        return;
    rock_list_release(false);
    splash_progress_set_delay(delay);
}

static void rock_progress(int current, int total, const char *fmt, ...)
{
    if (usb_taken || !splash_progress_needs_draw(current, total))
        return;

    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), P2STR((const unsigned char *)fmt), args);
    va_end(args);

    rock_list_release(false);
    struct rock_native_scope scope = rock_display_native_begin();
    splash_progress(current, total, "%s", text);
    rock_display_native_end(&scope, ROCK_RESUME_SILENT);
}

static void rock_splashf(int ticks, const char *fmt, ...)
{
    if (usb_taken)
        return;

    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), P2STR((const unsigned char *)fmt), args);
    va_end(args);
    struct rock_native_scope scope = rock_modal_begin();
    splashf(ticks, "%s", text);
    rock_modal_end(&scope);
}

static bool rock_select_db_folders(bool autoresume)
{
    if (usb_taken)
        return false;

    struct rock_native_scope scope = rock_modal_begin();
    bool changed;
    if (autoresume)
        changed = folder_select_in_buffer(str(LANG_AUTORESUME),
                    (char *)global_settings.autoresume_paths,
                    sizeof(global_settings.autoresume_paths), tail, tail_size);
    else
        changed = folder_select_in_buffer(str(LANG_SELECT_FOLDER),
                    (char *)global_settings.tagcache_scan_paths,
                    sizeof(global_settings.tagcache_scan_paths), tail, tail_size);
    rock_modal_end(&scope);
    return changed;
}

static const struct plugin_api api = {
#define ROCK_DIRECT(name) .name = name,
#define ROCK_SLOT(decl, name, binding) .name = binding,
#include "sdk/rock_api.def"
#undef ROCK_SLOT
#undef ROCK_DIRECT
};

static bool compatible(const struct plugin_header *header)
{
    return header->lc_hdr.magic == PLUGIN_MAGIC &&
           header->lc_hdr.target_id == TARGET_ID &&
           header->lc_hdr.api_version == PLUGIN_API_VERSION &&
           header->platform == CONFIG_PLATFORM &&
           header->header_size == sizeof(*header) &&
           header->api_size == sizeof(api) &&
           !memcmp(header->fingerprint, fingerprint, sizeof(fingerprint));
}

#if CONFIG_BINFMT == BINFMT_ROCK
static bool native_header_valid(const void *opaque, const unsigned char *arena,
                                size_t capacity, size_t bytes)
{
    const struct plugin_header *header = opaque;
    uintptr_t base = (uintptr_t)arena;
    uintptr_t end = (uintptr_t)header->lc_hdr.end_addr;
    uintptr_t file_end = base + bytes;
    uintptr_t bss = (uintptr_t)header->bss_start;
    uintptr_t entry = (uintptr_t)header->entry_point;
    uintptr_t slot = (uintptr_t)header->api;
    uintptr_t payload = base + sizeof(*header);

    return compatible(header) && header->lc_hdr.load_addr == arena &&
           (uintptr_t)header->file_end == file_end &&
           file_end <= bss && bss <= end && end - base <= capacity &&
           !(bss % 4) && !(end % 4) &&
           entry >= payload && entry <= file_end - 4 && !(entry % 4) &&
           slot >= payload && slot <= file_end - sizeof(rb) &&
           !(slot % _Alignof(const struct plugin_api *));
}
#endif

static int run_rock(const char *path, const char *file,
                    unsigned char *arena, size_t capacity,
                    fb_data *saved_pixels, fb_data *widget_pixels)
{
    void *handle;
#if CONFIG_BINFMT == BINFMT_ROCK
    struct plugin_header disk_header;
    handle = lc_open_checked(path, arena, capacity, &disk_header, sizeof(disk_header),
                             native_header_valid);
#else
    handle = lc_open(path, arena, capacity);
#endif
    if (!handle)
        return PLUGIN_ERROR;

    struct plugin_header *module = lc_get_header(handle);
    int result = PLUGIN_ERROR;
#if CONFIG_BINFMT == BINFMT_DLOPEN
    if (!rock_host_header_valid(module) || !compatible(module) ||
        module->lc_hdr.load_addr || module->lc_hdr.end_addr ||
        module->file_end || module->bss_start)
        goto unload;
    tail = arena;
#else
    tail = arena + ALIGN_UP((uintptr_t)module->lc_hdr.end_addr - (uintptr_t)arena,
                            ROCK_BUFFER_ALIGNMENT);
#endif
    tail_size = arena + capacity - tail;
    *module->api = &api;

    filename = path;
    usb_taken = false;
    if (!media_initialized)
    {
        mutex_init(&media_mutex);
        media_initialized = true;
    }
    mutex_lock(&media_mutex);
    media_disabled = false;
    mutex_unlock(&media_mutex);
#ifdef HAVE_SW_POWEROFF
    original_sw_poweroff = button_get_sw_poweroff_state();
#endif
    rock_begin_settings();
    rock_display_start(saved_pixels, widget_pixels);
    button_clear_pressed();
    rock_action_reset();

    result = module->entry_point(file);
    rock_media_shutdown();
    if (usb_taken)
        result = PLUGIN_USB_CONNECTED;
    rock_flush_settings();
    if (!usb_taken)
        restore_input_settings();
    rock_close_files();
    rock_list_discard();
    button_clear_pressed();
    rock_display_stop();

    filename = NULL;
    usb_taken = false;
    tail = NULL;
    tail_size = 0;

#if CONFIG_BINFMT == BINFMT_DLOPEN
unload:
#endif
    lc_close(handle);
    return result;
}

int rock_open_from_browser(const char *path, const char *file)
{
    size_t capacity;
    unsigned char *arena = app_try_claim_buffer(&capacity, arena_owner);
    if (!arena)
    {
        splash(HZ, ID2P(LANG_PLUGIN_CANT_OPEN));
        return GO_TO_PREVIOUS;
    }

    capacity -= 2 * FRAMEBUFFER_SIZE;
    fb_data *caller_pixels = (fb_data *)(arena + capacity);
    fb_data *widget_pixels = (fb_data *)(arena + capacity + FRAMEBUFFER_SIZE);
    int result = run_rock(path, file, arena, capacity, caller_pixels,
                          widget_pixels);
    app_release_buffer(arena_owner);

    if (result == PLUGIN_USB_CONNECTED || result == PLUGIN_GOTO_ROOT)
        return GO_TO_ROOT;
    if (result == PLUGIN_GOTO_WPS)
        return GO_TO_WPS;
    if (result == PLUGIN_ERROR)
        splash(HZ, ID2P(LANG_PLUGIN_ERROR));
    return GO_TO_PREVIOUS;
}
