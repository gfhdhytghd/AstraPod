/* PodBox: the stock iPod Classic's wheel scrolling and letter jump for
 * lists. GPL version 2+.
 *
 * Each part follows the stock 1.1.2 and 2.0.5 code, which is the same in
 * both: the speed estimate, the accumulator Cover Flow also uses, the list's
 * wheel handler, the group search and the letter key. Time is in
 * microseconds throughout; stock keeps the accumulator in milliseconds. */
#include <stdlib.h>
#include <string.h>
#include "list_quickscroll.h"

enum {
    SPEED_IDLE_US = 1000000,  /* a gap this long restarts the estimate */
    SLOW_US = 20000,          /* per report: slower runs a row an event */
    FAST_US = 17000,          /* per report: faster may start a jump */
    DOUBLE_US = 350000,       /* the multiplier doubles this often */
    BURST_US = 400000,        /* a gap this long resets it */
    MAX_MULT = 32,
    ENTER_STEPS = 16,         /* rows one event must move to start a jump */
    JUMP_POSITIONS = 4,       /* positions a group, with no multiplier */
    GALLOP = 32,              /* rows a stride when searching for a group */
};

static bool after(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) > 0;
}

void quickscroll_moved(struct quickscroll *qs, uint32_t now_us, int moves)
{
    if (moves <= 0)
        return;
    uint32_t each = (now_us - qs->moved_at) / moves;
    for (int i = 0; i < moves && i < 16; i++)
    {
        qs->ring[qs->slot] = each;
        qs->slot = (qs->slot + 1) & 15;
    }
    qs->moved_at = now_us;
}

/* The mean time of the last 16 reports, cleared when a read follows the
 * last one by an idle second. A cleared ring holds zeros, so the first
 * reports of a gesture read fast, as stock's do. */
static uint32_t speed(struct quickscroll *qs, uint32_t now)
{
    uint32_t read = qs->read_at;
    qs->read_at = now;
    if (!qs->timed || now - read > SPEED_IDLE_US)
    {
        memset(qs->ring, 0, sizeof(qs->ring));
        qs->timed = true;
        return 0;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++)
        sum += qs->ring[i];
    return sum >> 4;
}

/* A period of 0 is unknown, which counts as slow. */
static bool slow(const struct quickscroll *qs, uint32_t period)
{
    return !qs->accel || period == 0 || period > SLOW_US;
}

static bool fast(const struct quickscroll *qs, uint32_t period)
{
    return qs->accel && period != 0 && period < FAST_US;
}

static int accumulate(struct quickscroll *qs, uint32_t now, int motion,
                      uint32_t period)
{
    if (qs->fresh)
    {
        qs->mult = 1;
        qs->double_at = now + DOUBLE_US;
        qs->fresh = false;
    }
    else if (after(now, qs->double_at) && qs->mult < MAX_MULT)
    {
        qs->mult *= 2;
        qs->double_at = now + DOUBLE_US;
    }
    if (after(now, qs->burst_at) || qs->last_motion * motion < 0 ||
        slow(qs, period))
    {
        qs->mult = 1;
        qs->double_at = now + DOUBLE_US;
    }
    qs->burst_at = now + BURST_US;
    if (qs->accel)
        motion *= qs->mult;
    int sum = qs->rem + motion;
    int steps = sum / qs->threshold;
    qs->last_motion = motion;
    qs->rem = sum - steps * qs->threshold;
    return steps;
}

static void reset_accumulator(struct quickscroll *qs)
{
    qs->fresh = true;
    qs->last_motion = qs->rem = 0;
}

void quickscroll_reset(struct quickscroll *qs)
{
    memset(qs, 0, sizeof(*qs));
}

static void key_at(const struct quickscroll_list *list, int item, char *key)
{
    key[0] = '\0';
    list->key(item, key, list->ctx);
    if (!key[0])
        strcpy(key, QUICKSCROLL_NONE);
}

static bool key_is(const struct quickscroll_list *list, int item,
                   const char *key)
{
    char at[QUICKSCROLL_KEY_SIZE];
    key_at(list, item, at);
    return !strcmp(at, key);
}

static int clamp_row(const struct quickscroll_list *list, int item)
{
    return item < 0 ? 0 : item >= list->count ? list->count - 1 : item;
}

/* The first row of the next group (d > 0), or of the current group or the
 * one before it (d < 0). Strides are d rows, so an event of more than one
 * step can land past a group's first row or pass a one-row group. */
static int jump_to_group(const struct quickscroll_list *list, int sel, int d)
{
    char cur[QUICKSCROLL_KEY_SIZE];
    if (!d)
        return sel;
    key_at(list, sel, cur);
    int i = sel;
    if (d < 0 && sel - 1 >= 0 && !key_is(list, sel - 1, cur))
    {
        i = sel - 1;
        key_at(list, i, cur);
    }
    for (long p = (long)i + d; p >= 0 && p < list->count; p += (long)GALLOP * d)
    {
        if (!key_is(list, p, cur))
            break;
        i = p;
    }
    while (i >= 0 && i < list->count && key_is(list, i, cur))
        i += d;
    if (i >= 0 && i < list->count)
        return d > 0 ? i : i + 1;
    return d > 0 ? list->count - 1 : 0;
}

int quickscroll_group(const struct quickscroll_list *list, int item, int d)
{
    return jump_to_group(list, item, d);
}

static void set_mode(struct quickscroll *qs, int mode)
{
    if (mode == qs->mode)
        return;
    if (mode == 0)
    {
        qs->mode = 0;
        qs->touch_key[0] = '\0';
        return;
    }
    if (qs->mode == 0)
        qs->just_entered = true;
    qs->mode = mode;
}

/* A slow event moves at most one row. */
static int normal_rows(const struct quickscroll *qs, int steps, uint32_t period)
{
    if (slow(qs, period) && steps)
        steps = steps > 0 ? 1 : -1;
    return steps;
}

static void jump_scroll(struct quickscroll *qs,
                        const struct quickscroll_list *list,
                        struct quickscroll_move *move, int steps,
                        uint32_t period)
{
    char key[QUICKSCROLL_KEY_SIZE];
    int start = move->selected;
    int sel;
    if (qs->mode == 2)
    {
        sel = clamp_row(list, start + normal_rows(qs, steps, period));
        key_at(list, sel, key);
        if (strcmp(key, QUICKSCROLL_NONE))
            sel = jump_to_group(list, start, steps > 0 ? 1 : -1);
    }
    else
    {
        sel = jump_to_group(list, start, steps);
        key_at(list, sel, key);
        if (!strcmp(key, QUICKSCROLL_NONE) && steps < 0)
            sel = clamp_row(list, start - 1);
    }
    move->selected = sel;
    if (!strcmp(key, QUICKSCROLL_NONE))
        set_mode(qs, 2);
    else
    {
        set_mode(qs, 1);
        move->jumped = true;
    }
}

struct quickscroll_move quickscroll_wheel(struct quickscroll *qs,
                                          const struct quickscroll_list *list,
                                          uint32_t now_us, int positions,
                                          bool touched)
{
    struct quickscroll_move move = { list->selected, 0, false };
    if (list->count <= 0)
        return move;
    if (!qs->timed && !qs->live)
    {
        /* Made on a list's first wheel event: doubling is due at once. */
        qs->live = true;
        qs->mult = 1;
        qs->double_at = now_us;
        qs->burst_at = now_us + BURST_US;
    }
    int threshold = list->row_height > 30 ? 6 : 4;

    if (touched)
    {
        if (!qs->touch_key[0])
        {
            key_at(list, list->selected, qs->touch_key);
            qs->touch_index = list->selected;
        }
    }
    else
        quickscroll_release(qs);

    if (qs->mode == 1)
    {
        qs->accel = false;
        qs->threshold = JUMP_POSITIONS;
        qs->just_entered = false;
    }
    else
    {
        qs->accel = true;
        qs->threshold = threshold;
    }
    /* An event with no motion, as stock's on a touch, reads no speed. */
    if (!positions)
        return move;
    uint32_t period = speed(qs, now_us);
    int steps = accumulate(qs, now_us, positions, period);

    if (qs->mode)
        jump_scroll(qs, list, &move, steps, period);
    else if (qs->touch_index != -1 && abs(steps) >= ENTER_STEPS &&
             fast(qs, period) && !key_is(list, list->selected, qs->touch_key))
    {
        set_mode(qs, 1);
        jump_scroll(qs, list, &move, steps, period);
    }
    else
    {
        move.rows = normal_rows(qs, steps, period);
        move.selected = clamp_row(list, list->selected + move.rows);
    }
    return move;
}

bool quickscroll_release(struct quickscroll *qs)
{
    if (!qs->mode)
        return false;
    set_mode(qs, 0);
    qs->touch_index = -1;
    reset_accumulator(qs);
    return true;
}

bool quickscroll_overlay(const struct quickscroll *qs,
                         const struct quickscroll_list *list, char *key)
{
    key[0] = '\0';
    if (qs->mode == 0 || list->count <= 0)
        return false;
    key_at(list, list->selected, key);
    if (!strcmp(key, QUICKSCROLL_NONE))
    {
        key[0] = '\0';
        return false;
    }
    return true;
}

/* Stock's fold table: lower case and these accented letters file under
 * A to Z. Anything else below U+2E80 is filed with the digits; Ñ is missing
 * from stock's table and so files there too. */
static const struct { uint16_t cp; char letter; } folds[] = {
    {0xe0,'A'},{0xc0,'A'},{0xe2,'A'},{0xc2,'A'},{0xe6,'A'},{0xc6,'A'},
    {0xe4,'A'},{0xc4,'A'},{0xe1,'A'},{0xc1,'A'},{0xe5,'A'},{0xc5,'A'},
    {0xe3,'A'},{0xc3,'A'},{0x105,'A'},{0x104,'A'},
    {0xe7,'C'},{0xc7,'C'},{0x107,'C'},{0x106,'C'},{0x10d,'C'},{0x10c,'C'},
    {0x10f,'D'},{0x10e,'D'},
    {0xe9,'E'},{0xc9,'E'},{0xe8,'E'},{0xc8,'E'},{0xea,'E'},{0xca,'E'},
    {0xeb,'E'},{0xcb,'E'},{0x119,'E'},{0x118,'E'},{0x11b,'E'},{0x11a,'E'},
    {0x11f,'G'},{0x11e,'G'},
    {0xee,'I'},{0xce,'I'},{0xef,'I'},{0xcf,'I'},{0xed,'I'},{0xcd,'I'},
    {0xec,'I'},{0xcc,'I'},{0x131,'I'},{0x130,'I'},
    {0x142,'L'},{0x141,'L'},
    {0xf1,'N'},{0x144,'N'},{0x143,'N'},{0x148,'N'},{0x147,'N'},
    {0xf4,'O'},{0xd4,'O'},{0x153,'O'},{0x152,'O'},{0xf6,'O'},{0xd6,'O'},
    {0xf3,'O'},{0xd3,'O'},{0xf2,'O'},{0xd2,'O'},{0xf8,'O'},{0xd8,'O'},
    {0xf5,'O'},{0xd5,'O'},{0x151,'O'},{0x150,'O'},
    {0x159,'R'},{0x158,'R'},
    {0xdf,'S'},{0x15b,'S'},{0x15a,'S'},{0x15f,'S'},{0x15e,'S'},{0x161,'S'},
    {0x160,'S'},
    {0x165,'T'},{0x164,'T'},
    {0xf9,'U'},{0xd9,'U'},{0xfb,'U'},{0xdb,'U'},{0xfc,'U'},{0xdc,'U'},
    {0xfa,'U'},{0xda,'U'},{0x171,'U'},{0x170,'U'},{0x16f,'U'},{0x16e,'U'},
    {0xff,'Y'},{0x178,'Y'},{0xfd,'Y'},{0xdd,'Y'},
    {0x17c,'Z'},{0x17b,'Z'},{0x17a,'Z'},{0x179,'Z'},{0x17e,'Z'},{0x17d,'Z'},
};

static uint32_t first_char(const unsigned char *s)
{
    if (s[0] < 0x80)
        return s[0];
    int n = s[0] >= 0xf0 ? 3 : s[0] >= 0xe0 ? 2 : s[0] >= 0xc0 ? 1 : 0;
    uint32_t c = s[0] & (0x3f >> n);
    for (int i = 1; i <= n; i++)
    {
        if ((s[i] & 0xc0) != 0x80)
            return 0xfffd;
        c = c << 6 | (s[i] & 0x3f);
    }
    return n ? c : 0xfffd;
}

void quickscroll_key_of(const char *text, char *key)
{
    const unsigned char *s = (const unsigned char *)(text ? text : "");
    while (*s == ' ')
        s++;
    uint32_t c = first_char(s);
    char letter = 0;
    if (c >= 'a' && c <= 'z')
        letter = (char)(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z')
        letter = (char)c;
    else
    {
        for (size_t i = 0; i < sizeof(folds) / sizeof(folds[0]); i++)
        {
            if (folds[i].cp == c)
            {
                letter = folds[i].letter;
                break;
            }
        }
    }
    if (letter)
    {
        key[0] = letter;
        key[1] = '\0';
    }
    else
        strcpy(key, c == 0 || c >= 0x2e80 ? QUICKSCROLL_NONE : QUICKSCROLL_DIGITS);
}
