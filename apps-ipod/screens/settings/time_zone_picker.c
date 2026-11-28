/* AstraPod Settings > Date & Time > Time Zone. GPL version 2 or later. */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "system/time_zone.h"
#include "system/time_zone_catalog.h"
#include "ui/scene.h"
#include "widgets/list.h"
#include "widgets/menu.h"

static const char *const regions[] = {
    "Etc", "Africa", "America", "Antarctica", "Asia", "Atlantic",
    "Australia", "Europe", "Indian", "Pacific",
};

static int zone_region(const char *name)
{
    const char *slash = strchr(name, '/');
    if (slash)
        for (unsigned i = 0; i < sizeof(regions) / sizeof(*regions); i++)
            if (slash - name == (int)strlen(regions[i]) &&
                !strncmp(name, regions[i], slash - name))
                return i;
    return -1;
}

static const char *region_name(int i, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    return i == 0 ? "UTC" : regions[i];
}

struct city_list {
    int region, count;
    int selected;
    int indices[384]; /* America is the largest picker region. */
};

static const char *city_name(int item, void *data, char *buf, size_t size)
{
    const struct city_list *list = data;
    const char *name = time_zone_name(list->indices[item], true);
    strlcpy(buf, strchr(name, '/') + 1, size);
    for (char *p = buf; *p; p++)
        if (*p == '_')
            *p = ' ';
    return buf;
}

static void fill_cities(struct city_list *list)
{
    list->count = list->selected = 0;
    size_t len = strcspn(global_settings.time_zone, "|");
    for (int i = 0; i < time_zone_count(); i++)
    {
        const char *name = time_zone_name(i, true);
        if (!name || zone_region(name) != list->region)
            continue;
        if (strlen(name) == len && !strncmp(name, global_settings.time_zone, len))
            list->selected = list->count;
        list->indices[list->count++] = i;
    }
}

int time_zone_picker(void)
{
    struct simplelist_info info;
    static struct city_list cities;
    struct ui_scene scene = {0};
    int region = zone_region(global_settings.time_zone);
    if (region < 0)
        region = 0;

    ui_scene_open_page(&scene, "time-zone-regions");
    bool usb = false;
    const char *chosen = NULL;
    while (!usb && !chosen)
    {
        simplelist_info_init(&info, str(LANG_FRUIT_TIME_ZONE),
                             sizeof(regions) / sizeof(*regions), NULL);
        info.fruit_nav = true;
        info.get_name = region_name;
        info.selection = region;
        usb = simplelist_show_list(&info);
        if (usb || info.selection < 0)
            break;
        region = info.selection;
        if (region == 0)
        {
            chosen = TIME_ZONE_DEFAULT;
            break;
        }

        cities.region = region;
        fill_cities(&cities);
        simplelist_info_init(&info, (char *)regions[region], cities.count, &cities);
        info.fruit_nav = true;
        info.get_name = city_name;
        info.selection = cities.selected;
        struct ui_scene page = {0};
        ui_scene_open_page(&page, "time-zone-cities");
        usb = simplelist_show_list(&info);
        ui_scene_close_modal(&page, !usb, "time-zone-cities-back");
        if (!usb && info.selection >= 0)
            chosen = time_zone_name(cities.indices[info.selection], true);
    }
    if (chosen)
    {
        char value[TIME_ZONE_NAME_SIZE];
        strlcpy(value, chosen, sizeof(value));
        time_zone_cfg_load(global_settings.time_zone, value);
        settings_save();
    }
    ui_scene_close_modal(&scene, !usb, "time-zone-back");
    return usb ? MENU_ATTACHED_USB : 0;
}
