/* A list frame as a panel hardware-scroll frame. Pure, so
 * tools/tests/list_hwscroll_test.c builds it on the host. GPL version 2+.
 *
 * The panel moves the whole area by the change in the list's top. Every
 * pixel that is not then the last frame's pixel that much further down
 * goes in a band: the parts of the area outside the rows, which must not
 * move; the rows the move exposes; the marker or bar where it was, carried
 * along, and where it is now; the rows whose text colour follows the
 * selection; and the scrollbar, whenever it moves or fades. The list's background is one colour along
 * the scroll axis, so the rows' surroundings move unchanged. */
#include <string.h>
#include "list_hwscroll.h"

/* Bands before merging: two outside the rows, the exposed rows, the
 * scrollbar or notes, two markers and two selected rows. */
#define RAW_BANDS 12

struct bands {
    struct ui_rect band[RAW_BANDS];
    int count;
};

static int lower(int a, int b) { return a < b ? a : b; }
static int upper(int a, int b) { return a > b ? a : b; }

static struct ui_rect clip(struct ui_rect r, struct ui_rect bound)
{
    int left = upper(r.x, bound.x), top = upper(r.y, bound.y);
    int right = lower(r.x + r.width, bound.x + bound.width);
    int bottom = lower(r.y + r.height, bound.y + bound.height);
    return (struct ui_rect){ left, top, right - left, bottom - top };
}

static bool contains(struct ui_rect a, struct ui_rect b)
{
    return a.x <= b.x && a.y <= b.y && a.x + a.width >= b.x + b.width &&
           a.y + a.height >= b.y + b.height;
}

static void add(struct bands *set, struct ui_rect r, struct ui_rect area)
{
    r = clip(r, area);
    if (r.width > 0 && r.height > 0 && set->count < RAW_BANDS)
        set->band[set->count++] = r;
}

static void drop(struct bands *set, int i)
{
    set->band[i] = set->band[--set->count];
}

/* Drops bands inside another and joins those of one column that touch. */
static void merge(struct bands *set)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (int i = 0; i < set->count && !changed; ++i)
            for (int j = i + 1; j < set->count && !changed; ++j)
            {
                struct ui_rect *a = &set->band[i], *b = &set->band[j];
                changed = true;
                if (contains(*a, *b))
                    drop(set, j);
                else if (contains(*b, *a))
                    drop(set, i);
                else if (a->x == b->x && a->width == b->width &&
                         a->y <= b->y + b->height && b->y <= a->y + a->height)
                {
                    int top = lower(a->y, b->y);
                    a->height = upper(a->y + a->height, b->y + b->height) - top;
                    a->y = top;
                    drop(set, j);
                }
                else
                    changed = false;
            }
    }
}

bool list_hwscroll_plan(const struct list_hwscroll_layout *layout,
                        const struct list_hwscroll_pose *shown,
                        const struct list_hwscroll_pose *next,
                        struct ui_hwscroll_frame *frame)
{
    const struct ui_rect *content = &layout->content;
    struct ui_rect area = { 0, layout->area_top, layout->width,
                            layout->height - layout->area_top };
    int shift = next->top - shown->top;
    int bottom = content->y + content->height;
    if (content->height <= 0 || shift >= content->height ||
        -shift >= content->height)
        return false;

    struct bands set = { .count = 0 };
    struct ui_rect rows = { 0, 0, layout->width, 0 };
    if (shift)
    {
        rows.y = area.y;
        rows.height = content->y - area.y;
        add(&set, rows, area);
        rows.y = bottom;
        rows.height = layout->height - bottom;
        add(&set, rows, area);
        rows.y = shift > 0 ? bottom - shift : content->y;
        rows.height = shift > 0 ? shift : -shift;
        add(&set, rows, area);
    }
    else if (layout->notes.height > 0 && shown->selected != next->selected)
        add(&set, layout->notes, area);
    if (shift || shown->indicator != next->indicator)
        add(&set, clip(layout->scrollbar, *content), area);

    struct ui_rect marker = shown->marker;
    if (shift || memcmp(&marker, &next->marker, sizeof(marker)))
    {
        marker.y -= shift;
        add(&set, clip(marker, *content), area);
        add(&set, clip(next->marker, *content), area);
    }
    if (shown->lit != next->lit)
    {
        rows.height = layout->span * layout->pitch;
        rows.y = content->y - next->top + shown->lit * layout->pitch;
        add(&set, clip(rows, *content), area);
        rows.y = content->y - next->top + next->lit * layout->pitch;
        add(&set, clip(rows, *content), area);
    }

    merge(&set);
    if (set.count > UI_HWSCROLL_BANDS)
        return false;
    long pixels = 0;
    for (int i = 0; i < set.count; ++i)
        pixels += (long)set.band[i].width * set.band[i].height;
    if ((pixels + layout->width - 1) / layout->width > LIST_HWSCROLL_ROWS)
        return false;

    /* In order of first row, which is the order the panel's scan reads. */
    for (int i = 1; i < set.count; ++i)
        for (int j = i; j > 0 && set.band[j].y < set.band[j - 1].y; --j)
        {
            struct ui_rect swap = set.band[j];
            set.band[j] = set.band[j - 1];
            set.band[j - 1] = swap;
        }
    frame->top = layout->area_top;
    frame->shift = shift;
    frame->count = set.count;
    for (int i = 0; i < set.count; ++i)
        frame->band[i] = set.band[i];
    return true;
}
