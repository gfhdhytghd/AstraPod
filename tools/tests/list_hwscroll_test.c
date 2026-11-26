/* The list's hardware-scroll planner (apps-ipod/widgets/list_hwscroll.c),
 * on the host. A model list is drawn for two poses; the panel is the first
 * drawing moved around the ring by the frame's shift with its bands laid
 * over from the second, and must then equal the second everywhere in the
 * area. From the repository root:
 *   gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *       -I apps-ipod -o /tmp/list_hwscroll_test tools/tests/list_hwscroll_test.c &&
 *   /tmp/list_hwscroll_test
 * GPL version 2 or later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PODBOX_UI_RETAINED_H
struct ui_rect {
    int x, y, width, height;
};
#include "widgets/list_hwscroll.c"

#define W 320
#define H 240
#define COUNT 20

enum style { MARKER, BAR };

struct model {
    struct list_hwscroll_layout layout;
    enum style style;
    bool notes;
};

static unsigned old_fb[H][W], new_fb[H][W], panel[H][W];

/* Pixels that differ between items, rows of an item, columns, and whether
 * the item is in selected text, so a misplaced row cannot pass. */
static unsigned item_pixel(int item, int row, int x, bool lit)
{
    return 1000 + item * 97 + row * 3 + x % 7 + (lit ? 50000 : 0);
}

static void draw(const struct model *m, const struct list_hwscroll_pose *p,
                 unsigned fb[H][W])
{
    const struct list_hwscroll_layout *l = &m->layout;
    const struct ui_rect *c = &l->content;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            unsigned v = 2;                      /* background */
            if (y < l->area_top)
                v = 1;                           /* status bar */
            else if (y < c->y)
                v = 300 + x % 5 + y;             /* header, fixed */
            else if (m->notes && y >= l->notes.y &&
                     y < l->notes.y + l->notes.height)
                v = 400 + p->selected * 13 + x % 3;
            else if (y < c->y + c->height && x >= c->x && x < c->x + c->width)
            {
                int offset = y - c->y + p->top;
                int item = offset >= 0 ? offset / l->pitch :
                           -((-offset + l->pitch - 1) / l->pitch);
                int row = offset - item * l->pitch;
                bool in_bar = m->style == BAR && y >= p->marker.y &&
                              y < p->marker.y + p->marker.height;
                if (item >= 0 && item < COUNT)
                    v = item_pixel(item, row, x,
                                   item >= p->lit && item < p->lit + l->span);
                if (in_bar)
                    v += 70000;
                if (m->style == MARKER && x >= p->marker.x &&
                    x < p->marker.x + p->marker.width && y >= p->marker.y &&
                    y < p->marker.y + p->marker.height)
                    v = 9;
                const struct ui_rect *s = &l->scrollbar;
                if (s->width && x >= s->x && x < s->x + s->width)
                {
                    int range = COUNT * l->pitch;
                    int from = p->top * c->height / range;
                    int to = (p->top + c->height) * c->height / range;
                    v = (y - c->y >= from && y - c->y < to ? 11 : 12) +
                        100 * p->indicator;
                }
            }
            fb[y][x] = v;
        }
}

static struct list_hwscroll_pose pose(const struct model *m, int top,
                                      int marker_y, int lit, int selected)
{
    const struct list_hwscroll_layout *l = &m->layout;
    struct ui_rect marker = { 40, marker_y, 4, l->pitch * l->span };
    if (m->style == BAR)
    {
        marker.x = 0;
        marker.width = W;
    }
    return (struct list_hwscroll_pose){ top, marker, lit, selected, 0 };
}

/* Applies the frame to the panel the way the LCD does and counts the
 * pixels of the area that differ from the new drawing. */
static long check(const struct model *m, const struct list_hwscroll_pose *a,
                  const struct list_hwscroll_pose *b,
                  struct ui_hwscroll_frame *frame)
{
    draw(m, a, old_fb);
    draw(m, b, new_fb);
    if (!list_hwscroll_plan(&m->layout, a, b, frame))
        return -1;
    int top = frame->top, height = H - top;
    assert(top == m->layout.area_top);
    assert(frame->count >= 0 && frame->count <= UI_HWSCROLL_BANDS);
    for (int y = top; y < H; ++y)
    {
        int from = top + ((y - top + frame->shift) % height + height) % height;
        memcpy(panel[y], old_fb[from], sizeof(panel[y]));
    }
    for (int i = 0; i < frame->count; ++i)
    {
        const struct ui_rect *band = &frame->band[i];
        assert(band->width > 0 && band->height > 0);
        assert(band->x >= 0 && band->x + band->width <= W);
        assert(band->y >= top && band->y + band->height <= H);
        assert(i == 0 || frame->band[i - 1].y <= band->y);
        for (int y = band->y; y < band->y + band->height; ++y)
            memcpy(&panel[y][band->x], &new_fb[y][band->x],
                   band->width * sizeof(unsigned));
    }
    long wrong = 0;
    for (int y = top; y < H; ++y)
        for (int x = 0; x < W; ++x)
            wrong += panel[y][x] != new_fb[y][x];
    return wrong;
}

static int band_rows(const struct ui_hwscroll_frame *frame)
{
    long pixels = 0;
    for (int i = 0; i < frame->count; ++i)
        pixels += (long)frame->band[i].width * frame->band[i].height;
    return (pixels + W - 1) / W;
}

/* Scrim's look: status 35, six 30-pixel rows from 46, an 8-pixel
 * scrollbar at the right edge, a thin marker. */
static struct model scrim(bool notes)
{
    struct model m = {
        .layout = {
            .width = W, .height = H, .area_top = 35,
            .content = { 0, 46, W, 180 },
            .scrollbar = { W - 8, 46, 8, 180 },
            .pitch = 30, .span = 1,
        },
        .style = MARKER, .notes = notes,
    };
    if (notes)
    {
        m.layout.content.height = 150;
        m.layout.scrollbar.height = 150;
        m.layout.notes = (struct ui_rect){ 0, 196, W, 44 };
    }
    return m;
}

/* Classic's look: a full-width bar and the overlay scrollbar. */
static struct model classic(void)
{
    struct model m = {
        .layout = {
            .width = W, .height = H, .area_top = 22,
            .content = { 0, 22, W, 216 },
            .scrollbar = { W - 6, 22, 6, 216 },
            .pitch = 27, .span = 1,
        },
        .style = BAR,
    };
    return m;
}

static int sweep(const struct model *m)
{
    const struct list_hwscroll_layout *l = &m->layout;
    int max_top = COUNT * l->pitch - l->content.height;
    int tried = 0, planned = 0;
    for (int a = -l->pitch / 2; a <= max_top + l->pitch / 2; a += 29)
        for (int d = -l->content.height; d <= l->content.height; d += 11)
            for (int move = -40; move <= 40; move += 40)
            {
                int b = a + d;
                int cursor = (a + b) / 2 / l->pitch;
                int lit_a = cursor, lit_b = cursor + (move > 0) - (move < 0);
                struct list_hwscroll_pose pa = pose(m,
                    a, l->content.y + cursor * l->pitch - a, lit_a, lit_a);
                struct list_hwscroll_pose pb = pose(m,
                    b, pa.marker.y + move, lit_b, lit_b);
                struct ui_hwscroll_frame frame;
                long wrong = check(m, &pa, &pb, &frame);
                ++tried;
                if (wrong < 0)
                {
                    /* A small move is refused only beside tall notes. */
                    assert(d > 20 || d < -20 || m->notes);
                    continue;
                }
                ++planned;
                if (wrong)
                    fprintf(stderr, "top %d -> %d, marker %d -> %d: %ld wrong\n",
                            a, b, pa.marker.y, pb.marker.y, wrong);
                assert(wrong == 0);
                assert(band_rows(&frame) <= LIST_HWSCROLL_ROWS);
            }
    assert(planned > tried / 3);
    return planned;
}

int main(void)
{
    struct ui_hwscroll_frame frame;
    struct model m = scrim(false);
    const struct list_hwscroll_layout *l = &m.layout;

    /* A slow scroll step: the rows move 4 pixels and the marker 4 down. */
    struct list_hwscroll_pose a = pose(&m, 60, 46 + 90, 5, 5);
    struct list_hwscroll_pose b = pose(&m, 64, 46 + 94, 5, 5);
    assert(check(&m, &a, &b, &frame) == 0);
    assert(frame.shift == 4 && frame.count <= 5 && band_rows(&frame) <= 40);

    /* Elastic ends: the list pulled above its first row and past its last. */
    a = pose(&m, -6, 46 + 6, 0, 0);
    b = pose(&m, -13, 46 + 13, 0, 0);
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == -7);
    int max_top = COUNT * l->pitch - l->content.height;
    a = pose(&m, max_top + 9, 46 + 150 - 9, 19, 19);
    b = pose(&m, max_top + 15, 46 + 150 - 15, 19, 19);
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == 6);
    a = pose(&m, max_top + 15, 46 + 150 - 15, 19, 19);
    b = pose(&m, max_top + 2, 46 + 150 - 2, 19, 19);
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == -13);

    /* A retarget mid-scroll turns the list round: down 9, then up 5. */
    a = pose(&m, 100, 46 + 70, 6, 6);
    b = pose(&m, 109, 46 + 80, 6, 7);
    assert(check(&m, &a, &b, &frame) == 0);
    a = b;
    b = pose(&m, 104, 46 + 68, 5, 5);
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == -5);

    /* Rows at rest and the marker moving: no shift, only the marker path
     * and, when it changes, the selected rows. */
    a = pose(&m, 60, 46 + 30, 3, 3);
    b = pose(&m, 60, 46 + 41, 3, 3);
    assert(check(&m, &a, &b, &frame) == 0);
    assert(frame.shift == 0 && frame.count == 1 && frame.band[0].width == 4);
    a = b;
    b = pose(&m, 60, 46 + 47, 4, 4);
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == 0);
    assert(band_rows(&frame) <= 2 * l->pitch + 1);
    a = pose(&m, 60, 46 + 30, 3, 3);
    assert(check(&m, &a, &a, &frame) == 0 && frame.count == 0);

    /* A fading scrollbar over rows at rest sends its column alone. */
    b = a;
    b.indicator = 13;
    a.indicator = 14;
    assert(check(&m, &a, &b, &frame) == 0 && frame.shift == 0);
    assert(frame.count == 1 && frame.band[0].x == l->scrollbar.x &&
           frame.band[0].width == l->scrollbar.width);

    /* Notes follow the selection even when the rows stand still. */
    struct model noted = scrim(true);
    a = pose(&noted, 30, 46 + 30, 2, 2);
    b = pose(&noted, 30, 46 + 60, 3, 3);
    assert(check(&noted, &a, &b, &frame) == 0 && frame.shift == 0);

    /* Large jumps are refused: the whole content height, and more rows
     * than one refresh's gap holds. */
    a = pose(&m, 0, 46, 0, 0);
    b = pose(&m, l->content.height, 46, 6, 6);
    assert(check(&m, &a, &b, &frame) < 0);
    b = pose(&m, 120, 46, 4, 4);
    assert(check(&m, &a, &b, &frame) < 0);

    /* Classic's one-row jump at the bottom of the page: the bar stays on
     * its screen row and the list moves one row up. */
    struct model c = classic();
    int pitch = c.layout.pitch;
    a = pose(&c, 2 * pitch, 22 + 7 * pitch, 9, 9);
    b = pose(&c, 3 * pitch, 22 + 7 * pitch, 10, 10);
    assert(check(&c, &a, &b, &frame) == 0);
    assert(frame.shift == pitch && frame.count <= 3);
    assert(band_rows(&frame) <= 3 * pitch + 2);
    /* And inside the page: the bar moves, the rows do not. */
    a = pose(&c, 2 * pitch, 22 + 3 * pitch, 5, 5);
    b = pose(&c, 2 * pitch, 22 + 4 * pitch, 6, 6);
    assert(check(&c, &a, &b, &frame) == 0);
    assert(frame.shift == 0 && band_rows(&frame) == 2 * pitch);

    int planned = sweep(&m) + sweep(&noted) + sweep(&c);
    printf("list_hwscroll: PASS, %d swept frames exact\n", planned);
    return 0;
}
