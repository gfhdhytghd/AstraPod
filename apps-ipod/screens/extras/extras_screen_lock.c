/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Screen Lock, as stock draws it. With no combination it asks for
 * a new one on the four-digit tumbler and for it again to confirm. With one
 * it offers Lock and Reset: Lock shows the padlock and swallows every button
 * until the combination is entered, and leaves for the main menu; Reset asks
 * for the old combination, then a new one. The combination is kept in
 * SL_FILE as four ASCII digits.
 *
 * Parts: the combination file, the page, the tumbler, then the screen.
 ****************************************************************************/
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "file.h"
#include "font.h"
#include "lcd.h"
#include "lang.h"
#include "rbpaths.h"
#include "input/action.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "ui/stock_art.h"
#include "root_menu.h"
#include "extras_bar.h"
#include "extras.h"
#include "ui/scene.h"

#define SL_FILE        ROCKBOX_DIR "/screen_lock.dat"
#define SL_DIGITS      4

#define DIGIT_FONT     FONT_DIR "/29-helvetica-bold.pbf"
#define CAPTION_FONT   FONT_DIR "/17-helvetica-bold.pbf"
#define LABEL_FONT     FONT_DIR "/13-helvetica-bold.pbf"
#define WHITE          0xffffff
#define BAND_ROWS      EXTRAS_BAND_ROWS

#define PADLOCK_X      19
#define PADLOCK_Y      22
#define LOCKED_X       104             /* the padlock alone, while locked   */
#define TUMBLER_X      128
#define TUMBLER_Y      106
#define FOCUS_X        145             /* over the first digit              */
#define DIGIT_X        150             /* the first digit's pen             */
#define DIGIT_PITCH    40
#define DIGIT_BASELINE 140
#define CAPTION_BASELINE 217

/* Lock and Reset sit in a well inside a panel; each segment is its label
 * and SEGMENT_PAD wide, and the panel the well and PANEL_PAD each side. */
#define PANEL_CENTRE   223
#define PANEL_Y        106
#define PANEL_PAD      16
#define SEGMENT_PAD    32
#define WELL_Y         117
#define CAP_W          6
#define LABEL_BASELINE 134

/* ---- the combination file ---- */

static bool sl_load(char code[SL_DIGITS])
{
    int fd = open(SL_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, code, SL_DIGITS) == SL_DIGITS;
    close(fd);
    for (int i = 0; ok && i < SL_DIGITS; i++)
        ok = code[i] >= '0' && code[i] <= '9';
    return ok;
}

static void sl_save(const char code[SL_DIGITS])
{
    int fd = open(SL_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, code, SL_DIGITS);
    close(fd);
}

bool extras_screen_lock_set(void)
{
    char code[SL_DIGITS];
    return sl_load(code);
}

/* ---- the page ---- */

enum sl_view { SL_TUMBLER, SL_CHOICE, SL_LOCKED };

static struct {
    enum sl_view view;
    int caption;                     /* a LANG_ id                          */
    char digit[SL_DIGITS];
    int focus;                       /* the digit being set, or -1          */
    int button;                      /* 0 Lock, 1 Reset                     */
    int digit_font, caption_font, label_font;
} sl;

/* The Lock and Reset panel is none of stock's bitmaps, so it is sampled from
 * stock's own screen, rows from y PANEL_Y: CAP_W columns each side as stock
 * composed them over Background_LightBlue_Image at this spot, and between
 * them one column, stretched, of colour and alpha over the backdrop. */
static const uint32_t panel[57][2 * CAP_W + 1] = {
    { 0x02000000, 0x02000000, 0x03000000, 0x04000000, 0x04000000, 0x05000000,
      0x05000000, 0x05000000, 0x04000000, 0x04000000, 0x03000000, 0x02000000,
      0x02000000 },
    { 0xff7b8995, 0xff7b8996, 0xff798693, 0xff687680, 0xff4e5860, 0xff3d444a,
      0x81000000, 0xff343c41, 0xff434b54, 0xff596671, 0xff667580, 0xff697783,
      0xff697781 },
    { 0xff7a8996, 0xff72808b, 0xff566169, 0xff7f8387, 0xffabaeb0, 0xffb5b7b8,
      0xffb5b7b8, 0xffb3b5b6, 0xffa8aaad, 0xff797e81, 0xff49535c, 0xff61707b,
      0xff687681 },
    { 0xff73818e, 0xff535e64, 0xffa4a7a9, 0xffa6a8a9, 0xff929496, 0xff808384,
      0xdf7f8080, 0xff7f8082, 0xff8f9193, 0xffa4a5a7, 0xffa0a3a6, 0xff475058,
      0xff626e7b },
    { 0xff5d6d75, 0xff9ca0a3, 0xff8e9192, 0xff7c7f80, 0xff787a7c, 0xff787a7c,
      0xd9777877, 0xff757779, 0xff757879, 0xff7a7c7d, 0xff8c8e8f, 0xff989b9e,
      0xff515c65 },
    { 0xff555e64, 0xff919396, 0xff77797b, 0xff717375, 0xff6f7274, 0xff6f7274,
      0xdb6f6f6f, 0xff6d6f70, 0xff6d6f70, 0xff6e7172, 0xff747778, 0xff8e9092,
      0xff4e545a },
    { 0xff51595f, 0xff838688, 0xff696b6e, 0xff676a6c, 0xff676a6c, 0xff67696c,
      0xdb646565, 0xff656769, 0xff656769, 0xff656769, 0xff67696b, 0xff818385,
      0xff4a5056 },
    { 0xff424b51, 0xff7f8183, 0xff616465, 0xff616465, 0xff606365, 0xff606365,
      0xda5b5b5b, 0xff5e6062, 0xff5e6062, 0xff5f6163, 0xff5f6163, 0xff7d7f80,
      0xff3b4248 },
    { 0xff3e464c, 0xff787b7c, 0xff595b5d, 0xff5a5c5e, 0xff585a5c, 0xff585a5c,
      0xd7515151, 0xff555759, 0xff555759, 0xff57595b, 0xff56585a, 0xff76787a,
      0xff363c42 },
    { 0xff384047, 0xff737577, 0xff525456, 0xff515456, 0xff525456, 0xff525556,
      0xd94a4a4a, 0xff505254, 0xff4f5153, 0xff4f5153, 0xff505153, 0xff717374,
      0xff31373d },
    { 0xff383f47, 0xff707274, 0xff4f5153, 0xff4e5052, 0xff4e5052, 0xff4e5052,
      0xda474747, 0xff4c4d4f, 0xff4c4e4f, 0xff4c4d4f, 0xff4d4e50, 0xff6e7071,
      0xff31363c },
    { 0xff373f45, 0xff6e7172, 0xff4b4c4e, 0xff4a4d4e, 0xff4a4d4e, 0xff4a4c4e,
      0xdc424242, 0xff484a4b, 0xff484a4c, 0xff484a4c, 0xff484a4c, 0xff6d6e70,
      0xff30363c },
    { 0xff373e45, 0xff6d6e70, 0xff46484a, 0xff46484a, 0xff46494a, 0xff454749,
      0xe23f3f40, 0xff434547, 0xff444648, 0xff444547, 0xff444547, 0xff6b6c6e,
      0xff30353c },
    { 0xff363d43, 0xff6b6c6e, 0xff444547, 0xff424445, 0xff424446, 0xff424446,
      0xff414345, 0xff414243, 0xff404244, 0xff404243, 0xff424345, 0xff696a6c,
      0xff2f353b },
    { 0xff363d43, 0xff686a6b, 0xff3f4143, 0xff3f4143, 0xff3f4142, 0xff3f4042,
      0xff3e4142, 0xff3d3e40, 0xff3d3e40, 0xff3d3f40, 0xff3d3f40, 0xff666869,
      0xff2f353b },
    { 0xff353c42, 0xff656768, 0xff3a3d3e, 0xff3b3d3f, 0xff3b3d3f, 0xff3b3d3f,
      0xe0333333, 0xff393b3d, 0xff393b3c, 0xff393b3c, 0xff393a3c, 0xff646566,
      0xff2e3439 },
    { 0xff343b42, 0xff646667, 0xff37393b, 0xff37393b, 0xff37393b, 0xff383a3b,
      0xff37393a, 0xff363839, 0xff363739, 0xff363739, 0xff363739, 0xff636465,
      0xff2e3439 },
    { 0xff343c42, 0xff626465, 0xff353738, 0xff353739, 0xff363839, 0xff353738,
      0xff353738, 0xff333536, 0xff343637, 0xff343536, 0xff343536, 0xff616263,
      0xff2d3439 },
    { 0xff343c41, 0xff5f6162, 0xff333436, 0xff323436, 0xff323436, 0xff323435,
      0xff323435, 0xff313233, 0xff313234, 0xff313234, 0xff313234, 0xff5e5f60,
      0xff2d3238 },
    { 0xff323a40, 0xff5d5f60, 0xff2f3032, 0xff2f3133, 0xff2f3133, 0xff2f3032,
      0xff2d2f30, 0xff2d2e30, 0xff2e2f31, 0xff2e2f30, 0xff2d2f30, 0xff5c5d5e,
      0xff2d3238 },
    { 0xff343940, 0xff5b5d5e, 0xff2c2e30, 0xff2d2e30, 0xff2c2e30, 0xff2c2e2f,
      0xff2c2e2f, 0xff2b2c2d, 0xff2b2d2e, 0xff2b2c2e, 0xff2b2d2e, 0xff5a5b5c,
      0xff2c3238 },
    { 0xff333940, 0xff5a5b5d, 0xff2a2c2d, 0xff2a2b2d, 0xff2a2c2d, 0xff2a2c2d,
      0xe3222221, 0xff292a2b, 0xff292a2b, 0xff282a2b, 0xff292a2b, 0xff595a5b,
      0xff2c3237 },
    { 0xff33393f, 0xff595b5c, 0xff28292a, 0xff272829, 0xff28292a, 0xff282a2b,
      0xe21e1e1e, 0xff272829, 0xff262828, 0xff252628, 0xff262729, 0xff58595a,
      0xff2c3137 },
    { 0xff33393f, 0xff58595b, 0xff262729, 0xff262729, 0xff262829, 0xff262729,
      0xe61e1e1e, 0xff252627, 0xff252627, 0xff252627, 0xff252627, 0xff575859,
      0xff2c3137 },
    { 0xff33383f, 0xff454647, 0xff0b0c0e, 0xff0b0d0e, 0xff0b0c0e, 0xff0b0c0e,
      0xe5000000, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff444546,
      0xff2d3236 },
    { 0xff33383e, 0xff454647, 0xff0a0c0d, 0xff0a0c0d, 0xff0a0c0d, 0xff0a0c0d,
      0xff0a0c0d, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2c3236 },
    { 0xff32383e, 0xff454647, 0xff0a0b0d, 0xff0a0b0d, 0xff0a0b0d, 0xff0a0b0d,
      0xe3000000, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2b3236 },
    { 0xff32373e, 0xff454647, 0xff0a0b0d, 0xff0a0c0d, 0xff0a0b0d, 0xff0a0b0d,
      0xff0a0b0d, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2c3136 },
    { 0xff31373d, 0xff454647, 0xff0a0b0d, 0xff0a0b0d, 0xff0a0b0d, 0xff0a0b0d,
      0xff0a0b0d, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2b3135 },
    { 0xff31363d, 0xff454547, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0d, 0xff0a0b0c,
      0xff0a0b0d, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444445,
      0xff2b3035 },
    { 0xff30363c, 0xff454546, 0xff0a0b0c, 0xff0a0b0d, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2a3035 },
    { 0xff30353c, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0d,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444445,
      0xff2b3035 },
    { 0xff2f363b, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444445,
      0xff2a3034 },
    { 0xff2f363b, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff434445,
      0xff2a3034 },
    { 0xff2f363b, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff090a0b, 0xff444545,
      0xff2a3034 },
    { 0xff2f353a, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff090a0b, 0xff080a0b, 0xff090a0b, 0xff444445,
      0xff2a3034 },
    { 0xff2f353a, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff080a0b, 0xff090a0b, 0xff090a0b, 0xff434445,
      0xff2a3033 },
    { 0xff2f353a, 0xff444546, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c, 0xff0a0b0c,
      0xff0a0b0c, 0xff090a0b, 0xff080a0b, 0xff080a0b, 0xff080a0b, 0xff434445,
      0xff292f34 },
    { 0xff2f3439, 0xff444546, 0xff090b0c, 0xff090b0c, 0xff090b0c, 0xff090b0c,
      0xff090b0c, 0xff080a0b, 0xff080a0b, 0xff080a0b, 0xff080a0b, 0xff434445,
      0xff292f33 },
    { 0xff2e3539, 0xff444546, 0xff090b0c, 0xff090b0c, 0xff090b0c, 0xff090b0c,
      0xff090b0c, 0xff080a0a, 0xff08090b, 0xff08090a, 0xff080a0a, 0xff434445,
      0xff292f33 },
    { 0xff2d3439, 0xff444546, 0xff090b0c, 0xff090b0c, 0xff090b0c, 0xff090b0c,
      0xff090b0c, 0xff08090a, 0xff08090a, 0xff08090a, 0xff08090a, 0xff434445,
      0xff282f33 },
    { 0xff2e3539, 0xff444546, 0xff090b0c, 0xff090b0c, 0xff090b0c, 0xff090b0c,
      0xff090b0c, 0xff08090a, 0xff08090a, 0xff08090a, 0xff08090a, 0xff434445,
      0xff292f33 },
    { 0xff2e3438, 0xff4c4d4e, 0xff0f1011, 0xff090b0c, 0xff090b0c, 0xff090b0c,
      0xff090b0c, 0xff08090a, 0xff08090a, 0xff08090a, 0xff0e0f10, 0xff4c4d4d,
      0xff292e32 },
    { 0xff31383c, 0xff5c5f60, 0xff222324, 0xff0a0b0c, 0xff090b0b, 0xff090b0c,
      0xff090b0c, 0xff08090a, 0xff08090a, 0xff090a0b, 0xff212223, 0xff5a5c5e,
      0xff2c3136 },
    { 0xff4a565d, 0xff363c40, 0xff545556, 0xff1c1e1e, 0xff0c0d0e, 0xff090b0b,
      0xff090a0b, 0xff08090a, 0xff0b0c0d, 0xff1c1d1d, 0xff535455, 0xff33383b,
      0xff454d54 },
    { 0xff57646c, 0xff3e474d, 0xff3f4448, 0xff57595a, 0xff333435, 0xff1e1f20,
      0xff151617, 0xff1d1e1f, 0xff323334, 0xff565858, 0xff3c4143, 0xff394246,
      0xff525961 },
    { 0xff5a666f, 0xff556068, 0xff414a50, 0xff2b3235, 0xff525558, 0xff555759,
      0xe5515151, 0xff545657, 0xff505255, 0xff282e31, 0xff3b4449, 0xff4d575d,
      0xff525e65 },
    { 0x10000000, 0x17000000, 0x1f000000, 0x39000000, 0x6b000000, 0x97000000,
      0xa1010201, 0x97000000, 0x6b000000, 0x39000000, 0x1f000000, 0x18000000,
      0x11000000 },
    { 0x10000000, 0x15000000, 0x1c000000, 0x20000000, 0x29000000, 0x31000000,
      0x3a030403, 0x31000000, 0x28000000, 0x21000000, 0x1b000000, 0x15000000,
      0x10000000 },
    { 0x0d000000, 0x11000000, 0x18000000, 0x1c000000, 0x24000000, 0x2a000000,
      0x2f000000, 0x2a000000, 0x24000000, 0x1d000000, 0x18000000, 0x11000000,
      0x0e000000 },
    { 0x0b000000, 0x0f000000, 0x13000000, 0x18000000, 0x1e000000, 0x23000000,
      0x2c040506, 0x24000000, 0x1f000000, 0x17000000, 0x13000000, 0x0f000000,
      0x0b000000 },
    { 0x09000000, 0x0d000000, 0x0f000000, 0x13000000, 0x18000000, 0x1d000000,
      0x1b000000, 0x1d000000, 0x18000000, 0x13000000, 0x0f000000, 0x0c000000,
      0x0a000000 },
    { 0x07000000, 0x09000000, 0x0c000000, 0x0f000000, 0x13000000, 0x18000000,
      0x1a000000, 0x18000000, 0x12000000, 0x0f000000, 0x0c000000, 0x09000000,
      0x08000000 },
    { 0x05000000, 0x07000000, 0x09000000, 0x0c000000, 0x0e000000, 0x11000000,
      0x17000000, 0x12000000, 0x0e000000, 0x0c000000, 0x09000000, 0x08000000,
      0x05000000 },
    { 0x05000000, 0x05000000, 0x07000000, 0x09000000, 0x0b000000, 0x0d000000,
      0x10010406, 0x0d000000, 0x0a000000, 0x09000000, 0x07000000, 0x05000000,
      0x05000000 },
    { 0x02000000, 0x05000000, 0x05000000, 0x06000000, 0x08000000, 0x09000000,
      0x0f1b2021, 0x0a000000, 0x08000000, 0x06000000, 0x05000000, 0x04000000,
      0x03000000 },
    { 0x02000000, 0x02000000, 0x02000000, 0x05000000, 0x05000000, 0x07000000,
      0x06000000, 0x06000000, 0x05000000, 0x04000000, 0x03000000, 0x03000000,
      0x03000000 },
};

static void put_panel(struct stock_canvas *c, int x, int width)
{
    int y0 = MAX(c->y, PANEL_Y);
    int y1 = MIN(c->y + c->height, PANEL_Y + (int)ARRAYLEN(panel));
    for (int y = y0; y < y1; y++)
        for (int u = 0; u < width; u++)
        {
            int col = u < CAP_W ? u : u >= width - CAP_W ?
                      u - (width - CAP_W) + CAP_W + 1 : CAP_W;
            uint32_t p = panel[y - PANEL_Y][col];
            stock_canvas_blend(c, x + u, y, p & 0xffffff, p >> 24);
        }
}

/* A left cap, a stretched centre column and a right cap, `width` wide. */
static void put_bar(struct stock_canvas *c, enum stock_art_id left,
                    enum stock_art_id centre, enum stock_art_id right, int x,
                    int width)
{
    stock_art_put(c, left, x, WELL_Y);
    for (int u = CAP_W; u < width - CAP_W; u++)
        stock_art_put(c, centre, x + u, WELL_Y);
    stock_art_put(c, right, x + width - CAP_W, WELL_Y);
}

static int text_width(const char *text, int font)
{
    int width = 0;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    return width;
}

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int baseline)
{
    if (font >= 0)
        stock_canvas_text(c, font, text, x, baseline - font_get(font)->ascent,
                          WHITE);
}

static void put_choice(struct stock_canvas *c)
{
    const char *label[2] = { str(LANG_EXTRAS_SCREEN_LOCK_LOCK),
                             str(LANG_EXTRAS_SCREEN_LOCK_RESET) };
    int seg[2], x;
    for (int i = 0; i < 2; i++)
        seg[i] = text_width(label[i], sl.label_font) + SEGMENT_PAD;
    int well = seg[0] + seg[1];
    int panel_w = well + 2 * PANEL_PAD;
    x = PANEL_CENTRE - panel_w / 2;
    put_panel(c, x, panel_w);
    x += PANEL_PAD;
    put_bar(c, STOCK_ART_OPTIONBAR_BLACK_WELL_LEFTCAP,
            STOCK_ART_OPTIONBAR_BLACK_WELL_CENTER,
            STOCK_ART_OPTIONBAR_BLACK_WELL_RIGHTCAP, x, well);
    put_bar(c, STOCK_ART_OPTIONBAR_BLACK_THUMB_LEFTCAP,
            STOCK_ART_OPTIONBAR_BLACK_THUMB_CENTER,
            STOCK_ART_OPTIONBAR_BLACK_THUMB_RIGHTCAP,
            sl.button ? x + seg[0] : x, seg[sl.button]);
    for (int i = 0; i < 2; i++)
    {
        put_text(c, sl.label_font, label[i],
                 x + (seg[i] - text_width(label[i], sl.label_font)) / 2,
                 LABEL_BASELINE);
        x += seg[i];
    }
}

static void put_tumbler(struct stock_canvas *c)
{
    stock_art_put(c, STOCK_ART_LOCKSCREEN_TUMBLERBGND, TUMBLER_X, TUMBLER_Y);
    if (sl.focus >= 0)
        stock_art_put(c, STOCK_ART_LOCKSCREEN_TUMBLERFOCUS,
                      FOCUS_X + sl.focus * DIGIT_PITCH, TUMBLER_Y);
    for (int i = 0; i < SL_DIGITS; i++)
    {
        char d[2] = { sl.digit[i], '\0' };
        put_text(c, sl.digit_font, d, DIGIT_X + i * DIGIT_PITCH,
                 DIGIT_BASELINE);
    }
    const char *caption = str(sl.caption);
    put_text(c, sl.caption_font, caption,
             (LCD_WIDTH - text_width(caption, sl.caption_font) + 1) / 2,
             CAPTION_BASELINE);
}

static void compose(struct stock_canvas *c)
{
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    stock_art_put(c, STOCK_ART_LOCKSCREEN_PADLOCK,
                  sl.view == SL_LOCKED ? LOCKED_X : PADLOCK_X, PADLOCK_Y);
    if (sl.view == SL_CHOICE)
        put_choice(c);
    else if (sl.view == SL_TUMBLER)
        put_tumbler(c);
    extras_bar_put(c, sl.view == SL_LOCKED ? str(LANG_ROCKBOX_TITLE) :
                                             str(LANG_EXTRAS_SCREEN_LOCK));
}

static void paint(void)
{
    uint32_t *band = extras_band(0);
    if (!band)
        return;
    const struct ui_rect all = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS)
    {
        struct stock_canvas c = { band, 0, y, LCD_WIDTH, BAND_ROWS };
        compose(&c);
        stock_canvas_show(&c, all);
    }
    lcd_update();
}

/* ---- the tumbler ---- */

enum sl_result { SL_DONE, SL_MENU, SL_USB };

/* The next action, or -1 when USB has come and gone. */
static int next_action(int timeout)
{
    int action = get_action(CONTEXT_SETTINGS, timeout);
    if (default_event_handler(action) == SYS_USB_CONNECTED)
        return -1;
    return action;
}

/* Four digits from the wheel under `caption`, into sl.digit. Select and
 * Next move to the next digit, Previous back; Menu gives up. */
static enum sl_result sl_enter(int caption)
{
    sl.view = SL_TUMBLER;
    sl.caption = caption;
    sl.focus = 0;
    memset(sl.digit, '0', SL_DIGITS);
    paint();
    while (1)
    {
        int action = next_action(TIMEOUT_BLOCK);
        char *d = &sl.digit[sl.focus];
        switch (action)
        {
            case -1:
                return SL_USB;
            case ACTION_STD_CANCEL:
                return SL_MENU;
            case ACTION_SETTINGS_INC:
            case ACTION_SETTINGS_INCREPEAT:
                *d = *d == '9' ? '0' : *d + 1;
                break;
            case ACTION_SETTINGS_DEC:
            case ACTION_SETTINGS_DECREPEAT:
                *d = *d == '0' ? '9' : *d - 1;
                break;
            case ACTION_STD_OK:
            case ACTION_STD_NEXT:
                if (++sl.focus == SL_DIGITS)
                    return SL_DONE;
                break;
            case ACTION_STD_PREV:
                if (sl.focus > 0)
                    sl.focus--;
                break;
            default:
                continue;
        }
        paint();
    }
}

/* "Incorrect Combination" over the digits entered, for `ticks`. */
static enum sl_result sl_incorrect(long ticks)
{
    sl.caption = LANG_EXTRAS_SCREEN_LOCK_INCORRECT;
    sl.focus = -1;
    paint();
    long until = current_tick + ticks;
    while (TIME_BEFORE(current_tick, until))
        if (next_action(until - current_tick) == -1)
            return SL_USB;
    return SL_DONE;
}

/* A new combination, entered twice, into `code`. */
static enum sl_result sl_choose(char code[SL_DIGITS])
{
    while (1)
    {
        enum sl_result r = sl_enter(LANG_EXTRAS_SCREEN_LOCK_NEW);
        if (r != SL_DONE)
            return r;
        memcpy(code, sl.digit, SL_DIGITS);
        r = sl_enter(LANG_EXTRAS_SCREEN_LOCK_CONFIRM);
        if (r != SL_DONE)
            return r;
        if (!memcmp(code, sl.digit, SL_DIGITS))
            break;
        memset(sl.digit, '0', SL_DIGITS);
        if (sl_incorrect(3 * HZ) == SL_USB)
            return SL_USB;
    }
    sl_save(code);
    return SL_DONE;
}

/* `code` entered once; SL_MENU after a wrong one, shown for a second. */
static enum sl_result sl_check(const char code[SL_DIGITS])
{
    enum sl_result r = sl_enter(LANG_EXTRAS_SCREEN_LOCK_ENTER);
    if (r != SL_DONE || !memcmp(code, sl.digit, SL_DIGITS))
        return r;
    return sl_incorrect(HZ) == SL_USB ? SL_USB : SL_MENU;
}

/* ---- the screen ---- */

/* Every button but Menu asks for the combination; Menu and Play do
 * nothing. USB is served and the lock stays. */
static void sl_locked(const char code[SL_DIGITS])
{
    while (1)
    {
        sl.view = SL_LOCKED;
        paint();
        int action;
        do
            action = next_action(TIMEOUT_BLOCK);
        while (action != ACTION_STD_OK && action != ACTION_STD_NEXT &&
               action != ACTION_STD_PREV && action != ACTION_SETTINGS_INC &&
               action != ACTION_SETTINGS_DEC && action != -1);
        if (action != -1 && sl_check(code) == SL_DONE)
            return;
    }
}

/* Lock or Reset, Next and Previous moving between them. */
static enum sl_result sl_choice(char code[SL_DIGITS], bool *locked)
{
    sl.button = 0;
    while (1)
    {
        sl.view = SL_CHOICE;
        paint();
        int action = next_action(TIMEOUT_BLOCK);
        if (action == -1)
            return SL_USB;
        if (action == ACTION_STD_CANCEL)
            return SL_MENU;
        if (action == ACTION_STD_NEXT || action == ACTION_STD_PREV)
            sl.button = action == ACTION_STD_NEXT;
        else if (action == ACTION_STD_OK && sl.button == 0)
        {
            sl_locked(code);
            *locked = true;
            return SL_DONE;
        }
        else if (action == ACTION_STD_OK)
        {
            enum sl_result r = sl_check(code);
            if (r == SL_DONE)
                r = sl_choose(code);
            if (r == SL_USB)
                return r;
        }
    }
}

static void unload(int font)
{
    if (font >= 0)
        font_unload(font);
}

int extras_screen_lock(void)
{
    char code[SL_DIGITS];
    bool locked = false;
    enum sl_result r = SL_DONE;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "screen-lock-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    sl.digit_font = font_load_ex(DIGIT_FONT, 0, 10);
    sl.caption_font = font_load_ex(CAPTION_FONT, 0, 32);
    sl.label_font = font_load_ex(LABEL_FONT, 0, 16);

    if (!sl_load(code))
        r = sl_choose(code);
    if (r == SL_DONE)
        r = sl_choice(code, &locked);

    ui_scene_close_modal(&scene, r != SL_USB, "screen-lock-pop");
    unload(sl.digit_font);
    unload(sl.caption_font);
    unload(sl.label_font);
    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return r == SL_USB || locked ? GO_TO_ROOT : 0;
}
