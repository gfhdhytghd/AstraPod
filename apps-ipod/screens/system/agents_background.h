/* Pocket remote notifications. GPL version 2+. */
#ifndef AGENTS_BACKGROUND_H
#define AGENTS_BACKGROUND_H

#include <stdbool.h>
#include <stdint.h>

struct hr_model;
struct podlink_message;
struct ui_dirty_rect;
struct viewport;

struct agents_background_target {
    char question[12];
    char project[16];
    char thread[16];
    char thread_title[61];
    char status;
};

enum agents_badge_kind { AGENTS_BADGE_HIDDEN, AGENTS_BADGE_WAITING,
                         AGENTS_BADGE_FAILED, AGENTS_BADGE_WORKING,
                         AGENTS_BADGE_DONE };

struct agents_badge {
    uint8_t kind, count, ink;
};

void agents_background_update(const struct hr_model *model, int type, bool foreground);
void agents_background_disconnected(void);
void agents_background_enter(void);
bool agents_background_take_target(struct agents_background_target *target);

bool agents_background_receive(const struct podlink_message *message);
bool agents_background_foreground(void);
bool agents_fruit_nav(void);
void agents_background_reset_model(void);

unsigned int agents_background_next(char *record, unsigned int size);
bool agents_background_handshake_complete(void);
void agents_background_handshake_sent(void);
void agents_background_reset(void);
void agents_background_play_tone(void);
bool agents_background_redraw_due(void);

struct agents_badge agents_background_badge(void);
int agents_background_badge_width(struct agents_badge badge);
void agents_background_draw_badge(struct viewport *clip, struct agents_badge badge,
                                  int x, int y, int font, unsigned background);

/* Composite transient pixels immediately before LCD submission. Always
 * restore the underlying framebuffer afterward, including on Cover Flow. */
void agents_background_overlay(struct ui_dirty_rect *damage, bool cover_ready);
void agents_background_restore(void);

#endif
