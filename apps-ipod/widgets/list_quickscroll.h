/* PodBox: the stock iPod Classic's wheel scrolling and letter jump for
 * lists. GPL version 2+.
 *
 * The stock firmware turns wheel motion into rows with an accelerating
 * accumulator, and a spin fast enough to move 16 rows in one event, once the
 * selection has left the letter the touch began on, switches the list to
 * jumping a letter group per detent until the finger leaves the wheel. This
 * is that logic, without the list: the caller supplies time, raw wheel
 * positions, whether the wheel is touched, and each row's key. No Rockbox
 * headers, so a host test replays recorded stock input through it. */
#ifndef PODBOX_LIST_QUICKSCROLL_H
#define PODBOX_LIST_QUICKSCROLL_H

#include <stdbool.h>
#include <stdint.h>

/* A row's key: one letter A to Z, QUICKSCROLL_DIGITS for digits, other
 * scripts and punctuation, QUICKSCROLL_NONE for CJK and empty names, which
 * show no overlay and scroll row by row; or a caller's own group label. */
#define QUICKSCROLL_KEY_SIZE 16
#define QUICKSCROLL_DIGITS "#"
#define QUICKSCROLL_NONE "!"

typedef void (*quickscroll_key_fn)(int item, char *key, void *ctx);

struct quickscroll {
    /* Wheel speed: microseconds per wheel report, a ring of 16. */
    uint32_t ring[16];
    uint32_t moved_at, read_at;
    uint8_t slot;
    bool timed;
    /* The accumulator. */
    uint32_t double_at, burst_at;
    int mult, last_motion, rem, threshold;
    bool live, fresh, accel;
    /* Letter jump: 0 off, 1 jumping, 2 scrolling a block with no key. */
    uint8_t mode;
    bool just_entered;
    int touch_index;
    char touch_key[QUICKSCROLL_KEY_SIZE];
};

struct quickscroll_list {
    int count, selected;
    /* Height of a row in pixels: past 30, six positions make a row. */
    int row_height;
    quickscroll_key_fn key;
    void *ctx;
};

struct quickscroll_move {
    int selected;   /* the new selection */
    int rows;       /* rows to scroll from the old one, 0 for a jump */
    bool jumped;    /* a group jump: show the new selection at the top */
};

void quickscroll_reset(struct quickscroll *qs);
/* The wheel made `moves` reports, the last at `now_us`. Stock times each
 * report as it arrives, and the speed is the mean time over the last 16. */
void quickscroll_moved(struct quickscroll *qs, uint32_t now_us, int moves);
/* One wheel event: `positions` raw positions (96 a turn, signed) at
 * `now_us`, with the finger on the wheel or not. */
struct quickscroll_move quickscroll_wheel(struct quickscroll *qs,
                                          const struct quickscroll_list *list,
                                          uint32_t now_us, int positions,
                                          bool touched);
/* The first row of the group after `item` (d = 1) or before it (d = -1),
 * where a letter jump of one step goes. */
int quickscroll_group(const struct quickscroll_list *list, int item, int d);
/* The finger left the wheel: true when that ended a letter jump. */
bool quickscroll_release(struct quickscroll *qs);
/* Whether the letter overlay shows, and its key, for the selection. */
bool quickscroll_overlay(const struct quickscroll *qs,
                         const struct quickscroll_list *list, char *key);
/* The stock key of a name: its first character after spaces, folded. */
void quickscroll_key_of(const char *text, char *key);

#endif
