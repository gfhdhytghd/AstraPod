/* Specialist plugin services. GNU GPL version 2 or later. */
#include "plugins/rock_specialist.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_files.h"
#include "plugins/rock_widgets.h"
#include "plugins/sdk/plugin.h"
#include "screens/browse/browser.h"
#include "screens/settings/exported_settings.h"
#include "viewers/properties.h"
#include "files/filetypes.h"
#include "root_menu.h"
#include "usb.h"

#ifdef HAVE_PITCHCONTROL
#include "audio/spoken_speed.h"
#include "dsp_misc.h"
#endif

#define NATIVE_THEME_DIR ROCKBOX_DIR "/native-themes"

struct picker_filter {
    const char *extensions;
    unsigned int attr;
    bool hide_dirs;
};

static const struct picker_filter *picker;

static bool extension_matches(const char *name, const char *list)
{
    if (!list)
        return false;

    const char *suffix = strrchr(name, '.');
    size_t length = suffix ? strlen(suffix) : 0;
    while (*list)
    {
        if (*list != '.')
        {
            list++;
            continue;
        }
        const char *end = list + 1;
        while (*end && *end != '.' && *end != ' ' && *end != ',' &&
               *end != ';')
            end++;
        size_t count = end - list;
        if ((count == 2 && list[1] == '*') ||
            (suffix && count == length && !strncasecmp(list, suffix, count)))
            return true;
        list = end;
    }
    return false;
}

static bool show_picked_item(char *name, int attr,
                             struct browser_context *context)
{
    (void)context;
    if (attr & ATTR_DIRECTORY)
        return !picker->hide_dirs;

    return (picker->attr && (attr & FILE_ATTR_MASK) == picker->attr) ||
           (!picker->attr && !picker->extensions) ||
           extension_matches(name, picker->extensions);
}

int rock_pick_file(char *path, size_t size, const char *root, const char *title,
                   const char *extensions, unsigned int attr, bool hide_dirs)
{
    if (rock_usb_taken() || picker || !path || size < MAX_PATH ||
        !root || *root != '/' || strlen(root) >= MAX_PATH)
        return -1;

    char start[MAX_PATH];
    size_t length = strlcpy(start, root, sizeof(start));
    while (length > 1 && start[length - 1] == '/')
        start[--length] = '\0';
    if (!rock_dir_exists(start))
        return -1;

    struct picker_filter filter = {
        .extensions = extensions && *extensions ? extensions : NULL,
        .attr = attr & FILE_ATTR_MASK,
        .hide_dirs = hide_dirs,
    };
    struct browse_context browse = {
        .dirfilter = SHOW_ALL,
        .flags = BROWSE_DIRFILTER | BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU |
                 BROWSE_PLUGIN_PICKER,
        .callback_show_item = show_picked_item,
        .title = (char *)title,
        .icon = Icon_Plugin,
        .root = start,
        .buf = path,
        .bufsize = size,
    };
    *path = '\0';
    picker = &filter;
    struct rock_native_scope scope = rock_modal_begin();
    bool previous = action_set_menu_back(true);
    rockbox_browse(&browse);
    action_set_menu_back(previous);
    rock_modal_end(&scope);
    picker = NULL;
    if (rock_usb_taken())
        return -1;
    return (browse.flags & BROWSE_SELECTED) != 0;
}

int rock_properties(const char *path)
{
    if (rock_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (!path || *path != '/')
        return PLUGIN_ERROR;

    struct rock_native_scope scope = rock_modal_begin();
    properties(path);
    rock_modal_end(&scope);
    return rock_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}

int rock_main_menu_config(void)
{
    if (rock_usb_taken())
        return PLUGIN_USB_CONNECTED;

    struct rock_native_scope scope = rock_modal_begin();
    main_menu_config();
    rock_modal_end(&scope);
    return rock_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}

static const char *child_name(const char *path, const char *root)
{
    size_t length = strlen(root);
    if (strncasecmp(path, root, length) || path[length] != '/')
        return NULL;
    const char *name = path + length + 1;
    if (!*name || strchr(name, '/') || strchr(name, '\\') ||
        !strcmp(name, ".") || !strcmp(name, ".."))
        return NULL;
    return name;
}

int rock_remove_theme_file(const char *path)
{
    if (!path || rock_usb_taken())
        return -1;

    const char *name = child_name(path, THEME_DIR);
    bool native = name == NULL;
    if (native)
        name = child_name(path, NATIVE_THEME_DIR);
    if (!name)
        return -1;

    const char *extension = strrchr(name, '.');
    if (!extension || extension == name ||
        strcasecmp(extension, native ? ".podtheme" : ".cfg"))
        return -1;
    size_t length = extension - name;
    const char *active = global_settings.native_theme;
    if ((length == 5 && !strncasecmp(name, "scrim", length)) ||
        (length == 7 && (!strncasecmp(name, "classic", length) ||
                         !strncasecmp(name, "plastic", length))) ||
        (length == 13 && !strncasecmp(name, "fruit-company", length)) ||
        (length == 19 && !strncasecmp(name, "fruit-company-ultra", length)) ||
        (native && strlen(active) == length &&
         !strncasecmp(name, active, length)))
        return -1;

    if (!rock_file_exists(path) || rock_remove(path) < 0)
        return -1;
    reload_directory();
    return 0;
}

static struct rock_handoff handoff;
static enum { HANDOFF_NONE, HANDOFF_QUEUED, HANDOFF_RUNNING } handoff_state;

int rock_queue_handoff(const char *module, const char *path)
{
    if (rock_usb_taken() || handoff_state != HANDOFF_NONE ||
        !module || (path && (*path != '/' || !rock_file_exists(path))))
        return PLUGIN_ERROR;

    int length = snprintf(handoff.module, sizeof(handoff.module), "%s%s",
                          *module == '/' ? "" : PLUGIN_DIR "/", module);
    if (length < 0 || (size_t)length >= sizeof(handoff.module) ||
        strlcpy(handoff.path, path ? path : "", sizeof(handoff.path)) >=
        sizeof(handoff.path))
        return PLUGIN_ERROR;

    static const char *const categories[] = {
        PLUGIN_APPS_DIR, PLUGIN_DEMOS_DIR, PLUGIN_GAMES_DIR, VIEWERS_DIR,
    };
    for (size_t i = 0; i < ARRAYLEN(categories); i++)
    {
        const char *name = child_name(handoff.module, categories[i]);
        if (name && extension_matches(name, ".rock") &&
            rock_file_exists(handoff.module))
        {
            handoff.kind = ROCK_HANDOFF_PLUGIN;
            handoff_state = HANDOFF_QUEUED;
            return PLUGIN_GOTO_PLUGIN;
        }
    }
    return PLUGIN_ERROR;
}

int rock_queue_native_handoff(enum rock_handoff_kind kind, const char *path)
{
    if (rock_usb_taken() || handoff_state != HANDOFF_NONE || !path ||
        *path != '/' || kind < ROCK_HANDOFF_IMAGE ||
        kind > ROCK_HANDOFF_BROWSE ||
        strlcpy(handoff.path, path, sizeof(handoff.path)) >= sizeof(handoff.path))
        return PLUGIN_ERROR;
    if (!rock_file_exists(path) &&
        !(kind == ROCK_HANDOFF_BROWSE && rock_dir_exists(path)))
        return PLUGIN_ERROR;

    handoff.kind = kind;
    handoff_state = HANDOFF_QUEUED;
    return PLUGIN_GOTO_PLUGIN;
}

const struct rock_handoff *rock_take_handoff(void)
{
    if (handoff_state != HANDOFF_QUEUED)
        return NULL;
    handoff_state = HANDOFF_RUNNING;
    return &handoff;
}

void rock_clear_handoff(void)
{
    handoff_state = HANDOFF_NONE;
}

#ifdef HAVE_PITCHCONTROL
#define PITCH_PREVIEW_MIN 80
#define PITCH_PREVIEW_MAX 125

extern bool audio_is_initialized;

static struct mutex pitch_lock;
static bool pitch_owned;
static int32_t original_pitch;
static unsigned int pitch_generation;

void rock_pitch_preview_init(void)
{
    mutex_init(&pitch_lock);
}

static bool pitch_available(void)
{
#ifdef USB_ENABLE_AUDIO
    if (usb_audio_get_active())
        return false;
#endif
    return !rock_usb_taken() && (audio_status() & AUDIO_STATUS_PLAY) &&
           !spoken_speed_playing();
}

static const char *pitch_value(int item, void *data, char *buffer, size_t size)
{
    (void)data;
    snprintf(buffer, size, "%d%%", item + PITCH_PREVIEW_MIN);
    return buffer;
}

static int pitch_action(int action, struct gui_synclist *list)
{
    if (!gui_synclist_get_nb_items(list) || IS_SYSEVENT(action))
        return action;

    int value = gui_synclist_get_sel_pos(list) + PITCH_PREVIEW_MIN;
    bool available = pitch_available();
    mutex_lock(&pitch_lock);
    bool preview = pitch_owned && available;
    if (preview && action != ACTION_STD_CANCEL && action != ACTION_STD_MENU &&
        action != ACTION_STD_OK)
        dsp_set_pitch(value * PITCH_SPEED_PRECISION);
    mutex_unlock(&pitch_lock);
    return preview ? action : ACTION_STD_CANCEL;
}

void rock_pitch_preview_revoke(void)
{
    if (!audio_is_initialized)
        return;

    mutex_lock(&pitch_lock);
    pitch_generation++;
    if (pitch_owned)
    {
        dsp_set_pitch(original_pitch);
        pitch_owned = false;
    }
    mutex_unlock(&pitch_lock);
}

int rock_pitch_preview(void)
{
    if (rock_usb_taken())
        return PLUGIN_USB_CONNECTED;

    struct rock_native_scope scope = rock_modal_begin();
    mutex_lock(&pitch_lock);
    unsigned int generation = pitch_generation;
    mutex_unlock(&pitch_lock);

    /* Reading track metadata can lock playback. A track change revokes
     * the preview under pitch_lock, so check again before acquiring it. */
    bool available = pitch_available();
    mutex_lock(&pitch_lock);
    if (available && pitch_generation == generation)
    {
        original_pitch = dsp_get_pitch();
        pitch_owned = true;
    }
    available = pitch_owned;
    mutex_unlock(&pitch_lock);

    if (available)
    {
        int percent = (original_pitch + PITCH_SPEED_PRECISION / 2) /
                      PITCH_SPEED_PRECISION;
        struct simplelist_info list;
        simplelist_info_init(&list, "Pitch preview",
                             PITCH_PREVIEW_MAX - PITCH_PREVIEW_MIN + 1, NULL);
        list.get_name = pitch_value;
        list.action_callback = pitch_action;
        list.selection = MIN(MAX(percent, PITCH_PREVIEW_MIN), PITCH_PREVIEW_MAX)
                         - PITCH_PREVIEW_MIN;
        simplelist_show_list(&list);
        rock_pitch_preview_revoke();
    }
    else
        splash(HZ * 2, "Play a music track to preview pitch");
    rock_modal_end(&scope);
    return rock_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
#endif
