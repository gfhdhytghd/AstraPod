/* PodBox retained UI. GPL version 2+. */
#include "config.h"
#include "core_alloc.h"
#include "lcd.h"
#include "frame.h"
#include "system.h"
#include "retained.h"
#include "native_theme_runtime.h"
#include <string.h>

static const struct ui_rect screen = { 0, 0, LCD_WIDTH, LCD_HEIGHT };

static bool empty(struct ui_rect r)
{
    return r.width <= 0 || r.height <= 0;
}

static struct ui_rect intersect(struct ui_rect a, struct ui_rect b)
{
    int x = MAX(a.x, b.x);
    int y = MAX(a.y, b.y);
    return (struct ui_rect){ x, y, MIN(a.x + a.width, b.x + b.width) - x,
                                  MIN(a.y + a.height, b.y + b.height) - y };
}

static struct ui_rect unite(struct ui_rect a, struct ui_rect b)
{
    if (empty(a))
        return b;
    if (empty(b))
        return a;
    int x = MIN(a.x, b.x);
    int y = MIN(a.y, b.y);
    return (struct ui_rect){ x, y, MAX(a.x + a.width, b.x + b.width) - x,
                                  MAX(a.y + a.height, b.y + b.height) - y };
}

static struct ui_view_frame screen_frame(void)
{
    return (struct ui_view_frame){ .bounds = screen, .clip = screen,
        .opacity = UI_FULL_OPACITY, .brightness = UI_FULL_OPACITY };
}

static struct ui_view_frame resolve(const struct ui_view *view,
                                    const struct ui_view_frame *parent)
{
    const int *p = view->value;
    struct ui_view_frame frame = {
        .bounds = { parent->bounds.x + p[UI_X] + p[UI_OFFSET_X],
                    parent->bounds.y + p[UI_Y] + p[UI_OFFSET_Y],
                    p[UI_WIDTH], p[UI_HEIGHT] },
        .opacity = parent->opacity * p[UI_OPACITY] / UI_FULL_OPACITY,
        .brightness = parent->brightness * p[UI_BRIGHTNESS] / UI_FULL_OPACITY,
        .scroll_offset = p[UI_SCROLL_OFFSET], .selection = p[UI_SELECTION],
    };
    struct ui_rect clip = { frame.bounds.x + p[UI_CLIP_X],
                            frame.bounds.y + p[UI_CLIP_Y],
                            p[UI_CLIP_WIDTH] < 0 ? frame.bounds.width : p[UI_CLIP_WIDTH],
                            p[UI_CLIP_HEIGHT] < 0 ? frame.bounds.height : p[UI_CLIP_HEIGHT] };
    frame.clip = intersect(intersect(frame.bounds, clip), parent->clip);
    return frame;
}

static struct ui_view_frame view_frame(const struct ui_view *view)
{
    struct ui_view_frame parent = view->parent ? view_frame(view->parent) : screen_frame();
    return resolve(view, &parent);
}

void ui_view_init(struct ui_view *view, struct ui_rect bounds,
                  const struct ui_view_ops *ops, void *data)
{
    *view = (struct ui_view){ .ops = ops, .data = data, .value = {
        [UI_X] = bounds.x, [UI_Y] = bounds.y,
        [UI_WIDTH] = bounds.width, [UI_HEIGHT] = bounds.height,
        [UI_CLIP_WIDTH] = -1, [UI_CLIP_HEIGHT] = -1,
        [UI_OPACITY] = UI_FULL_OPACITY, [UI_BRIGHTNESS] = UI_FULL_OPACITY,
    }};
}

void ui_view_append(struct ui_view *parent, struct ui_view *child)
{
    struct ui_view **link = &parent->first_child;
    while (*link)
        link = &(*link)->next_sibling;
    *link = child;
    child->parent = parent;
}

void ui_page_init(struct ui_page *page, struct ui_view *root)
{
    *page = (struct ui_page){ .root = root, .focus = root,
                             .damage = view_frame(root).clip };
}

void ui_page_invalidate(struct ui_page *page, struct ui_rect rect)
{
    page->damage = unite(page->damage, intersect(rect, screen));
}

static void layout(struct ui_view *view, struct ui_rect bounds)
{
    if (view->ops && view->ops->layout)
        view->ops->layout(view, &bounds);
    bounds = (struct ui_rect){0, 0, view->value[UI_WIDTH], view->value[UI_HEIGHT]};
    for (struct ui_view *child = view->first_child; child; child = child->next_sibling)
        layout(child, bounds);
}

void ui_page_layout(struct ui_page *page)
{
    struct ui_rect before = view_frame(page->root).clip;
    layout(page->root, screen);
    ui_page_invalidate(page, unite(before, view_frame(page->root).clip));
}

static void set_value(struct ui_page *page, struct ui_view *view,
                      enum ui_property property, int value)
{
    if (property == UI_OPACITY || property == UI_BRIGHTNESS)
        value = MIN(MAX(value, 0), UI_FULL_OPACITY);
    if (view->value[property] == value)
        return;

    struct ui_rect before = view_frame(view).clip;
    view->value[property] = value;
    ui_page_invalidate(page, unite(before, view_frame(view).clip));
}

static void unlink_track(struct ui_page *page, const struct ui_track *reuse,
                         const struct ui_view *view, enum ui_property property)
{
    struct ui_track **link = &page->tracks;
    while (*link)
    {
        struct ui_track *track = *link;
        if (track == reuse || (track->view == view && track->property == property))
            *link = track->next;
        else
            link = &track->next;
    }
}

void ui_page_set(struct ui_page *page, struct ui_view *view,
                 enum ui_property property, int value)
{
    unlink_track(page, NULL, view, property);
    set_value(page, view, property, value);
}

void ui_page_tween(struct ui_page *page, struct ui_track *track, struct ui_view *view,
                   enum ui_property property, int target, unsigned frames,
                   bool ease_in, uint32_t clock)
{
    frames = ui_theme_duration(frames);
    int from = view->value[property];
    for (const struct ui_track *old = page->tracks; old; old = old->next)
    {
        if (old->view == view && old->property == property)
        {
            from = ui_tween_sample(&old->tween,
                                   ui_clock_frame(clock - old->tween.start_clock));
            break;
        }
    }
    unlink_track(page, track, view, property);
    if (!frames || from == target)
    {
        set_value(page, view, property, target);
        return;
    }

    set_value(page, view, property, from);
    *track = (struct ui_track){ .next = page->tracks, .view = view, .property = property,
        .tween = { .start_clock = clock, .from = from, .to = target,
                   .frames = frames, .ease_in = ease_in }};
    page->tracks = track;
}

void ui_page_cancel_tracks(struct ui_page *page)
{
    page->tracks = NULL;
}

static void step_view(struct ui_view *view, uint32_t clock)
{
    if (view->ops && view->ops->step)
        view->ops->step(view, clock);
    for (struct ui_view *child = view->first_child; child; child = child->next_sibling)
        step_view(child, clock);
}

void ui_page_step(struct ui_page *page, uint32_t clock)
{
    struct ui_track **link = &page->tracks;
    while (*link)
    {
        struct ui_track *track = *link;
        unsigned frame = ui_clock_frame(clock - track->tween.start_clock);
        set_value(page, track->view, track->property, ui_tween_sample(&track->tween, frame));
        if (frame >= track->tween.frames)
            *link = track->next;
        else
            link = &track->next;
    }
    step_view(page->root, clock);
}

static void paint_view(const struct ui_view *view, const struct ui_view_frame *parent,
                       struct ui_rect damage)
{
    struct ui_view_frame frame = resolve(view, parent);
    struct ui_rect clipped = intersect(frame.clip, damage);
    if (!frame.opacity || empty(clipped))
        return;

    if (view->ops && view->ops->paint)
    {
        struct ui_view_frame paint = frame;
        paint.clip = clipped;
        view->ops->paint(view, &paint);
    }
    for (const struct ui_view *child = view->first_child; child; child = child->next_sibling)
        paint_view(child, &frame, damage);
}

static void paint_page(struct ui_page *page, struct ui_rect damage)
{
    struct ui_view_frame root = screen_frame();
    unsigned int start = USEC_TIMER;
    paint_view(page->root, &root, damage);
    ui_note_render(USEC_TIMER - start);
    page->damage = (struct ui_rect){0};
}

struct ui_rect ui_page_paint(struct ui_page *page)
{
    struct ui_rect damage = page->damage;
    if (!empty(damage))
        paint_page(page, damage);
    return damage;
}

static void present(struct ui_rect damage)
{
    if (!empty(damage))
    {
        ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width, damage.height);
        ui_flush_dirty();
    }
}

void ui_page_draw(struct ui_page *page)
{
    present(ui_page_paint(page));
}

bool ui_page_dispatch(struct ui_page *page, int action)
{
    for (struct ui_view *view = page->focus; view; view = view->parent)
        if (view->ops && view->ops->action && view->ops->action(view, action))
            return true;
    return false;
}

bool ui_nav_push(struct ui_nav *nav, struct ui_page *page, enum ui_presentation type)
{
    if (nav->depth == UI_NAV_DEPTH)
        return false;
    struct ui_page *from = nav->depth ? nav->entry[nav->depth - 1].page : NULL;
    nav->entry[nav->depth++] = (struct ui_nav_entry){page, type};
    nav->damage = unite(nav->damage, type == UI_PAGE ? screen : view_frame(page->root).clip);
    if (nav->changed)
        nav->changed(nav, from, page, type, true);
    return true;
}

struct ui_page *ui_nav_pop(struct ui_nav *nav)
{
    if (!nav->depth)
        return NULL;
    struct ui_nav_entry old = nav->entry[--nav->depth];
    struct ui_page *to = nav->depth ? nav->entry[nav->depth - 1].page : NULL;
    nav->damage = unite(nav->damage, old.presentation == UI_PAGE ?
                                     screen : view_frame(old.page->root).clip);
    if (nav->changed)
        nav->changed(nav, old.page, to, old.presentation, false);
    ui_page_cancel_tracks(old.page);
    return old.page;
}

void ui_nav_draw(struct ui_nav *nav)
{
    if (!nav->depth)
        return;
    unsigned first = nav->depth - 1;
    while (first && nav->entry[first].presentation != UI_PAGE)
        --first;
    struct ui_rect damage = nav->damage;
    for (unsigned i = first; i < nav->depth; ++i)
        damage = unite(damage, nav->entry[i].page->damage);
    if (!empty(damage))
    {
        for (unsigned i = first; i < nav->depth; ++i)
            paint_page(nav->entry[i].page, damage);
        present(damage);
    }
    nav->damage = (struct ui_rect){0};
}

bool ui_nav_dispatch(struct ui_nav *nav, int action)
{
    return nav->depth && ui_page_dispatch(nav->entry[nav->depth - 1].page, action);
}

bool ui_layer_alloc(struct ui_layer *layer, int width, int height)
{
    if (layer->handle > 0 || width <= 0 || width > LCD_WIDTH ||
        height <= 0 || height > LCD_HEIGHT)
        return false;
    int handle = core_alloc_free_space(width * height * sizeof(fb_data));
    if (handle <= 0)
        return false;
    *layer = (struct ui_layer){ .handle = handle, .width = width, .height = height };
    return true;
}

void ui_layer_free(struct ui_layer *layer)
{
    if (layer->handle > 0)
        layer->handle = core_free(layer->handle);
}

#ifdef __thumb__
#define UI_PIXELS_ARM __attribute__((target("arm")))
#else
#define UI_PIXELS_ARM
#endif

static UI_PIXELS_ARM fb_data layer_pixel(fb_data src, fb_data dst,
                                         unsigned ink, unsigned light)
{
    unsigned back = UI_FULL_OPACITY - ink;
    unsigned red = (((src >> 11) * light / UI_FULL_OPACITY) * ink +
                     (dst >> 11) * back) / UI_FULL_OPACITY;
    unsigned green = ((((src >> 5) & 63) * light / UI_FULL_OPACITY) * ink +
                       ((dst >> 5) & 63) * back) / UI_FULL_OPACITY;
    unsigned blue = (((src & 31) * light / UI_FULL_OPACITY) * ink +
                      (dst & 31) * back) / UI_FULL_OPACITY;
    return (red << 11) | (green << 5) | blue;
}

/* Red and blue occupy separate 16-bit lanes. Each weighted sum needs at most
 * 13 bits, so an ARM word can blend both channels in one multiply pair. */
static UI_PIXELS_ARM uint32_t red_blue(fb_data pixel)
{
    return ((uint32_t)(pixel & 0xf800) << 5) | (pixel & 31);
}

UI_PIXELS_ARM void ui_blit_dim(fb_data *dst, const fb_data *src, int pixels,
                              unsigned brightness)
{
    if (brightness == UI_FULL_OPACITY)
    {
        memcpy(dst, src, pixels * sizeof(*dst));
        return;
    }
    for (int x = 0; x < pixels; ++x)
    {
        fb_data pixel = src[x];
        uint32_t rb = red_blue(pixel) * brightness >> 8;
        unsigned green = ((pixel >> 5) & 63) * brightness >> 8;
        dst[x] = ((rb >> 5) & 0xf800) | (green << 5) | (rb & 31);
    }
}

UI_PIXELS_ARM void ui_blit_tint(fb_data *dst, const fb_data *src, int pixels,
                               unsigned brightness, fb_data tint)
{
    if (!tint || brightness == UI_FULL_OPACITY)
    {
        ui_blit_dim(dst, src, pixels, brightness);
        return;
    }

    unsigned ink = UI_FULL_OPACITY - brightness;
    uint32_t rb_tint = red_blue(tint) * ink;
    unsigned g_tint = ((tint >> 5) & 63) * ink;
    for (int x = 0; x < pixels; ++x)
    {
        fb_data pixel = src[x];
        uint32_t rb = (red_blue(pixel) * brightness + rb_tint) >> 8;
        unsigned g = (((pixel >> 5) & 63) * brightness + g_tint) >> 8;
        dst[x] = ((rb >> 5) & 0xf800) | (g << 5) | (rb & 31);
    }
}

static UI_PIXELS_ARM void blend_aligned(fb_data *dst, const fb_data *src, int count,
                                        unsigned ink, unsigned light)
{
    if (ink == UI_FULL_OPACITY && light == UI_FULL_OPACITY)
    {
        memcpy(dst, src, count * sizeof(*dst));
        return;
    }
    if (light != UI_FULL_OPACITY && ink != UI_FULL_OPACITY)
    {
        for (int x = 0; x < count; ++x)
            dst[x] = layer_pixel(src[x], dst[x], ink, light);
        return;
    }

    ui_blit_mix(dst, src, dst, count, MIN(ink, light), UI_FULL_OPACITY - ink);
}

UI_PIXELS_ARM void ui_blit_mix(fb_data *dst, const fb_data *above, const fb_data *below,
                             int pixels, unsigned above_weight, unsigned below_weight)
{
    for (int x = 0; x < pixels; ++x)
    {
        fb_data a = above[x], b = below[x];
        uint32_t rb = (red_blue(a) * above_weight + red_blue(b) * below_weight) >> 8;
        unsigned green = ((((a >> 5) & 63) * above_weight) +
                          (((b >> 5) & 63) * below_weight)) >> 8;
        dst[x] = ((rb >> 5) & 0xf800) | (green << 5) | (rb & 31);
    }
}

UI_PIXELS_ARM void ui_layer_paint(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct ui_layer *layer = view->data;
    if (layer->handle <= 0 || empty(frame->clip))
        return;
    const struct ui_rect *bounds = &frame->bounds;
    const struct ui_rect *clip = &frame->clip;
    fb_data *data = core_get_data_pinned(layer->handle);
    const fb_data *pixels = data + layer->offset_pixels;
    bool exact = layer->width == bounds->width && layer->height == bounds->height;
    int left = clip->x - bounds->x;
    int sx_first = exact ? left : left * layer->width / bounds->width;
    int phase_first = exact ? 0 : left * layer->width % bounds->width;
    int step = exact ? 1 : layer->width / bounds->width;
    int remainder = exact ? 0 : layer->width % bounds->width;
    for (int y = clip->y; y < clip->y + clip->height; ++y)
    {
        int sy = y - bounds->y;
        if (!exact)
            sy = sy * layer->height / bounds->height;
        fb_data *dst = lcd_framebuffer_default.fb_ptr + y * LCD_WIDTH + clip->x;
        const fb_data *row = pixels + sy * layer->width;
        if (exact)
            blend_aligned(dst, row + left, clip->width, frame->opacity, frame->brightness);
        else
        {
            int sx = sx_first;
            int phase = phase_first;
            for (int x = 0; x < clip->width; ++x)
            {
                dst[x] = layer_pixel(row[sx], dst[x], frame->opacity, frame->brightness);
                sx += step;
                phase += remainder;
                if (phase >= bounds->width)
                {
                    phase -= bounds->width;
                    ++sx;
                }
            }
        }
    }
    core_put_data_pinned(data);
}
