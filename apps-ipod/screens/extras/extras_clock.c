/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Clocks, as the stock iPod Classic draws its world clock: a row
 * per clock over the world map, each with an analogue face, the city, its
 * time and its day. Select opens Add | Edit (| Delete), and Add and Edit
 * pick a city from the Region and City lists, whose pane shows a large clock.
 *
 * Each city displays the same instant in its own time zone.
 *
 * Parts: the cities; the time arithmetic; the saved clocks; the painters,
 * which compose at 8 bits a channel as stock does; the Region and City
 * lists; the screen.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "core_alloc.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "system/time_zone.h"
#include "input/action.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "root_menu.h"
#include "extras.h"
#include "extras_clock.h"

/* ---- the cities ---- */

struct city {
    const char *name;
    const char *zone;
};

/* In stock's order, region by region. */
static const struct city cities[] = {
    /* Africa */
    { "Accra", "Africa/Accra" }, { "Addis Ababa", "Africa/Addis_Ababa" },
    { "Algiers", "Africa/Algiers" }, { "Asmera", "Africa/Asmera" },
    { "Bamako", "Africa/Bamako" }, { "Bangui", "Africa/Bangui" },
    { "Cairo", "Africa/Cairo" }, { "Cape Town", "Africa/Johannesburg" },
    { "Conakry", "Africa/Conakry" }, { "Dakar", "Africa/Dakar" },
    { "Dar es Salaam", "Africa/Dar_es_Salaam" },
    { "Djibouti", "Africa/Djibouti" },
    { "Douala", "Africa/Douala" }, { "Freetown", "Africa/Freetown" },
    { "Harare", "Africa/Harare" }, { "Kampala", "Africa/Kampala" },
    { "Khartoum", "Africa/Khartoum" }, { "Kinshasa", "Africa/Kinshasa" },
    { "Lagos", "Africa/Lagos" }, { "Luanda", "Africa/Luanda" },
    { "Lusaka", "Africa/Lusaka" }, { "Maputo", "Africa/Maputo" },
    { "Mogadishu", "Africa/Mogadishu" }, { "Monrovia", "Africa/Monrovia" },
    { "Nairobi", "Africa/Nairobi" }, { "Ndjamena", "Africa/Ndjamena" },
    { "Nouakchott", "Africa/Nouakchott" },
    { "Ouagadougou", "Africa/Ouagadougou" },
    { "Rabat", "Africa/Casablanca" }, { "Tripoli", "Africa/Tripoli" },
    { "Tunis", "Africa/Tunis" },
    /* Asia */
    { "Abu Dhabi", "Asia/Dubai" }, { "Amman", "Asia/Amman" },
    { "Anadyr", "Asia/Anadyr" }, { "Ankara", "Asia/Istanbul" },
    { "Antananarivo", "Indian/Antananarivo" }, { "Baghdad", "Asia/Baghdad" },
    { "Bangkok", "Asia/Bangkok" }, { "Beijing", "Asia/Shanghai" },
    { "Beirut", "Asia/Beirut" }, { "Canton", "Asia/Shanghai" },
    { "Chennai (Madras)", "Asia/Kolkata" }, { "Colombo", "Asia/Colombo" },
    { "Dhaka", "Asia/Dhaka" }, { "Damascus", "Asia/Damascus" },
    { "Doha", "Asia/Qatar" }, { "Hanoi", "Asia/Ho_Chi_Minh" },
    { "Hong Kong", "Asia/Hong_Kong" }, { "Islamabad", "Asia/Karachi" },
    { "Jakarta", "Asia/Jakarta" }, { "Jerusalem", "Asia/Jerusalem" },
    { "Kabul", "Asia/Kabul" }, { "Katmandu", "Asia/Kathmandu" },
    { "Kolkata (Calcutta)", "Asia/Kolkata" },
    { "Krasnoyarsk", "Asia/Krasnoyarsk" },
    { "Kuala Lumpur", "Asia/Kuala_Lumpur" }, { "Kuwait", "Asia/Kuwait" },
    { "Magadan", "Asia/Magadan" }, { "Male", "Indian/Maldives" },
    { "Manama", "Asia/Bahrain" }, { "Manila", "Asia/Manila" },
    { "Mecca", "Asia/Riyadh" }, { "Mumbai (Bombay)", "Asia/Kolkata" },
    { "Muscat", "Asia/Muscat" }, { "New Delhi", "Asia/Kolkata" },
    { "Novosibirsk", "Asia/Novosibirsk" }, { "Omsk", "Asia/Omsk" },
    { "Osaka", "Asia/Tokyo" }, { "Phnom Penh", "Asia/Phnom_Penh" },
    { "Port Louis", "Indian/Mauritius" }, { "Pyongyang", "Asia/Pyongyang" },
    { "Rangoon", "Asia/Yangon" }, { "Riyadh", "Asia/Riyadh" },
    { "Sanaa", "Asia/Aden" }, { "Seoul", "Asia/Seoul" },
    { "Shanghai", "Asia/Shanghai" }, { "Singapore", "Singapore" },
    { "Taipei", "Asia/Taipei" }, { "Tashkent", "Asia/Tashkent" },
    { "Tehran", "Asia/Tehran" },
    { "Thanh Pho Ho Chi Minh", "Asia/Ho_Chi_Minh" },
    { "Tientsin", "Asia/Shanghai" }, { "Tokyo", "Asia/Tokyo" },
    { "Ulaanbaatar", "Asia/Ulaanbaatar" }, { "Victoria", "Indian/Mahe" },
    { "Vladivostok", "Asia/Vladivostok" }, { "Yakutsk", "Asia/Yakutsk" },
    { "Yekaterinburg", "Asia/Yekaterinburg" },
    /* Atlantic */
    { "Grytviken", "Atlantic/South_Georgia" },
    { "Ponta Delgada", "Atlantic/Azores" },
    { "Reykjav\xc3\xadk", "Atlantic/Reykjavik" },
    /* Australia */
    { "Adelaide", "Australia/Adelaide" }, { "Brisbane", "Australia/Brisbane" },
    { "Canberra", "Australia/Sydney" }, { "Darwin", "Australia/Darwin" },
    { "Hobart", "Australia/Hobart" }, { "Melbourne", "Australia/Melbourne" },
    { "Perth", "Australia/Perth" }, { "Sydney", "Australia/Sydney" },
    /* Europe */
    { "Amsterdam", "Europe/Amsterdam" }, { "Athens", "Europe/Athens" },
    { "Belgrade", "Europe/Belgrade" }, { "Berlin", "Europe/Berlin" },
    { "Brussels", "Europe/Brussels" }, { "Bucharest", "Europe/Bucharest" },
    { "Budapest", "Europe/Budapest" }, { "Cardiff", "Europe/London" },
    { "Copenhagen", "Europe/Copenhagen" }, { "Cork", "Europe/Dublin" },
    { "Dublin", "Europe/Dublin" }, { "Edinburgh", "Europe/London" },
    { "Geneva", "Europe/Zurich" }, { "Helsinki", "Europe/Helsinki" },
    { "Istanbul", "Asia/Istanbul" }, { "Kiev", "Europe/Kyiv" },
    { "Lisbon", "Europe/Lisbon" }, { "Ljubljana", "Europe/Ljubljana" },
    { "London", "Europe/London" }, { "Madrid", "Europe/Madrid" },
    { "Moscow", "Europe/Moscow" }, { "Munich", "Europe/Berlin" },
    { "Oslo", "Europe/Oslo" }, { "Paris", "Europe/Paris" },
    { "Prague", "Europe/Prague" }, { "Roma (Rome)", "Europe/Rome" },
    { "Sofia", "Europe/Sofia" }, { "St. Petersburg", "Europe/Moscow" },
    { "Stockholm", "Europe/Stockholm" }, { "Vienna", "Europe/Vienna" },
    { "Volgograd", "Europe/Volgograd" }, { "Warsaw", "Europe/Warsaw" },
    { "Zagreb", "Europe/Zagreb" }, { "Zurich", "Europe/Zurich" },
    /* North America */
    { "Adak", "America/Adak" }, { "Anchorage", "America/Anchorage" },
    { "Atlanta", "America/New_York" }, { "Austin", "America/Chicago" },
    { "Boston", "America/New_York" }, { "Calgary", "America/Edmonton" },
    { "California", "America/Los_Angeles" }, { "Chicago", "America/Chicago" },
    { "Columbus", "America/New_York" }, { "Cupertino", "America/Los_Angeles" },
    { "Dallas", "America/Chicago" }, { "Des Moines", "America/Chicago" },
    { "Denver", "America/Denver" }, { "Detroit", "America/Detroit" },
    { "Halifax", "America/Halifax" },
    { "Indianapolis", "America/Indiana/Indianapolis" },
    { "Knoxville", "America/New_York" },
    { "Los Angeles", "America/Los_Angeles" },
    { "Manchester", "America/New_York" }, { "Memphis", "America/Chicago" },
    { "Miami", "America/New_York" }, { "Minneapolis", "America/Chicago" },
    { "Montr\xc3\xa9" "al", "America/Toronto" },
    { "New York", "America/New_York" },
    { "Nuuk", "America/Nuuk" }, { "Ottawa", "America/Toronto" },
    { "Philadelphia", "America/New_York" }, { "Phoenix", "America/Phoenix" },
    { "Portland", "America/Los_Angeles" }, { "Regina", "America/Regina" },
    { "Salt Lake City", "America/Denver" },
    { "San Diego", "America/Los_Angeles" },
    { "San Francisco", "America/Los_Angeles" },
    { "Seattle", "America/Los_Angeles" },
    { "St. John's", "America/St_Johns" }, { "St. Louis", "America/Chicago" },
    { "Toronto", "America/Toronto" }, { "Vancouver", "America/Vancouver" },
    { "Washington, D.C.", "America/New_York" },
    { "Winnipeg", "America/Winnipeg" },
    /* Pacific */
    { "Guam", "Pacific/Guam" }, { "Honolulu", "Pacific/Honolulu" },
    { "Noum\xc3\xa9" "a", "Pacific/Noumea" },
    { "Pago Pago", "Pacific/Pago_Pago" },
    { "Wellington", "Pacific/Auckland" },
    /* South America */
    { "Asunci\xc3\xb3n", "America/Asuncion" },
    { "Bogot\xc3\xa1", "America/Bogota" },
    { "Bras\xc3\xadlia", "America/Sao_Paulo" },
    { "Buenos Aires", "America/Argentina/Buenos_Aires" },
    { "Caracas", "America/Caracas" }, { "Cayenne", "America/Cayenne" },
    { "Georgetown", "America/Guyana" }, { "Guatemala", "America/Guatemala" },
    { "Havana", "America/Havana" }, { "La Paz", "America/La_Paz" },
    { "Lima", "America/Lima" }, { "Managua", "America/Managua" },
    { "Mexico City", "America/Mexico_City" },
    { "Montevideo", "America/Montevideo" },
    { "Panama", "America/Panama" }, { "Paramaribo", "America/Paramaribo" },
    { "Port-au-Prince", "America/Port-au-Prince" },
    { "Quito", "America/Guayaquil" },
    { "Recife", "America/Recife" }, { "Rio de Janeiro", "America/Sao_Paulo" },
    { "San Jos\xc3\xa9", "America/Costa_Rica" },
    { "San Juan", "America/Puerto_Rico" },
    { "San Salvador", "America/El_Salvador" },
    { "Santiago", "America/Santiago" },
    { "Santo Domingo", "America/Santo_Domingo" },
    { "S\xc3\xa3o Paulo", "America/Sao_Paulo" },
    { "Tegucigalpa", "America/Tegucigalpa" },
};


#define REGIONS 8
/* Each region's first city, and the end of the table. */
static const uint8_t region_first[REGIONS + 1] = {
    0, 31, 88, 91, 99, 133, 173, 178, 205
};
static const int region_name[REGIONS] = {
    LANG_EXTRAS_CLOCK_AFRICA, LANG_EXTRAS_CLOCK_ASIA,
    LANG_EXTRAS_CLOCK_ATLANTIC, LANG_EXTRAS_CLOCK_AUSTRALIA,
    LANG_EXTRAS_CLOCK_EUROPE, LANG_EXTRAS_CLOCK_NORTH_AMERICA,
    LANG_EXTRAS_CLOCK_PACIFIC, LANG_EXTRAS_CLOCK_SOUTH_AMERICA,
};
/* Stock's first clock: California. */
#define DEFAULT_CITY 139

static int city_region(int city)
{
    int r = 0;
    while (r < REGIONS - 1 && city >= region_first[r + 1])
        r++;
    return r;
}

struct clock_time {
    int hour, minute;
    int day;                    /* -1 yesterday, 0 today, 1 tomorrow */
};

static struct clock_time city_time(int city)
{
    time_t utc = get_time_utc();
    if (utc < 0)
        return (struct clock_time){0};
    struct tm local;
    clock_localtime(utc, &local);
    long today = clock_civil_seconds(&local) / 86400;
    struct time_zone zone;
    int dst;
    if (time_zone_lookup(cities[city].zone, &zone))
        utc += time_zone_offset(&zone, utc, &dst);
    long day = utc / 86400;
    long minutes = utc / 60 % 1440;
    return (struct clock_time){ minutes / 60, minutes % 60,
                                MAX(-1, MIN(1, day - today)) };
}

static void format_time(char *buf, size_t size, int hour, int minute)
{
    if (global_settings.timeformat == 0)
        snprintf(buf, size, "%02d:%02d", hour, minute);
    else
        snprintf(buf, size, "%d:%02d %s", (hour + 11) % 12 + 1, minute,
                 hour < 12 ? "AM" : "PM");
}

/* ---- the saved clocks ---- */

#define MAX_CLOCKS 8
#define CLOCKS_FILE ROCKBOX_DIR "/clocks.txt"

static int clocks[MAX_CLOCKS];
static int clock_count;
/* Whether the clocks are the owner's rather than stock's default. */
static bool clocks_chosen;

static int find_city(const char *name)
{
    for (int i = 0; i < (int)ARRAYLEN(cities); i++)
        if (!strcmp(cities[i].name, name))
            return i;
    return -1;
}

/* One city name a line, the player's own first. */
static void clocks_load(void)
{
    clock_count = 0;
    int fd = open(CLOCKS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        char line[48];
        while (clock_count < MAX_CLOCKS &&
               read_line(fd, line, sizeof(line)) > 0)
        {
            int city = find_city(line);
            if (city >= 0)
                clocks[clock_count++] = city;
        }
        close(fd);
    }
    clocks_chosen = clock_count > 0;
    if (!clock_count)
        clocks[clock_count++] = DEFAULT_CITY;
}

static void clocks_save(void)
{
    int fd = open(CLOCKS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    clocks_chosen = true;
    for (int i = 0; i < clock_count; i++)
        fdprintf(fd, "%s\n", cities[clocks[i]].name);
    close(fd);
}

/* ---- the painters ---- */

#define ROW_H 80
#define VISIBLE_ROWS 3
#define FACE_X 125
#define FACE_Y 3
#define HAND_EDGE 73
#define LARGE_X 174
#define LARGE_Y 52
#define LARGE_HANDS_X 204
#define LARGE_HANDS_Y 82
#define NAME_X 5
#define TIME_RIGHT 298
#define SHADOW_H 26
#define DIALOG_Y 166
#define DIALOG_LEFT 18
#define DIALOG_RIGHT 302
#define BAR_Y 188
#define BUTTON_MIN_W 57
#define BUTTON_PAD 32
#define WHITE 0xffffff
#define BLACK 0x000000
#define DIM_TEXT 0x3d3d3d

/* The selected row's bar, top to bottom, as stock draws it. */
static const uint32_t selected_bar[ROW_H] = {
    0x96c5da, 0x09a4e1, 0x08a3e0, 0x08a2e0, 0x08a1e0, 0x08a0e0,
    0x089fe0, 0x089fdf, 0x089edf, 0x089ddf, 0x089cdf, 0x079bdf,
    0x079adf, 0x079ade, 0x0799de, 0x0798de, 0x0797de, 0x0796de,
    0x0795de, 0x0795dd, 0x0694dd, 0x0693dd, 0x0692dd, 0x0691dd,
    0x0690dd, 0x0690dc, 0x068fdc, 0x068edc, 0x068ddc, 0x058cdc,
    0x058bdc, 0x058bdb, 0x058adb, 0x0589db, 0x0588db, 0x0587db,
    0x0587da, 0x0586da, 0x0485da, 0x0484da, 0x0483da, 0x0482da,
    0x0482d9, 0x0481d9, 0x0480d9, 0x047fd9, 0x047ed9, 0x037dd9,
    0x037dd8, 0x037cd8, 0x037bd8, 0x037ad8, 0x0379d8, 0x0378d8,
    0x0378d7, 0x0377d7, 0x0276d7, 0x0275d7, 0x0274d7, 0x0273d7,
    0x0273d6, 0x0272d6, 0x0271d6, 0x0270d6, 0x026fd6, 0x026fd6,
    0x016ed5, 0x016dd5, 0x016cd5, 0x016bd5, 0x016ad5, 0x016ad4,
    0x0169d4, 0x0168d4, 0x0167d4, 0x0066d4, 0x0065d4, 0x0065d3,
    0x0064d3, 0x004386,
};

/* The frames of a hand from 12 to 3 o'clock, 3 degrees apart. The hour hand
 * has 30; the minute hand's 16 reach 1:30 and are mirrored past it. */
static const uint16_t hour_frames[30] = {
    STOCK_ART_CLOCK_HOURS_IMAGE_0, STOCK_ART_CLOCK_HOURS_IMAGE_1,
    STOCK_ART_CLOCK_HOURS_IMAGE_2, STOCK_ART_CLOCK_HOURS_IMAGE_3,
    STOCK_ART_CLOCK_HOURS_IMAGE_4, STOCK_ART_CLOCK_HOURS_IMAGE_5,
    STOCK_ART_CLOCK_HOURS_IMAGE_6, STOCK_ART_CLOCK_HOURS_IMAGE_7,
    STOCK_ART_CLOCK_HOURS_IMAGE_8, STOCK_ART_CLOCK_HOURS_IMAGE_9,
    STOCK_ART_CLOCK_HOURS_IMAGE_10, STOCK_ART_CLOCK_HOURS_IMAGE_11,
    STOCK_ART_CLOCK_HOURS_IMAGE_12, STOCK_ART_CLOCK_HOURS_IMAGE_13,
    STOCK_ART_CLOCK_HOURS_IMAGE_14, STOCK_ART_CLOCK_HOURS_IMAGE_15,
    STOCK_ART_CLOCK_HOURS_IMAGE_16, STOCK_ART_CLOCK_HOURS_IMAGE_17,
    STOCK_ART_CLOCK_HOURS_IMAGE_18, STOCK_ART_CLOCK_HOURS_IMAGE_19,
    STOCK_ART_CLOCK_HOURS_IMAGE_20, STOCK_ART_CLOCK_HOURS_IMAGE_21,
    STOCK_ART_CLOCK_HOURS_IMAGE_22, STOCK_ART_CLOCK_HOURS_IMAGE_23,
    STOCK_ART_CLOCK_HOURS_IMAGE_24, STOCK_ART_CLOCK_HOURS_IMAGE_25,
    STOCK_ART_CLOCK_HOURS_IMAGE_26, STOCK_ART_CLOCK_HOURS_IMAGE_27,
    STOCK_ART_CLOCK_HOURS_IMAGE_28, STOCK_ART_CLOCK_HOURS_IMAGE_29,
};
static const uint16_t minute_frames[16] = {
    STOCK_ART_CLOCK_MINS_IMAGE_0, STOCK_ART_CLOCK_MINS_IMAGE_1,
    STOCK_ART_CLOCK_MINS_IMAGE_2, STOCK_ART_CLOCK_MINS_IMAGE_3,
    STOCK_ART_CLOCK_MINS_IMAGE_4, STOCK_ART_CLOCK_MINS_IMAGE_5,
    STOCK_ART_CLOCK_MINS_IMAGE_6, STOCK_ART_CLOCK_MINS_IMAGE_7,
    STOCK_ART_CLOCK_MINS_IMAGE_8, STOCK_ART_CLOCK_MINS_IMAGE_9,
    STOCK_ART_CLOCK_MINS_IMAGE_10, STOCK_ART_CLOCK_MINS_IMAGE_11,
    STOCK_ART_CLOCK_MINS_IMAGE_12, STOCK_ART_CLOCK_MINS_IMAGE_13,
    STOCK_ART_CLOCK_MINS_IMAGE_14, STOCK_ART_CLOCK_MINS_IMAGE_15,
};

/* The hand at `step` of 120 round the dial, its frame's top-left at (x, y):
 * the frame for its angle within a quarter, turned clockwise into its
 * quarter. */
static void put_hand(struct stock_canvas *c, bool hour, int step, int x,
                     int y, uint32_t rgb)
{
    int quarter = step / 30, within = step % 30;
    bool mirror = !hour && within > 15;
    if (!stock_art_ready())
        return;
    const struct stock_art *art = &stock_art[hour ? hour_frames[within] :
        minute_frames[mirror ? 30 - within : within]];
    const int last = HAND_EDGE - 1;
    for (int sy = 0; sy < HAND_EDGE; sy++)
        for (int sx = 0; sx < HAND_EDGE; sx++)
        {
            unsigned a = stock_art_alpha(art, sx, sy);
            if (!a)
                continue;
            int u = sx, v = sy;
            if (mirror)
            {
                u = last - sy;
                v = last - sx;
            }
            for (int q = 0; q < quarter; q++)
            {
                int t = u;
                u = last - v;
                v = t;
            }
            stock_canvas_blend(c, x + u, y + v, rgb, a);
        }
}

/* The hands and centre cap of a clock at `hour`:`minute`, the 73-pixel
 * frames' top-left at (x, y). */
static void put_hands(struct stock_canvas *c, int x, int y, int hour,
                      int minute, uint32_t rgb)
{
    put_hand(c, true, ((hour % 12) * 60 + minute) / 6, x, y, rgb);
    put_hand(c, false, minute * 2, x, y, rgb);
    stock_art_put(c, STOCK_ART_CLOCK_CENTERCAP, x, y);
}

/* The world map, a mask whose pixels stock lays over the canvas as
 * (d' * (255 - a) + 252 * a) / 255, with d' = d + d / 128. */
static void put_map(struct stock_canvas *c)
{
    const struct stock_art *art = &stock_art[STOCK_ART_WORLDCLOCK_MAP];
    if (!stock_art_ready())
        return;
    for (int y = MAX(c->y, 0); y < MIN(c->y + c->height, art->height); y++)
    {
        uint32_t *row = c->pixels + (y - c->y) * c->width - c->x;
        for (int x = MAX(c->x, 0); x < MIN(c->x + c->width, art->width); x++)
        {
            unsigned a = stock_art_alpha(art, x, y);
            if (!a)
                continue;
            uint32_t d = row[x], out = 0;
            for (int shift = 0; shift <= 16; shift += 8)
            {
                unsigned v = (d >> shift) & 255;
                v += v >> 7;
                out |= ((v * (255 - a) + 252 * a) / 255) << shift;
            }
            row[x] = out;
        }
    }
}

/* `id`, one column wide, across columns [`from`, `to`) at row `y`. */
static void put_stretched(struct stock_canvas *c, enum stock_art_id id,
                          int from, int to, int y)
{
    for (int x = from; x < to; x++)
        stock_art_put_part(c, id, 0, 0, 1, stock_art[id].height, x, y);
}

/* Fill the canvas's rows of `top`..`top + ROW_H` with the selected bar. */
static void put_selected_bar(struct stock_canvas *c, int top)
{
    for (int y = MAX(top, c->y); y < MIN(top + ROW_H, c->y + c->height); y++)
    {
        uint32_t *row = c->pixels + (y - c->y) * c->width;
        for (int x = 0; x < c->width; x++)
            row[x] = selected_bar[y - top];
    }
}

void extras_clock_put_large(struct stock_canvas *band, int hour, int minute)
{
    put_map(band);
    stock_art_put(band, STOCK_ART_CLOCK_LARGE, LARGE_X, LARGE_Y);
    put_hands(band, LARGE_HANDS_X, LARGE_HANDS_Y, hour, minute, BLACK);
}

void extras_clock_pane_paint(struct stock_canvas *band)
{
    struct tm *tm = get_time();
    extras_clock_put_large(band, tm->tm_hour, tm->tm_min);
}

/* ---- the screen's state and fonts ---- */

enum { BUTTON_ADD, BUTTON_EDIT, BUTTON_DELETE };

static struct {
    int selected, top;
    bool dialog;
    int button;
    int name_font, text_font;
    int band;                   /* core_alloc handle of the band buffer */
} screen;

#define BAND_ROWS 16

static int text_width(int font, const char *text)
{
    int w = 0;
    font_getstringsize((const unsigned char *)text, &w, NULL, font);
    return w;
}

static const char *day_name(int day)
{
    return str(day < 0 ? LANG_EXTRAS_CLOCK_YESTERDAY :
               day > 0 ? LANG_EXTRAS_CLOCK_TOMORROW : LANG_EXTRAS_CLOCK_TODAY);
}

/* Row `i` of the clock list with its top at `top`. */
static void put_row(struct stock_canvas *c, int i, int top)
{
    bool selected = i == screen.selected;
    struct clock_time t = city_time(clocks[i]);
    bool night = t.hour < 6 || t.hour >= 18;
    if (selected)
        put_selected_bar(c, top);
    else
        put_stretched(c, STOCK_ART_WORLDCLOCK_GRAYBAR, 0, LCD_WIDTH, top);
    stock_art_put(c, night ? STOCK_ART_CLOCK_SMALL_NIGHT
                           : STOCK_ART_CLOCK_SMALL, FACE_X, top + FACE_Y);
    put_hands(c, FACE_X, top + FACE_Y, t.hour, t.minute,
              night ? WHITE : BLACK);

    uint32_t name_rgb = selected ? WHITE : BLACK;
    uint32_t text_rgb = selected ? WHITE : DIM_TEXT;
    stock_canvas_text(c, screen.name_font, cities[clocks[i]].name, NAME_X,
                      top + 28, name_rgb);
    char time[16];
    format_time(time, sizeof(time), t.hour, t.minute);
    stock_canvas_text(c, screen.name_font, time,
                      TIME_RIGHT - text_width(screen.name_font, time),
                      top + 20, text_rgb);
    const char *day = day_name(t.day);
    stock_canvas_text(c, screen.text_font, day,
                      TIME_RIGHT - text_width(screen.text_font, day),
                      top + 43, text_rgb);
}

static int button_count(void)
{
    return clock_count > 1 ? 3 : 2;
}

static const char *button_name(int b)
{
    return str(b == BUTTON_ADD ? LANG_EXTRAS_CLOCK_ADD :
               b == BUTTON_EDIT ? LANG_EDIT : LANG_DELETE);
}

static int button_width(int b)
{
    return MAX(BUTTON_MIN_W,
               text_width(screen.text_font, button_name(b)) + BUTTON_PAD);
}

static void put_option_bar(struct stock_canvas *c, bool thumb, int x0, int x1)
{
    enum stock_art_id left = thumb ? STOCK_ART_OPTIONBAR_BLACK_THUMB_LEFTCAP :
                                     STOCK_ART_OPTIONBAR_BLACK_WELL_LEFTCAP;
    enum stock_art_id center = thumb ? STOCK_ART_OPTIONBAR_BLACK_THUMB_CENTER :
                                       STOCK_ART_OPTIONBAR_BLACK_WELL_CENTER;
    enum stock_art_id right = thumb ? STOCK_ART_OPTIONBAR_BLACK_THUMB_RIGHTCAP :
                                      STOCK_ART_OPTIONBAR_BLACK_WELL_RIGHTCAP;
    int lw = stock_art[left].width, rw = stock_art[right].width;
    stock_art_put(c, left, x0, BAR_Y);
    put_stretched(c, center, x0 + lw, x1 - rw, BAR_Y);
    stock_art_put(c, right, x1 - rw, BAR_Y);
}

/* The Add | Edit (| Delete) dialog over the bottom of the list. */
static void put_dialog(struct stock_canvas *c)
{
    int lw = stock_art[STOCK_ART_SYSTEM_OVERLAY_LEFT].width;
    int rw = stock_art[STOCK_ART_SYSTEM_OVERLAY_RIGHT].width;
    stock_art_put(c, STOCK_ART_SYSTEM_OVERLAY_LEFT, DIALOG_LEFT, DIALOG_Y);
    put_stretched(c, STOCK_ART_SYSTEM_OVERLAY_MIDDLE, DIALOG_LEFT + lw,
                  DIALOG_RIGHT - rw, DIALOG_Y);
    stock_art_put(c, STOCK_ART_SYSTEM_OVERLAY_RIGHT, DIALOG_RIGHT - rw,
                  DIALOG_Y);

    int n = button_count(), total = 0;
    for (int b = 0; b < n; b++)
        total += button_width(b);
    int x = (LCD_WIDTH - total) / 2;
    put_option_bar(c, false, x, x + total);
    for (int b = 0; b < n; b++)
    {
        int w = button_width(b);
        if (b == screen.button)
            put_option_bar(c, true, x, x + w);
        const char *name = button_name(b);
        stock_canvas_text(c, screen.text_font, name,
                          x + (w - text_width(screen.text_font, name)) / 2,
                          BAR_Y + 6, WHITE);
        x += w;
    }
}

/* Compose the rows `y`..`y + height` of the screen into `c`. */
static void compose(struct stock_canvas *c)
{
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    put_map(c);
    int shown = MIN(clock_count - screen.top, VISIBLE_ROWS);
    for (int r = 0; r < shown; r++)
        put_row(c, screen.top + r, r * ROW_H);
    int below = shown * ROW_H;
    for (int x = 0; below < LCD_HEIGHT && x < LCD_WIDTH;
         x += stock_art[STOCK_ART_CLOCK_SHADOW].width)
        stock_art_put(c, STOCK_ART_CLOCK_SHADOW, x, below);
    if (screen.dialog)
        put_dialog(c);
}

static void clock_paint(const struct ui_view *view,
                        const struct ui_view_frame *frame)
{
    (void)view;
    struct ui_rect clip = frame->clip;
    if (screen.band <= 0)
        return;
    uint32_t *pixels = core_get_data_pinned(screen.band);
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            pixels, 0, y, LCD_WIDTH,
            MIN(BAND_ROWS, clip.y + clip.height - y),
        };
        compose(&band);
        stock_canvas_show(&band, clip);
    }
    core_put_data_pinned(pixels);
}

static void load_fonts(void)
{
    int glyphs = global_settings.glyphs_to_cache;
    screen.name_font = ui_theme_current_font_load(UI_FONT_LIST, glyphs);
    screen.text_font = ui_theme_current_font_load(UI_FONT_PANE_TEXT, glyphs);
}

static void unload_fonts(void)
{
    font_unload(screen.name_font);
    font_unload(screen.text_font);
}

/* ---- the Region and City lists ---- */

/* The clock the pane shows, as minutes of the day. */
static int pane_time;
/* The region whose cities the City list shows. */
static int list_region;

static void pane_paint(struct stock_canvas *band)
{
    extras_clock_put_large(band, pane_time / 60, pane_time % 60);
}

/* The pane shows the large clock at `t`, with `caption` under it. */
static void show_pane(struct clock_time t, const char *caption)
{
    struct menu_pane_content pane = {
        .kind = LIST_PANE_PICTURE, .art = -1, .baseline = 221,
        .paint = pane_paint, .stamp = t.hour * 60 + t.minute,
    };
    strlcpy(pane.title, caption, sizeof(pane.title));
    pane_time = pane.stamp;
    menu_pane_set_picture(&pane);
}

static const char *region_get_name(int item, void *data, char *buf,
                                   size_t size)
{
    (void)data; (void)buf; (void)size;
    return str(region_name[item]);
}

static enum list_row_kind region_get_kind(int item, void *data)
{
    (void)item; (void)data;
    return LIST_ROW_MENU;
}

/* The Region list's pane shows Greenwich time. */
static enum list_pane_kind region_get_pane(int item, void *data, int *art)
{
    (void)item; (void)data; (void)art;
    show_pane(city_time(0), "");
    return LIST_PANE_PICTURE;
}

static const char *city_get_name(int item, void *data, char *buf,
                                 size_t size)
{
    (void)data; (void)buf; (void)size;
    return cities[region_first[list_region] + item].name;
}

static enum list_pane_kind city_get_pane(int item, void *data, int *art)
{
    (void)data; (void)art;
    struct clock_time t = city_time(region_first[list_region] + item);
    char caption[16];
    format_time(caption, sizeof(caption), t.hour, t.minute);
    show_pane(t, caption);
    return LIST_PANE_PICTURE;
}

static int list_minute = -1;

/* Installs the pane, and redraws the list when the minute turns. */
static int list_action(int action, struct gui_synclist *list,
                       list_get_pane *pane)
{
    int minute = get_time()->tm_min;
    if (action == ACTION_REDRAW)
        gui_synclist_set_pane_callback(list, pane);
    else if (action == ACTION_NONE && minute != list_minute)
        action = ACTION_REDRAW;
    list_minute = minute;
    return action;
}

static int region_action(int action, struct gui_synclist *list)
{
    return list_action(action, list, region_get_pane);
}

static int city_action(int action, struct gui_synclist *list)
{
    return list_action(action, list, city_get_pane);
}

/* Pick a city, starting from `city` or, for -1, the first region. Returns
 * the city, or -1. */
static int pick_city(int city)
{
    struct simplelist_info info;
    int region = city >= 0 ? city_region(city) : 0;
    struct ui_scene region_scene = {0};
    ui_scene_open_page(&region_scene, "clock-region-push");
    while (1)
    {
        simplelist_info_init(&info, str(LANG_EXTRAS_CLOCK_REGION), REGIONS,
                             NULL);
        info.get_name = region_get_name;
        info.get_kind = region_get_kind;
        info.action_callback = region_action;
        info.selection = region;
        info.speak_onshow = false;
        bool usb = simplelist_show_list(&info);
        if (info.selection < 0)
        {
            ui_scene_close_modal(&region_scene, !usb, "clock-region-pop");
            return -1;
        }
        region = info.selection;

        list_region = region;
        int first = region_first[region];
        simplelist_info_init(&info, str(LANG_EXTRAS_CLOCK_CITY),
                             region_first[region + 1] - first, NULL);
        info.get_name = city_get_name;
        info.get_kind = NULL;
        info.action_callback = city_action;
        info.selection = city >= first && city < region_first[region + 1] ?
                         city - first : 0;
        info.speak_onshow = false;
        struct ui_scene city_scene = {0};
        ui_scene_open_page(&city_scene, "clock-city-push");
        usb = simplelist_show_list(&info);
        ui_scene_close_modal(&city_scene, !usb, "clock-city-pop");
        if (info.selection >= 0 || usb)
        {
            ui_scene_close_modal(&region_scene, !usb, "clock-region-pop");
            return usb ? -1 : first + info.selection;
        }
    }
}

/* ---- the screen ---- */

static void keep_visible(void)
{
    if (screen.selected < screen.top)
        screen.top = screen.selected;
    else if (screen.selected >= screen.top + VISIBLE_ROWS)
        screen.top = screen.selected - VISIBLE_ROWS + 1;
}

/* Carry out the dialog's `button`. */
static void choose(int button)
{
    int city;
    switch (button)
    {
        case BUTTON_ADD:
            if (clock_count >= MAX_CLOCKS)
                break;
            city = pick_city(clocks_chosen ? clocks[screen.selected] : -1);
            if (city >= 0)
            {
                clocks[clock_count++] = city;
                clocks_save();
            }
            break;
        case BUTTON_EDIT:
            city = pick_city(clocks_chosen ? clocks[screen.selected] : -1);
            if (city >= 0)
            {
                clocks[screen.selected] = city;
                clocks_save();
            }
            break;
        case BUTTON_DELETE:
            memmove(&clocks[screen.selected], &clocks[screen.selected + 1],
                    (clock_count - screen.selected - 1) * sizeof(clocks[0]));
            clock_count--;
            screen.selected = MIN(screen.selected, clock_count - 1);
            screen.top = MIN(screen.top, MAX(0, clock_count - VISIBLE_ROWS));
            keep_visible();
            clocks_save();
            break;
    }
}

int extras_clock(void)
{
    const struct ui_rect bounds = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int ret = 0, minute = -1;
    bool dirty = true;

    clocks_load();
    /* Not the main menu's: its rows all carry a chevron. */
    push_current_activity(ACTIVITY_TIMEDATESCREEN);
    screen.selected = screen.top = 0;
    screen.dialog = false;
    screen.button = BUTTON_ADD;
    screen.band = core_alloc(LCD_WIDTH * BAND_ROWS * sizeof(uint32_t));
    load_fonts();
    ui_view_init(&view, bounds, &(const struct ui_view_ops){
        .paint = clock_paint }, NULL);
    ui_page_init(&page, &view);
    ui_scene_open_page(&scene, "clock-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);

    while (1)
    {
        int now = get_time()->tm_min;
        if (dirty || now != minute)
        {
            minute = now;
            dirty = false;
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
        }
        int action = get_action(screen.dialog ? CONTEXT_YESNOSCREEN :
                                CONTEXT_STD, HZ / 2);
        if (screen.dialog)
        {
            switch (action)
            {
                case ACTION_STD_PREV:
                case ACTION_STD_PREVREPEAT:
                    screen.button = MAX(0, screen.button - 1);
                    dirty = true;
                    continue;
                case ACTION_STD_NEXT:
                case ACTION_STD_NEXTREPEAT:
                    screen.button = MIN(button_count() - 1, screen.button + 1);
                    dirty = true;
                    continue;
                case ACTION_YESNO_ACCEPT:
                    screen.dialog = false;
                    choose(screen.button);
                    dirty = true;
                    continue;
                case ACTION_STD_CANCEL:
                case ACTION_STD_MENU:
                    screen.dialog = false;
                    dirty = true;
                    continue;
            }
        }
        else
        {
            switch (action)
            {
                case ACTION_STD_PREV:
                case ACTION_STD_PREVREPEAT:
                    if (screen.selected > 0)
                    {
                        screen.selected--;
                        keep_visible();
                        dirty = true;
                    }
                    continue;
                case ACTION_STD_NEXT:
                case ACTION_STD_NEXTREPEAT:
                    if (screen.selected < clock_count - 1)
                    {
                        screen.selected++;
                        keep_visible();
                        dirty = true;
                    }
                    continue;
                case ACTION_STD_OK:
                    screen.dialog = true;
                    screen.button = MIN(screen.button, button_count() - 1);
                    dirty = true;
                    continue;
                case ACTION_STD_CANCEL:
                case ACTION_STD_MENU:
                    break;
                case ACTION_TREE_WPS:
                    ret = GO_TO_WPS;
                    break;
                default:
                    if (default_event_handler(action) == SYS_USB_CONNECTED)
                        ret = GO_TO_ROOT;
                    else
                        continue;
            }
            break;
        }
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "clock-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    unload_fonts();
    pop_current_activity();
    if (screen.band > 0)
        screen.band = core_free(screen.band);
    return ret;
}
