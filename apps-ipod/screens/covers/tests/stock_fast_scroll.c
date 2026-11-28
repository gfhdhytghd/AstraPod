/* Host probe for the iPod Classic cover strip. Compile ultra_flow.c and
 * ui/ultra.c with the simulator's CFLAGS and -ffunction-sections, then link
 * them with this file using -Wl,--gc-sections. */
#include <assert.h>
#include <stdio.h>
#include "../stock_motion.h"

enum { WHEEL_PACKET = 9, PACKET_US = 2000, LCD_WIDTH = 320 };

static bool cover_visible(const struct stock_flow *flow, uint32_t now)
{
    struct stock_visible visible[STOCK_MAX_VISIBLE];
    int count = stock_sample(flow, now, visible);
    for (int i = 0; i < count; i++)
    {
        struct stock_edge edge[2];
        stock_project(visible[i].pose, edge);
        int x = (edge[0].x + 32767) >> 16;
        int y = (edge[1].x + 32767) >> 16;
        int left = x < y ? x : y, right = x < y ? y : x;
        if (left < right && left < LCD_WIDTH && right > 0)
            return true;
    }
    return false;
}

static bool check_strip(const struct stock_flow *flow, uint32_t from, uint32_t to)
{
    unsigned blank = 0;
    for (uint32_t t = from; t <= to; t += 1000)
        if (!cover_visible(flow, t))
            blank++;
    if (blank)
        fprintf(stderr, "%s target %d: %u samples without cover pixels\n",
                flow->spring ? "Ultra" : "Classic", flow->selected, blank);
    return blank == 0;
}

static bool flick(int direction, bool spring)
{
    enum { ALBUMS = 256 };
    struct stock_flow flow = { .spring = spring };
    stock_init(&flow, ALBUMS, direction > 0 ? 0 : ALBUMS - 1, 0, false);
    uint32_t now = PACKET_US;
    for (; now <= 29 * PACKET_US; now += PACKET_US)
        stock_scroll(&flow, direction * WHEEL_PACKET, now);
    assert(flow.selected == (direction > 0 ? ALBUMS - 1 : 0));

    bool landing = check_strip(&flow, now, now + STOCK_SCROLL_US);
    now += 480000;
    if (!cover_visible(&flow, now))
        return false;
    stock_scroll(&flow, -direction * WHEEL_PACKET, now);
    assert(flow.length > 0);
    return check_strip(&flow, now, now + STOCK_SCROLL_US) && landing;
}

int main(void)
{
    bool ok = true;
    for (int direction = -1; direction <= 1; direction += 2)
    {
        ok &= flick(direction, false);
        ok &= flick(direction, true);
    }

    struct stock_flow flow = { .spring = true };
    stock_init(&flow, 10, 4, 0, false);
    stock_scroll(&flow, 1, 0);
    assert(stock_scroll_progress(&flow, 480000) > STOCK_ONE);
    ok &= check_strip(&flow, 0, STOCK_ULTRA_SCROLL_US);
    if (!ok)
        return 1;

    puts("PASS: Classic and Ultra cover pixels through fast flicks, edges and reversals");
    return 0;
}
