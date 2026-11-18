/* PodBox scene transitions. GPL version 2+. */
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"
#include "thread.h"
#include "settings/settings.h"
#include "scene.h"
#include <string.h>

#define PAGE_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define SCENE_BYTES (3 * FRAMEBUFFER_SIZE)
#define PUSH_FRAMES ANIM_F(12)
#define POP_FRAMES ANIM_F(10)

static struct ui_scene *owner;
static unsigned int owner_thread;

size_t ui_scene_reserve_size(void)
{
    return global_settings.animations ?
        SCENE_BYTES + BUFLIB_ALLOC_OVERHEAD + sizeof(intptr_t) : 0;
}

static fb_data *surface(fb_data *data, bool front)
{
    return data + (front ? PAGE_PIXELS : 0);
}

static int position(const struct ui_scene *scene, unsigned frame)
{
    const struct ui_tween *tween = &scene->tween;
    int offset = ui_tween_sample(tween, frame) & ~1;
    /* Keep the last two-pixel strip until the endpoint is due. */
    if (frame < tween->frames && tween->from != tween->to && offset == tween->to)
        offset += tween->to == 0 ? 2 : -2;
    return offset;
}

static void compose(fb_data *data, int offset)
{
    const fb_data *back = surface(data, false);
    const fb_data *front = surface(data, true);
    fb_data *out = data + 2 * PAGE_PIXELS;

    for (int row = 0; row < LCD_HEIGHT; row++)
    {
        memcpy(out, back, offset * sizeof(*out));
        memcpy(out + offset, front, (LCD_WIDTH - offset) * sizeof(*out));
        back += LCD_WIDTH;
        front += LCD_WIDTH;
        out += LCD_WIDTH;
    }
}

static void release_scene(struct ui_scene *scene)
{
    if (!scene->handle)
        return;
    owner = NULL;
    lcd_hold_updates(false);
    scene->handle = core_free(scene->handle);
    scene->capturing = false;
    cpu_boost(false);
}

void ui_scene_cancel(struct ui_scene *scene)
{
    if (!scene->handle)
        return;
    release_scene(scene);
    struct viewport *previous = lcd_set_viewport(NULL);
    lcd_update();
    lcd_set_viewport(previous);
}

void ui_scene_interrupt(void)
{
    if (owner && thread_self() == owner_thread)
        ui_scene_cancel(owner);
}

bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push)
{
    if (!global_settings.animations || !lcd_active() || (owner && owner != scene))
        return false;

    uint32_t now = USEC_TIMER;
    bool active = scene->handle > 0;
    if (!active)
    {
        int handle = core_alloc_free_space(SCENE_BYTES);
        if (handle <= 0)
            return false;
        scene->handle = handle;
        owner_thread = thread_self();
        owner = scene;
        lcd_hold_updates(true);
        cpu_boost(true);
    }
    int offset = push ? LCD_WIDTH : 0;
    if (active && from == (push ? scene->back_page : scene->front_page) &&
                  to == (push ? scene->front_page : scene->back_page))
    {
        offset = position(scene, ui_clock_frame(now - scene->tween.start_clock));
        /* The redirected destination is already painted. Start incremental
         * updates from that page's own pixels, including its status bar. */
        fb_data *data = core_get_data_pinned(scene->handle);
        memcpy(lcd_framebuffer_default.fb_ptr, surface(data, push), FRAMEBUFFER_SIZE);
        core_put_data_pinned(data);
    }
    else
    {
        fb_data *data = core_get_data_pinned(scene->handle);
        const fb_data *previous = lcd_framebuffer_default.fb_ptr;
        if (active)
        {
            compose(data, position(scene, ui_clock_frame(now - scene->tween.start_clock)));
            previous = data + 2 * PAGE_PIXELS;
        }
        memcpy(surface(data, !push), previous, FRAMEBUFFER_SIZE);
        core_put_data_pinned(data);
        scene->back_page = push ? from : to;
        scene->front_page = push ? to : from;
    }

    scene->target_front = push;
    scene->tween = (struct ui_tween){
        .from = offset, .to = push ? 0 : LCD_WIDTH,
        .frames = push ? PUSH_FRAMES : POP_FRAMES, .ease_in = !push,
    };
    scene->last_offset = -1;
    scene->capturing = true;
    return true;
}

void ui_scene_start(struct ui_scene *scene)
{
    if (!scene->capturing)
        return;
    scene->capturing = false;
    scene->tween.start_clock = USEC_TIMER;
    ui_scene_update(scene, true);
}

void ui_scene_update(struct ui_scene *scene, bool target_changed)
{
    if (!scene->handle || scene->capturing)
        return;
    if (!lcd_active())
    {
        release_scene(scene);
        return;
    }

    unsigned frame = ui_clock_frame((uint32_t)USEC_TIMER - scene->tween.start_clock);
    if (frame >= scene->tween.frames || scene->tween.from == scene->tween.to)
    {
        if (lcd_present_scene(lcd_framebuffer_default.fb_ptr))
            release_scene(scene);
        return;
    }

    int offset = position(scene, frame);
    if (offset == scene->last_offset && !target_changed)
        return;
    scene->last_offset = offset;

    fb_data *data = core_get_data_pinned(scene->handle);
    if (target_changed)
        memcpy(surface(data, scene->target_front), lcd_framebuffer_default.fb_ptr,
               FRAMEBUFFER_SIZE);
    compose(data, offset);
    lcd_present_scene(data + 2 * PAGE_PIXELS);
    core_put_data_pinned(data);
}

int ui_scene_timeout(const struct ui_scene *scene, int timeout)
{
    if (!scene->handle || scene->capturing || timeout == TIMEOUT_NOBLOCK)
        return timeout;

    uint32_t elapsed = (uint32_t)USEC_TIMER - scene->tween.start_clock;
    unsigned frame = ui_clock_frame(elapsed);
    if (frame >= scene->tween.frames || scene->tween.from == scene->tween.to)
        return 1;
    if (position(scene, frame) != scene->last_offset)
        return TIMEOUT_NOBLOCK;

    int ticks = ui_clock_timeout(elapsed, scene->tween.frames, HZ);
    return timeout == TIMEOUT_BLOCK ? ticks : MIN(ticks, timeout);
}
