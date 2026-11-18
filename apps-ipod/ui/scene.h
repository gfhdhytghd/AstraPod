/* PodBox scene transitions. GPL version 2+. */
#ifndef PODBOX_UI_SCENE_H
#define PODBOX_UI_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include "animation.h"

struct ui_scene {
    const void *back_page, *front_page;
    struct ui_tween tween;
    int handle, last_offset;
    bool target_front, capturing;
};

/* The UI thread owns a scene from capture through the end of its transition.
 * The page painter continues to own the canonical framebuffer. */
bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push);
void ui_scene_start(struct ui_scene *scene);
void ui_scene_update(struct ui_scene *scene, bool target_changed);
int ui_scene_timeout(const struct ui_scene *scene, int timeout);
void ui_scene_cancel(struct ui_scene *scene);
void ui_scene_interrupt(void);
/* Space to leave free when laying out a new playback buffer. */
size_t ui_scene_reserve_size(void);

#endif
