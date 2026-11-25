/* PodBox scene transitions. GPL version 2+. */
#ifndef PODBOX_UI_SCENE_H
#define PODBOX_UI_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "animation.h"

struct ui_scene {
    const void *back_page, *front_page;
    struct ui_tween tween;
    int handle, backdrop_handle, composed_offset, last_offset, pan_top;
    int status_height, push_top, row_y, row_pitch, row_count;
    unsigned trace, dim_color, push_half_frames;
    bool target_front, capturing, automatic, vertical, root_entry;
    bool staggered, rows_changed, reuse_target;
};

/* The UI thread owns a scene from capture through the end of its transition.
 * The page painter continues to own the canonical framebuffer. */
bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push);
/* Move adjacent pages together below the fixed chrome. */
bool ui_scene_capture_pan(struct ui_scene *scene, const void *from, const void *to,
                          int direction, int top);
/* The incoming list reports the already-painted visible row bands. */
void ui_scene_push_rows(int y, int pitch, int count);
void ui_scene_snap_push(void);
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
/* Present a crossfade without modifying the page framebuffer. Optional
 * screen-space column spans keep moving artwork opaque (end exclusive). */
bool ui_scene_present_fade(struct ui_scene *scene, unsigned opacity,
                           const int16_t *art_top, const int16_t *art_end);
bool ui_scene_restore_modal(struct ui_scene *scene);
/* Draw the retained parent page afresh, off screen. The painter is given the
 * parent's page key and returns false, having drawn nothing, for a page it
 * does not know. */
void ui_scene_set_parent_painter(bool (*paint)(const void *page));
bool ui_scene_repaint_backdrop(struct ui_scene *scene);
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
