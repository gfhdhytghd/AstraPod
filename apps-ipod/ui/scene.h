/* PodBox scene transitions. GPL version 2+. */
#ifndef PODBOX_UI_SCENE_H
#define PODBOX_UI_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include "animation.h"

struct ui_scene {
    const void *back_page, *front_page;
    struct ui_tween tween;
    int handle, backdrop_handle, composed_offset, last_offset;
    int status_height;
    unsigned trace, dim_color;
    bool target_front, capturing, automatic, vertical, root_entry;
};

/* The UI thread owns a scene from capture through the end of its transition.
 * The page painter continues to own the canonical framebuffer. */
bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push);
void ui_scene_start(struct ui_scene *scene);
void ui_scene_update(struct ui_scene *scene, bool target_changed);
int ui_scene_timeout(const struct ui_scene *scene, int timeout);
void ui_scene_cancel(struct ui_scene *scene);
void ui_scene_discard(struct ui_scene *scene);
void ui_scene_interrupt(void);
void ui_scene_fade_out(void);
/* Root-level screens hand their first painted frame to the action pump. */
void ui_scene_enter(struct ui_scene *, const void *from, const void *to, bool push);
void ui_scene_boot(struct ui_scene *scene);
/* A full-page picker retains its underlying page until dismissal. */
void ui_scene_open_modal(struct ui_scene *scene, const char *route);
bool ui_scene_capture_modal(struct ui_scene *scene);
void ui_scene_show_modal(struct ui_scene *scene, const char *route);
void ui_scene_open_page(struct ui_scene *scene, const char *route);
const void *ui_scene_modal_parent(void);
void ui_scene_modal_restored(const void *page);
int ui_scene_modal_body_offset(int offset, int status_height);
/* Returns true after restoring the retained backdrop to the framebuffer. */
bool ui_scene_close_modal(struct ui_scene *scene, bool animate, const char *route);
bool ui_scene_automatic(void);
int ui_scene_poll(int timeout);
/* Space to leave free when laying out a new playback buffer. */
size_t ui_scene_reserve_size(void);

#endif
