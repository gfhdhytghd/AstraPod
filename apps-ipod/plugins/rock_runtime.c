/* AstraPod foreground plugin runtime. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "plugins/rock_files.h"
#include "plugins/rock_widgets.h"
#include "plugins/sdk/plugin.h"
#include "rock-compat/rock_fingerprint.h"
#include "core_alloc.h"
#include "root_menu.h"
#include "speech/language.h"
#include "lang.h"
#include "system/app_buffer.h"
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

void rock_usb_takeover(void)
{
    if (!filename || usb_taken)
        return;
    rock_flush_settings();
    usb_taken = true;
    rock_close_files();
}

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

static const struct settings_list *rock_find_setting(const void *variable)
{
#ifdef ROCK_COMPAT_GENERATED
    /* Playback Control asks for these two settings through the generated view. */
    if (variable == &settings_view.playlist_shuffle)
        variable = &global_settings.playlist_shuffle;
    else if (variable == &settings_view.repeat_mode)
        variable = &global_settings.repeat_mode;
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

static long rock_event_ex(long event, void (*cleanup)(void *), void *parameter)
{
    if (usb_taken)
        return SYS_USB_CONNECTED;
    if (!IS_SYSEVENT(event) || (!rock_ui_raw() && event != SYS_USB_CONNECTED))
        return default_event_handler_ex(event, cleanup, parameter);

    struct rock_native_scope scope = rock_modal_begin();
    long result = default_event_handler_ex(event, cleanup, parameter);
    rock_modal_end(&scope);
    return result;
}

static long rock_event(long event)
{
    return rock_event_ex(event, NULL, NULL);
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

static int run_rock(const char *path, unsigned char *arena, size_t capacity,
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
    rock_begin_settings();
    rock_display_start(saved_pixels, widget_pixels);
    button_clear_pressed();

    result = module->entry_point(NULL);
    if (usb_taken)
        result = PLUGIN_USB_CONNECTED;
    rock_flush_settings();
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

int rock_open_from_browser(const char *path)
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
    int result = run_rock(path, arena, capacity, caller_pixels, widget_pixels);
    app_release_buffer(arena_owner);

    if (result == PLUGIN_USB_CONNECTED || result == PLUGIN_GOTO_ROOT)
        return GO_TO_ROOT;
    if (result == PLUGIN_GOTO_WPS)
        return GO_TO_WPS;
    if (result == PLUGIN_ERROR)
        splash(HZ, ID2P(LANG_PLUGIN_ERROR));
    return GO_TO_PREVIOUS;
}
