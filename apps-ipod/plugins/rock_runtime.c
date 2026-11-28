/* AstraPod foreground plugin runtime. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "plugins/rock_video_log.h"
#include "plugins/app_tray.h"
#include "plugins/rock_iram.h"
#include "plugins/rock_iram_layout.h"
#include "plugins/rock_large_stack.h"
#include "plugins/rock_pitch.h"
#include "podlink-plugin-test.h"
#include "usb.h"
#include "plugins/rock_files.h"
#include "storage.h"
#include "plugins/rock_widgets.h"
#include "plugins/rock_native_handoff.h"
#include "plugins/rock_catalog.h"
#include "plugins/lua/paths.h"
#include "plugins/sdk/plugin.h"
#include "rock-compat/rock_fingerprint.h"
#include "core_alloc.h"
#include "draw/resize.h"
#include "plugins/quake/quake_heap.h"
#include "files/filetypes.h"
#include "metadata/chapters.h"
#include "metadata/cuesheet.h"
#include "metadata/mp4_chapters.h"
#include "root_menu.h"
#include "pcm_waveform.h"
#include "sound.h"
#include "timer.h"
#include "speech/language.h"
#include "lang.h"
#include "system/activity.h"
#include "system/app_buffer.h"
#include "widgets/folder_select.h"
#include "widgets/splash.h"
#include "system/core_map.h"
#include "ui/native_theme_runtime.h"

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
#define ROCK_VIDEO_MODULE PLUGIN_APPS_DIR "/h264_player.rock"
#define ROCK_HEAP_LIMIT (2 * 1024 * 1024)
#define ROCK_VIDEO_HEAP_LIMIT (10u * 1024 * 1024)

static const unsigned char fingerprint[] = ROCK_FINGERPRINT;
static const char arena_owner[] = "Rockbox plugin";
static const char *filename;
static unsigned char *tail;
static size_t tail_size;
static int engine_heap;
static unsigned int foreground_owner;
static bool usb_taken;
#ifdef HAVE_HW_H264
static const struct rock_video_request *current_video;
#endif
static unsigned int mpeg_owner;
static void (*mpeg_drain)(void);

static bool rock_mpeg_set_drain(void (*drain)(void))
{
    if (!mpeg_owner || thread_self() != mpeg_owner || usb_taken)
        return false;
    mpeg_drain = drain;
    return true;
}

static unsigned int rock_mpeg_start_worker(void (*run)(void *), void *arg,
                                           void *stack, size_t bytes)
{
    return mpeg_drain ? rock_mpeg_worker_start(run, arg, stack, bytes) : 0;
}

/* Synchronous stream queues must drain before the generic worker barrier. */
static void rock_mpeg_end(void)
{
    if (mpeg_drain)
    {
        void (*drain)(void) = mpeg_drain;
        mpeg_drain = NULL;
        drain();
    }
    mpeg_owner = 0;
}

static void rock_mpeg_storage_idle(void)
{
    storage_sleep();
}

static void rock_mpeg_storage_keep_active(void)
{
#ifdef HAVE_DISK_STORAGE
    storage_spin();
#endif
}

static const struct rock_mpeg_api rock_mpeg_api = {
    .set_drain = rock_mpeg_set_drain,
    .start_worker = rock_mpeg_start_worker,
    .queue_init = queue_init,
    .queue_enable_queue_send = queue_enable_queue_send,
    .queue_empty = queue_empty,
    .queue_post = queue_post,
    .queue_send = queue_send,
    .queue_reply = queue_reply,
    .queue_wait = queue_wait,
    .queue_wait_w_tmo = queue_wait_w_tmo,
    .mutex_init = mutex_init,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .storage_idle = rock_mpeg_storage_idle,
    .storage_keep_active = rock_mpeg_storage_keep_active,
};
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
    rock_h264_audio_revoke();
    rock_stream_revoke();
    rock_pitch_revoke();
    rock_pitch_preview_revoke();
    if (!media_initialized)
        return;
    mutex_lock(&media_mutex);
    rock_media_stop_locked();
    mutex_unlock(&media_mutex);
}

void rock_playback_takeover(void)
{
    rock_h264_audio_revoke();
    rock_stream_revoke();
    rock_pitch_revoke();
    rock_pitch_preview_revoke();
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

static size_t rock_waveform_read(int16_t *samples, unsigned int window_ms)
{
    mutex_lock(&media_mutex);
    size_t frames = 0;
    if (!media_disabled && audio_status() == AUDIO_STATUS_PLAY)
        frames = mixer_waveform_read(samples, window_ms);
    mutex_unlock(&media_mutex);
    return frames;
}

static struct mp3entry *rock_audio_current_track(void)
{
    static struct mp3entry snapshot;
    return !usb_taken && audio_current_track_snapshot(&snapshot) ? &snapshot : NULL;
}

static bool lua_track_watched;
static volatile unsigned int lua_track_sequence;

static void lua_track_changed(unsigned short event, void *data)
{
    (void)event;
    (void)data;
    lua_track_sequence++;
}

static unsigned int rock_lua_track_event(bool enabled)
{
    enabled = enabled && !usb_taken;
    if (enabled && !lua_track_watched)
        lua_track_watched = add_event(PLAYBACK_EVENT_TRACK_CHANGE,
                                      lua_track_changed);
    else if (!enabled && lua_track_watched)
    {
        remove_event(PLAYBACK_EVENT_TRACK_CHANGE, lua_track_changed);
        lua_track_watched = false;
    }
    return lua_track_sequence;
}

static int rock_lua_volume(void)
{
    return global_status.volume;
}

/* Do not overwrite a physical volume change during a fade. */
static bool rock_lua_fade_volume(int expected, int value)
{
    if (usb_taken || global_status.volume != expected ||
        value > expected || value < sound_min(SOUND_VOLUME))
        return false;
    global_status.volume = value;
    setvol();
    return true;
}

static bool rock_lua_sleep_timer(int minutes)
{
    if (usb_taken || minutes < 1 || minutes > 240)
        return false;
    set_sleeptimer_duration(minutes);
    return true;
}

static bool rock_lua_play_playlist(void)
{
    if (usb_taken ||
        !rock_file_exists(ROCK_LUA_PLAYLIST_DIR ROCK_LUA_PLAYLIST))
        return false;

    audio_stop();
    if (playlist_create(ROCK_LUA_PLAYLIST_DIR, ROCK_LUA_PLAYLIST) < 0 ||
        !playlist_amount())
        return false;
    playlist_start(0, 0, 0);
    return true;
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
    {
        bool was_shuffled = global_settings.playlist_shuffle;
        rock_compat_flush(&settings_view, &settings_baseline, &global_settings);
        if (global_settings.playlist_shuffle != was_shuffled)
            global_settings.shuffle_mode = global_settings.playlist_shuffle ?
                                           SHUFFLE_SONGS : SHUFFLE_OFF;
    }
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
    rock_tsr_stop(ROCK_TSR_USB);
    if (!filename || usb_taken)
        return;
#ifdef HAVE_PODLINK_PLUGIN_TEST
    podlink_plugin_test_usb_taken();
#endif
    rock_mpeg_end();
    rock_lua_track_event(false);
    rock_h264_end();
    rock_media_shutdown();
    rock_execution_end();
    rock_hid_close();
    rock_podlink_close();
    rock_flush_settings();
    restore_input_settings();
    usb_taken = true;
    rock_release_plugin_font();
    rock_close_files();
}

static long rock_iram_button_event(long event)
{
    if (event == SYS_USB_CONNECTED)
        rock_iram_defer_usb(button_get_data());
    return event;
}

static long rock_button_get(bool block)
{
    return usb_taken ? SYS_USB_CONNECTED :
                       rock_iram_button_event(button_get(block));
}

static long rock_button_get_w_tmo(int timeout)
{
    return usb_taken ? SYS_USB_CONNECTED :
                       rock_iram_button_event(button_get_w_tmo(timeout));
}

static int rock_get_action(int context, int timeout)
{
    if (usb_taken)
        return SYS_USB_CONNECTED;
    return rock_iram_button_event(get_action(context, timeout));
}

static int rock_get_custom_action(int context, int timeout,
                                  const struct button_mapping *(*map)(int))
{
    return usb_taken ? SYS_USB_CONNECTED :
                       rock_iram_button_event(get_custom_action(context, timeout, map));
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

#ifdef HAVE_HW_H264
static void *rock_claim_video_scratch(size_t bytes)
{
    /* The module's VPU, AAC and PCM buffers share one pinned allocation. */
    if (foreground_owner != thread_self() || engine_heap ||
        !bytes || bytes > ROCK_VIDEO_HEAP_LIMIT)
        return NULL;

    int handle = core_alloc(bytes);
    if (handle <= 0)
        return NULL;
    void *scratch = core_get_data_pinned(handle);
    engine_heap = handle;
    if (usb_taken)
        return NULL;
    rock_h264_begin(scratch, bytes);
    rock_h264_audio_begin(scratch, bytes);
    return scratch;
}
#endif

static void *rock_claim_scratch(size_t minimum, size_t preferred,
                                size_t *granted)
{
    if (!granted)
        return NULL;
    *granted = 0;
    if (usb_taken)
        return NULL;
#ifdef HAVE_HW_H264
    if (filename && !strcmp(filename, ROCK_VIDEO_MODULE))
    {
        size_t free_bytes = core_available();
        size_t largest = core_allocatable();
        void *scratch = rock_claim_video_scratch(minimum);
        if (scratch)
            *granted = minimum;
        if (!scratch || (current_video && current_video->probe))
        {
            int fd = rock_open(ROCK_VIDEO_LOG, O_WRONLY | O_CREAT | O_APPEND);
            if (fd >= 0)
            {
                const char *path = current_video && current_video->path ?
                    current_video->path : "(file picker)";
                rock_fdprintf(fd, "\n%s stage=core-alloc code=%d "
                              "free=%lu largest=%lu limit=%lu "
                              "requested=%lu granted=%lu\n",
                              path, scratch ? 0 : -1,
                              (unsigned long)free_bytes, (unsigned long)largest,
                              (unsigned long)ROCK_VIDEO_HEAP_LIMIT,
                              (unsigned long)minimum,
                              (unsigned long)*granted);
                rock_close(fd);
            }
        }
        return scratch;
    }
#endif
    size_t bytes = MIN(preferred, tail_size) & ~(ROCK_BUFFER_ALIGNMENT - 1);
    if (!bytes || bytes < minimum)
        return NULL;
    void *result = tail;
    tail += bytes;
    tail_size -= bytes;
    *granted = bytes;
    return result;
}

/* Keep the heap pinned through CRT cleanup, including USB unwind. */
static void *rock_claim_heap(size_t bytes)
{
    if (!filename || foreground_owner != thread_self() ||
        usb_taken || engine_heap || !bytes ||
        bytes > ROCK_HEAP_LIMIT)
        return NULL;

    int handle = core_alloc(bytes);
    if (handle <= 0)
        return NULL;
    core_pin(handle);
    engine_heap = handle;
    return core_get_data(handle);
}

static size_t rock_reserve_buffer(size_t size)
{
    size = size >= tail_size ? tail_size :
           MIN(tail_size, ALIGN_UP(size, ROCK_BUFFER_ALIGNMENT));
    tail += size;
    tail_size -= size;
    return tail_size;
}

static int quake_heap_handle;

static void *rock_quake_heap_acquire(void)
{
    if (!filename || usb_taken || quake_heap_handle ||
        strcmp(filename, PLUGIN_GAMES_DIR "/quake.rock") ||
        (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE |
                           AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD)))
        return NULL;

    int handle = core_alloc(ROCK_QUAKE_HEAP_BYTES);
    if (handle <= 0)
    {
        core_map_log("quake heap 12 MiB");
        return NULL;
    }

    core_pin(handle);
    quake_heap_handle = handle;
    return core_get_data(handle);
}

static void rock_quake_heap_release(void)
{
    if (quake_heap_handle > 0)
    {
        core_unpin(quake_heap_handle);
        quake_heap_handle = core_free(quake_heap_handle);
    }
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
    bool first_page = rock_display_first_page();
    int result = do_menu(menu, selected, parent, hide_theme);
    if (result == MENU_ATTACHED_USB)
        rock_usb_takeover();
    rock_modal_end_with_resume(&scope, first_page ? ROCK_RESUME_DISCARD
                                                 : ROCK_RESUME_PRESENT);
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
    if (variable == &settings_view.playlist_shuffle)
        return find_setting(&global_settings.shuffle_mode);
    ROCK_FIND_MEMBER(repeat_mode);
    ROCK_FIND_MEMBER(brightness);
    ROCK_FIND_MEMBER(backlight_timeout);
    ROCK_FIND_MEMBER(backlight_timeout_plugged);
    ROCK_FIND_MEMBER(backlight_on_button_hold);
#undef ROCK_FIND_MEMBER
#endif
    if (variable == &global_settings.playlist_shuffle)
        return find_setting(&global_settings.shuffle_mode);
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

static bool rock_image_decoder_open(const char *name,
                                    struct rock_image_decoder_request *request);
static void rock_image_decoder_close(void);

static bool rock_app_tray_available(void)
{
    return foreground_owner && thread_self() == foreground_owner && !usb_taken;
}

static bool rock_app_tray_writable(void)
{
    if (!rock_app_tray_available() || usb_host_wants_storage())
        return false;
#ifdef HAVE_USBSTACK
    if (usb_exclusive_storage())
        return false;
#endif
    return true;
}

static bool rock_app_tray_register(const struct rock_tray_bitmap *frames,
                                   unsigned count)
{
    return rock_app_tray_writable() &&
           app_tray_register8(filename, frames, count);
}

static bool rock_app_tray_register12(const struct rock_tray_icon12 *frames,
                                     unsigned count)
{
    return rock_app_tray_writable() &&
           app_tray_register(filename, frames, count);
}

static bool rock_app_tray_set(unsigned frame)
{
    return rock_app_tray_writable() && app_tray_set(filename, frame);
}

static int rock_app_tray_get(void)
{
    return rock_app_tray_available() ? app_tray_get(filename, get_time()) : -1;
}

static bool rock_app_background_schedule(const struct rock_tray_rule *rules,
                                         unsigned count)
{
    return rock_app_tray_writable() &&
           app_background_schedule(filename, rules, count, get_time());
}

static const struct plugin_api api = {
#define ROCK_DIRECT(name) .name = name,
#define ROCK_SLOT(decl, name, binding) .name = binding,
#include "sdk/rock_api.def"
#undef ROCK_SLOT
#undef ROCK_DIRECT
};

static bool compatible(const struct plugin_header *header, uint32_t magic)
{
    uint32_t flags = magic == ROCK_TSR_MAGIC ? 0 : ROCK_IMAGE_LARGE_STACK;
    bool flags_valid = (header->flags & ~flags) == 0;
#ifdef HAVE_ROCK_IRAM_LEASE
    flags_valid |= magic == PLUGIN_MAGIC && header->flags == ROCK_IMAGE_IRAM;
#endif
    return flags_valid && header->lc_hdr.magic == magic &&
           header->lc_hdr.target_id == TARGET_ID &&
           header->lc_hdr.api_version == PLUGIN_API_VERSION &&
           header->platform == CONFIG_PLATFORM &&
           header->header_size == sizeof(*header) &&
           header->api_size == sizeof(api) &&
           !memcmp(header->fingerprint, fingerprint, sizeof(fingerprint));
}

#if CONFIG_BINFMT == BINFMT_ROCK
static bool native_image_valid(const void *opaque, const unsigned char *arena,
                               size_t capacity, size_t bytes, uint32_t magic)
{
    const struct plugin_header *header = opaque;
    uintptr_t base = (uintptr_t)arena;
    uintptr_t end = (uintptr_t)header->lc_hdr.end_addr;
    uintptr_t file_end = base + bytes;
    uintptr_t bss = (uintptr_t)header->bss_start;
    uintptr_t entry = (uintptr_t)header->entry_point;
    uintptr_t slot = (uintptr_t)header->api;
    uintptr_t payload = base + sizeof(*header);
    uintptr_t source = (uintptr_t)header->rock_iram_source;
    uintptr_t data_end = (uintptr_t)header->rock_iram_data_end;
    uintptr_t iram_end = (uintptr_t)header->rock_iram_end;
    uintptr_t dram_end = file_end;

    if (header->flags == ROCK_IMAGE_IRAM)
    {
        if (!rock_iram_extent_valid(payload, file_end, source, data_end, iram_end))
            return false;
        dram_end = source;
    }
    else if (source || data_end || iram_end)
        return false;

    return compatible(header, magic) && header->lc_hdr.load_addr == arena &&
           (uintptr_t)header->file_end == file_end &&
           file_end <= bss && bss <= end &&
           end - base <= (capacity & ~(size_t)(ROCK_BUFFER_ALIGNMENT - 1)) &&
           !(bss % 4) && !(end % 4) &&
           entry >= payload && entry <= dram_end - 4 && !(entry % 4) &&
           slot >= payload && slot <= dram_end - sizeof(rb) &&
           !(slot % _Alignof(const struct plugin_api *));
}

static bool native_header_valid(const void *header, const unsigned char *arena,
                                size_t capacity, size_t bytes)
{
    return native_image_valid(header, arena, capacity, bytes, PLUGIN_MAGIC);
}

static bool overlay_header_valid(const void *header, const unsigned char *arena,
                                 size_t capacity, size_t bytes)
{
    return native_image_valid(header, arena, capacity, bytes,
                              ROCK_OVERLAY_MAGIC);
}

static bool tsr_header_valid(const void *header, const unsigned char *arena,
                             size_t capacity, size_t bytes)
{
    return native_image_valid(header, arena, capacity, bytes, ROCK_TSR_MAGIC);
}
#endif

#if CONFIG_BINFMT == BINFMT_ROCK
static bool image_decoder_header_valid(const void *header,
                                       const unsigned char *arena,
                                       size_t capacity, size_t bytes)
{
    return native_image_valid(header, arena, capacity, bytes,
                              ROCK_IMAGE_DECODER_MAGIC);
}
#endif

static unsigned char *decoder_base;
static size_t decoder_capacity;
static void *decoder_handle;

static void rock_image_decoder_close(void)
{
    if (decoder_handle && thread_self() == foreground_owner)
    {
        lc_close(decoder_handle);
        decoder_handle = NULL;
    }
}

static bool rock_image_decoder_open(const char *name,
                                    struct rock_image_decoder_request *request)
{
    static const char *const names[] = { "bmp", "jpeg", "png", "ppm", "gif", "jpegp" };
    if (!name || !request || !request->parent_api || !decoder_capacity ||
        foreground_owner != thread_self() || usb_taken || decoder_handle)
        return false;

    unsigned int i;
    for (i = 0; i < ARRAYLEN(names); ++i)
        if (!strcmp(name, names[i]))
            break;
    if (i == ARRAYLEN(names))
        return false;

    char path[sizeof(ROCKBOX_DIR "/image-decoders/image_jpegp.ovl")];
    snprintf(path, sizeof(path), ROCKBOX_DIR "/image-decoders/image_%s.ovl",
             names[i]);
#if CONFIG_BINFMT == BINFMT_ROCK
    struct plugin_header disk_header;
    void *handle = lc_open_checked(path, decoder_base, decoder_capacity,
                                  &disk_header, sizeof(disk_header),
                                  image_decoder_header_valid);
#else
    void *handle = lc_open(path, decoder_base, decoder_capacity);
#endif
    if (!handle)
        return false;

    struct plugin_header *module = lc_get_header(handle);
    bool valid = true;
#if CONFIG_BINFMT == BINFMT_DLOPEN
    valid = rock_host_header_valid(module) &&
            compatible(module, ROCK_IMAGE_DECODER_MAGIC) &&
            !module->lc_hdr.load_addr && !module->lc_hdr.end_addr &&
            !module->file_end && !module->bss_start &&
            !module->rock_iram_source && !module->rock_iram_data_end &&
            !module->rock_iram_end;
#endif
    if (valid && !module->flags)
    {
        *module->api = &api;
        request->decoder = NULL;
        if (module->entry_point(request) == PLUGIN_OK && request->decoder)
        {
            decoder_handle = handle;
            return true;
        }
    }
    lc_close(handle);
    return false;
}

struct rock_invocation {
    struct plugin_header *module;
    const char *path;
    const char *file;
    struct rock_video_request *video;
    fb_data *saved_pixels;
    fb_data *widget_pixels;
    bool shell;
};

static int invoke_module(void *opaque)
{
    const struct rock_invocation *call = opaque;
#ifdef HAVE_USBSTACK
    if (usb_exclusive_storage())
        return PLUGIN_USB_CONNECTED;
#endif
    filename = call->path;
    foreground_owner = thread_self();
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
    rock_pitch_session_open();
    rock_stream_session_open();
    rock_execution_begin();
    mpeg_owner = thread_self();
    rock_h264_begin(tail, tail_size);
    rock_h264_audio_begin(tail, tail_size);
    rock_begin_settings();
    bool video = !strcmp(call->path, ROCK_VIDEO_MODULE);
    rock_display_start(call->saved_pixels, call->widget_pixels, video);
    button_clear_pressed();
    rock_action_reset();

#ifdef HAVE_PODLINK_PLUGIN_TEST
    if (call->shell)
        podlink_plugin_test_started();
#else
    (void)call->shell;
#endif
    push_activity_without_refresh(ACTIVITY_PLUGIN);
    struct rock_video_request fallback = {
        .path = call->file,
        .battery_level = battery_level,
        .result = ROCK_VIDEO_FAILED,
    };
    const void *parameter = call->file;
    if (video)
        parameter = call->video ? call->video : &fallback;
#ifdef HAVE_HW_H264
    if (video)
        current_video = parameter;
#endif
    int result = call->module->entry_point(parameter);
#ifdef HAVE_HW_H264
    current_video = NULL;
#endif
    if (get_current_activity() == ACTIVITY_PLUGIN)
        pop_current_activity_without_refresh();
#ifdef HAVE_PODLINK_PLUGIN_TEST
    if (call->shell)
        podlink_plugin_test_stopped();
#endif
    rock_mpeg_end();
    rock_lua_track_event(false);
    rock_h264_end();
    rock_media_shutdown();
    rock_execution_end();
    rock_hid_close();
    rock_podlink_close();
    if (usb_taken)
        result = PLUGIN_USB_CONNECTED;
    rock_flush_settings();
    if (!usb_taken)
        restore_input_settings();
    rock_release_plugin_font();
    rock_close_files();
    rock_quake_heap_release();
    rock_list_discard();
    button_clear_pressed();
    rock_display_stop(result == PLUGIN_OK);
    if (engine_heap)
    {
        core_unpin(engine_heap);
        core_free(engine_heap);
        engine_heap = 0;
    }

    rock_image_decoder_close();
    foreground_owner = 0;
    filename = NULL;
    usb_taken = false;

    return result;
}

static int run_rock(const char *path, const char *file,
                    unsigned char *arena, size_t capacity,
                    fb_data *saved_pixels, fb_data *widget_pixels, bool shell,
                    bool overlay, struct rock_video_request *video)
{
#ifdef HAVE_USBSTACK
    if (usb_exclusive_storage())
        return PLUGIN_USB_CONNECTED;
#endif
    size_t length = strlen(path);
    size_t suffix = sizeof(ROCK_TSR_SUFFIX) - 1;
    bool resident = !overlay && length >= suffix &&
        !strcasecmp(path + length - suffix, ROCK_TSR_SUFFIX);
    if (resident)
        rock_tsr_stop(ROCK_TSR_REPLACED);

    void *handle;
#if CONFIG_BINFMT == BINFMT_ROCK
    struct plugin_header disk_header;
    handle = lc_open_checked(path, resident ? rock_tsr_arena : arena,
        resident ? sizeof(rock_tsr_arena) : capacity,
        &disk_header, sizeof(disk_header),
        resident ? tsr_header_valid :
        overlay ? overlay_header_valid : native_header_valid);
#else
    handle = lc_open(path, arena, capacity);
#endif
    if (!handle)
        return PLUGIN_ERROR;

    struct plugin_header *module = lc_get_header(handle);
    int result = PLUGIN_ERROR;
#ifdef HAVE_ROCK_IRAM_LEASE
    bool iram_held = false;
#endif
#if CONFIG_BINFMT == BINFMT_DLOPEN
    uint32_t magic = resident ? ROCK_TSR_MAGIC :
                     overlay ? ROCK_OVERLAY_MAGIC : PLUGIN_MAGIC;
    if (!rock_host_header_valid(module) || !compatible(module, magic) ||
        module->lc_hdr.load_addr || module->lc_hdr.end_addr ||
        module->file_end || module->bss_start ||
        module->rock_iram_source || module->rock_iram_data_end ||
        module->rock_iram_end)
        goto unload;
#endif
#ifdef HAVE_ROCK_IRAM_LEASE
    if (module->flags == ROCK_IMAGE_IRAM)
    {
        if (!rock_iram_acquire())
            goto unload;
        iram_held = true;
#if CONFIG_BINFMT == BINFMT_ROCK
        uintptr_t data_end = (uintptr_t)module->rock_iram_data_end;
        memcpy((void *)ROCK_IRAM_ORIGIN, module->rock_iram_source,
               data_end - ROCK_IRAM_ORIGIN);
        memset((void *)data_end, 0, (uintptr_t)module->rock_iram_end - data_end);
        lc_commit_image();
#endif
    }
#endif
    tail = arena;
#if CONFIG_BINFMT == BINFMT_ROCK
    if (!resident)
        tail += ALIGN_UP((uintptr_t)module->lc_hdr.end_addr - (uintptr_t)arena,
                         ROCK_BUFFER_ALIGNMENT);
#endif
    tail_size = arena + capacity - tail;
    const char *leaf = strrchr(path, '/');
    if (leaf && !strcmp(leaf, "/imageviewer_compat.rock") &&
        capacity > ROCK_IMAGE_DECODER_OFFSET &&
        tail < arena + ROCK_IMAGE_DECODER_OFFSET)
    {
        decoder_base = arena + ROCK_IMAGE_DECODER_OFFSET;
        decoder_capacity = capacity - ROCK_IMAGE_DECODER_OFFSET;
        tail_size = decoder_base - tail;
    }
    rock_display_init();
    *module->api = &api;

    struct rock_invocation call = {
        .module = module, .path = path, .file = file, .video = video,
        .saved_pixels = saved_pixels, .widget_pixels = widget_pixels,
        .shell = shell,
    };
    if (resident)
        rock_tsr_begin(handle, module);

    if (module->flags & ROCK_IMAGE_LARGE_STACK)
    {
        result = rock_large_stack_launch(invoke_module, &call);
        if (result == PLUGIN_CHAIN_OVERLAY)
            result = PLUGIN_ERROR;
    }
    else
        result = invoke_module(&call);
    tail = NULL;
    tail_size = 0;
    decoder_base = NULL;
    decoder_capacity = 0;
    if (resident)
        return rock_tsr_finish(result == PLUGIN_OK) ? result : PLUGIN_ERROR;

#if CONFIG_BINFMT == BINFMT_DLOPEN || defined(HAVE_ROCK_IRAM_LEASE)
unload:
#endif
    lc_close(handle);
#ifdef HAVE_ROCK_IRAM_LEASE
    if (iram_held && rock_iram_release())
        result = PLUGIN_USB_CONNECTED;
#endif
    return result;
}

static int rock_open_foreground(const char *path, const char *file, bool shell,
                                bool quiet, struct rock_video_request *video)
{
#ifdef HAVE_PODLINK_PLUGIN_TEST
    if (!podlink_plugin_test_claim(shell))
        return PLUGIN_ERROR;
#endif
    size_t capacity;
    unsigned char *arena = app_try_claim_buffer(&capacity, arena_owner);
    int result = PLUGIN_ERROR;
    if (arena)
    {
        capacity -= 2 * FRAMEBUFFER_SIZE;
        fb_data *caller_pixels = (fb_data *)(arena + capacity);
        fb_data *widget_pixels = (fb_data *)(arena + capacity + FRAMEBUFFER_SIZE);
        rock_overlay_begin();
        rock_clear_handoff();
        result = run_rock(path, file, arena, capacity, caller_pixels,
                          widget_pixels, shell, false, video);
        const struct rock_handoff *next = NULL;
        if (result == PLUGIN_CHAIN_OVERLAY)
        {
            struct rock_overlay_launch launch;
            if (rock_overlay_take(&launch))
                result = run_rock(launch.path,
                                  launch.has_parameter ? launch.parameter : NULL,
                                  arena, capacity, caller_pixels, widget_pixels,
                                  shell, true, NULL);
            if (result == PLUGIN_CHAIN_OVERLAY)
                result = PLUGIN_ERROR;
        }
        else if (result == PLUGIN_GOTO_PLUGIN)
        {
            if (!shell)
                next = rock_take_handoff();
            if (!next)
                result = PLUGIN_ERROR;
            else if (next->kind == ROCK_HANDOFF_PLUGIN)
            {
                result = run_rock(next->module, next->path[0] ? next->path : NULL,
                                  arena, capacity,
                                  caller_pixels, widget_pixels,
                                  false, false, NULL);
                if (result == PLUGIN_GOTO_PLUGIN ||
                    result == PLUGIN_CHAIN_OVERLAY)
                    result = PLUGIN_ERROR;
                next = NULL;
            }
        }
        rock_overlay_end();
        app_release_buffer(arena_owner);
#ifdef HAVE_ROCK_IRAM_LEASE
        rock_iram_finish_usb();
#endif
        if (next)
        {
            struct rock_handoff handoff = *next;
            rock_clear_handoff();
            int native = rock_dispatch_native_handoff(&handoff);
            result = native == GO_TO_ROOT ? PLUGIN_GOTO_ROOT :
                     native == GO_TO_WPS ? PLUGIN_GOTO_WPS : PLUGIN_OK;
        }
        rock_clear_handoff();
        if (result == PLUGIN_ERROR && !quiet)
            splash(HZ, ID2P(LANG_PLUGIN_ERROR));
    }
    else if (!quiet)
        splash(HZ, ID2P(LANG_PLUGIN_CANT_OPEN));

#ifdef HAVE_PODLINK_PLUGIN_TEST
    podlink_plugin_test_release();
#endif
    return result;
}

int rock_run_from_shell(const char *path)
{
    bool previous = action_set_menu_back(true);
    int result = rock_open_foreground(path, NULL, true, true, NULL);
    action_set_menu_back(previous);
    return result;
}

#ifdef HAVE_HW_H264
int rock_probe_video(const char *path)
{
    struct rock_video_request request = {.path = path, .probe = true};
    return rock_open_foreground(ROCK_VIDEO_MODULE, path, false, true, &request);
}
#endif

static size_t rock_video_chapters(const char *path, uint32_t *offsets)
{
    int handle = core_alloc(sizeof(struct cuesheet));
    if (handle <= 0)
        return 0;
    struct cuesheet *cue = core_get_data_pinned(handle);
    int count = read_mp4_chapters(path, cue);
    for (int i = 0; i < count; i++)
        offsets[i] = cue->tracks[i].offset;
    core_unpin(handle);
    core_free(handle);
    return count >= MIN_CHAPTERS ? count : 0;
}

int rock_open_video(const char *path, const char *title, bool has_previous)
{
    uint32_t chapters[MAX_TRACKS];
    if (audio_status())
        audio_stop();
    struct rock_video_request request = {
        .path = path,
        .title = title,
        .battery_level = battery_level,
        .chapters = chapters,
        .chapter_count = rock_video_chapters(path, chapters),
        .has_previous = has_previous,
        .result = ROCK_VIDEO_FAILED,
    };
    int result = rock_open_foreground(ROCK_VIDEO_MODULE, path, false, false,
                                      &request);
    if (result == PLUGIN_USB_CONNECTED || result == PLUGIN_GOTO_ROOT)
        return ROCK_VIDEO_USB;
    return request.result;
}

static int rock_map_browser_result(int result)
{
    if (result == PLUGIN_USB_CONNECTED || result == PLUGIN_GOTO_ROOT)
        return GO_TO_ROOT;
    if (result == PLUGIN_GOTO_WPS)
        return GO_TO_WPS;
    if (result == PLUGIN_GOTO_FILEBROWSER)
        return GO_TO_FILEBROWSER;
    return GO_TO_PREVIOUS;
}

int rock_open_from_browser(const char *path, const char *file)
{
    return rock_map_browser_result(
        rock_open_foreground(path, file, false, false, NULL));
}

int rock_open_from_browser_quiet(const char *path, const char *file,
                                 bool *failed)
{
    int result = rock_open_foreground(path, file, false, true, NULL);
    if (failed)
        *failed = (result == PLUGIN_ERROR);
    return rock_map_browser_result(result);
}
