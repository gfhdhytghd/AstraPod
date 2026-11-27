/* AstraPod foreground motion scheduling. GPL version 2+. */
#ifndef ASTRA_UI_MOTION_SCHEDULE_H
#define ASTRA_UI_MOTION_SCHEDULE_H

#include <stdbool.h>

struct ui_motion
{
    unsigned lease;
    unsigned trace;
};

void ui_motion_schedule_init(void);
struct ui_motion ui_motion_begin(const char *route);
/* Call when a new gesture retargets an animation already in flight. */
void ui_motion_renew(struct ui_motion *motion);
void ui_motion_end(struct ui_motion *motion);
bool ui_motion_active(void);

#endif
