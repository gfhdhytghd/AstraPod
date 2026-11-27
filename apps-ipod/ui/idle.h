/* Idle screen scheduling. GPL version 2+. */
#ifndef PODBOX_UI_IDLE_H
#define PODBOX_UI_IDLE_H

#include <stdbool.h>
#include <stdint.h>

void ui_idle_list_present(void);
int ui_idle_status_timeout(uint32_t start, unsigned frames);
int ui_idle_wps_timeout(bool remote, bool settled, int meter_ticks);
int ui_idle_cover_boost_tail(void);
int ui_idle_cover_timeout(int timeout, long boost_until, bool boosted);

#endif
