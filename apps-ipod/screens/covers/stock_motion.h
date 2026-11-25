/* Cover Flow's time-based pose, live strip and perspective on a 320x240 LCD.
 * Coordinates, degrees, interpolation and projected pixels use signed Q16. */
#ifndef COVERS_STOCK_MOTION_H
#define COVERS_STOCK_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#define STOCK_ONE 65536
#define STOCK_CAMERA 160
#define STOCK_HALF 35
#define STOCK_BAND 100
#define STOCK_SPACING 15
#define STOCK_SCROLL_US 1000000u
#define STOCK_LEAVE_US 500000u
/* The 200-unit band holds at most 14 covers at 15-unit spacing. */
#define STOCK_MAX_VISIBLE 16
#define STOCK_STARTS (2 * STOCK_MAX_VISIBLE)

struct stock_pose { int32_t x, y, z, yaw; };
struct stock_edge { int32_t x, top, bottom, depth; };
struct stock_visible { int album; struct stock_pose pose; bool linear; };

enum stock_phase { STOCK_REST, STOCK_ENTER, STOCK_MENU_ENTER, STOCK_SCROLL, STOCK_LEAVE };
struct stock_flow {
    int count, selected, first, length;
    uint32_t start_us;
    enum stock_phase phase;
    bool playback_walls;
    struct stock_pose start[STOCK_STARTS];
};

static inline int stock_abs(int n)
{
    return n < 0 ? -n : n;
}

static inline int32_t stock_mul(int32_t a, int32_t b)
{
    return ((int64_t)a * b + STOCK_ONE / 2) >> 16;
}

static inline int stock_progress(uint32_t elapsed, uint32_t duration, bool leave)
{
    if (elapsed >= duration)
        return STOCK_ONE;
    /* duration is a multiple of 32: keep the fixed-point numerator in 32 bits. */
    int t = ((leave ? elapsed : duration - elapsed) * (STOCK_ONE / 32))
            / (duration / 32);
    int t2 = stock_mul(t, t);
    int power = stock_mul(stock_mul(t2, t2), t);
    return leave ? power : STOCK_ONE - power;
}

/* For a wall, x = 15 * rel + sign(rel) * 40. Keep the album-distance
 * term separate so large libraries never need a Q16 world-sized coordinate. */
static inline struct stock_pose stock_slot_base(int rel)
{
    int sign = (rel > 0) - (rel < 0);
    struct stock_pose p = { sign * (55 - STOCK_SPACING) * STOCK_ONE,
                            -9 * STOCK_ONE,
                            rel ? 40 * STOCK_ONE : 145 * (STOCK_ONE / 2),
                            sign * 90 * STOCK_ONE };
    return p;
}

static inline void stock_init(struct stock_flow *f, int count, int selected,
                              uint32_t now, bool enter)
{
    f->count = count;
    f->selected = selected;
    f->first = selected > 3 ? selected - 3 : 0;
    f->length = count - selected > 3 ? selected - f->first + 4 : count - f->first;
    f->start_us = now;
    f->phase = enter ? STOCK_ENTER : STOCK_REST;
    f->playback_walls = false;
    for (int i = 0; i < f->length; ++i) {
        int rel = f->first + i - selected;
        struct stock_pose p = stock_slot_base(rel);
        p.x += rel * STOCK_SPACING * STOCK_ONE;
        if (enter) {
            if (rel)
                p.x += (rel > 0 ? 100 : -100) * STOCK_ONE;
            else {
                p.x = -43 * STOCK_ONE;
                p.y = -10 * STOCK_ONE;
                p.yaw = -9 * STOCK_ONE;
            }
        }
        f->start[i] = p;
    }
}

/* The selected album enters as wall 1 of the right stack. Albums to its
 * right start one wall farther out and shift into their final slots. */
#define STOCK_MENU_TURN_US 400000u

static inline void stock_enter(struct stock_flow *f, int count, int selected,
                               uint32_t now, bool from_playback)
{
    stock_init(f, count, selected, now, true);
    if (from_playback || !count)
        return;
    f->phase = STOCK_MENU_ENTER;
    for (int i = selected - f->first; i < f->length; i++) {
        int wall = f->first + i - selected + 1;
        f->start[i] = stock_slot_base(wall);
        f->start[i].x += (100 + wall * STOCK_SPACING) * STOCK_ONE;
    }
}

static inline bool stock_entering(const struct stock_flow *f)
{
    return f->phase == STOCK_ENTER || f->phase == STOCK_MENU_ENTER;
}

static inline int stock_scroll_progress(const struct stock_flow *f, uint32_t now)
{
    return f->phase == STOCK_SCROLL ?
           stock_progress(now - f->start_us, STOCK_SCROLL_US, false) : STOCK_ONE;
}

static inline struct stock_visible stock_current(const struct stock_flow *f,
                                  int album, uint32_t now, int e)
{
    int rel = album - f->selected;
    int i = album - f->first;
    int anchor = i < 0 ? 0 : i >= f->length ? f->length - 1 : i;
    struct stock_pose a = f->start[anchor], b = stock_slot_base(rel);
    int64_t from_slots = i - anchor, to_slots = rel;
    uint32_t elapsed = now - f->start_us;
    int32_t entry_offset = 0;
    if (f->phase == STOCK_MENU_ENTER && rel >= 0) {
        /* Translate the stack in wall order. Once the inner wall is visible,
         * apply the normal one-slot scroll to every right-hand member. */
        unsigned delay = (unsigned)(rel + 1) * 100000u;
        int ingress = stock_progress(elapsed > delay ? elapsed - delay : 0,
                                     STOCK_SCROLL_US, false);
        entry_offset = 100 * (STOCK_ONE - ingress);
        a.x -= 100 * STOCK_ONE;
        e = stock_progress(elapsed > STOCK_MENU_TURN_US ? elapsed - STOCK_MENU_TURN_US : 0,
                           STOCK_SCROLL_US, false);
    } else if (stock_entering(f)) {
        unsigned delay = rel ? (unsigned)stock_abs(rel) * 100000u : 250000u;
        e = stock_progress(elapsed > delay ? elapsed - delay : 0, STOCK_SCROLL_US, false);
    } else if (f->phase == STOCK_LEAVE) {
        unsigned back = stock_abs(rel) >= 3 ? STOCK_LEAVE_US :
                        250000u + (unsigned)stock_abs(rel) * 100000u;
        e = elapsed >= STOCK_LEAVE_US ? STOCK_ONE :
            stock_progress(elapsed + back, STOCK_LEAVE_US, true);
        if (rel) {
            b.x += (rel > 0 ? 100 : -100) * STOCK_ONE;
        } else {
            b = (struct stock_pose){170 * STOCK_ONE, -9 * STOCK_ONE,
                                    40 * STOCK_ONE, 90 * STOCK_ONE};
        }
    }
    int64_t x = entry_offset + a.x + stock_mul(b.x - a.x, e) +
                STOCK_SPACING * (from_slots * (STOCK_ONE - e) + to_slots * e);
    struct stock_visible v = {
        .album = album,
        .pose = { x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : (int32_t)x,
                  a.y + stock_mul(b.y - a.y, e),
                  a.z + stock_mul(b.z - a.z, e),
                  a.yaw + stock_mul(b.yaw - a.yaw, e) },
        .linear = e >= 65209  /* ceil(0.995 * 65536) */
    };
    return v;
}

/* Scrolling preserves album order and at least one slot spacing between
 * centres. Beyond the live strip, each new start inherits its neighbour's
 * timer and pose, offset by one spacing. Locate the strip at the event time
 * so rendering fewer frames cannot change which pose a retarget captures. */
static inline int stock_first(const struct stock_flow *f, uint32_t now, int e)
{
    if (f->phase != STOCK_SCROLL)
        return f->first;
    int lo = 0, hi = f->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (stock_current(f, mid, now, e).pose.x <= -STOCK_BAND * STOCK_ONE)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static inline void stock_snapshot(struct stock_flow *f, uint32_t now)
{
    struct stock_pose next[STOCK_STARTS];
    int e = stock_scroll_progress(f, now);
    int first = stock_first(f, now, e);
    if (f->phase == STOCK_SCROLL && first > 0) {
        --first;
        /* Retain the target and its adjacent wall when input outruns the
         * visible strip. Only these offscreen poses differ from translating a
         * neighbour by one slot. The visible strip always has room. */
        int left = f->selected > 0 ? f->selected - 1 : 0;
        if (first > left) {
            int extend = first - left;
            first -= extend < STOCK_MAX_VISIBLE ? extend : STOCK_MAX_VISIBLE;
        }
    }
    int n = 0;
    for (int a = first; a < f->count && n < STOCK_STARTS; ++a) {
        if (f->phase != STOCK_SCROLL && n == f->length)
            break;
        struct stock_pose p = stock_current(f, a, now, e).pose;
        next[n++] = p;
        if (f->phase == STOCK_SCROLL && p.x >= STOCK_BAND * STOCK_ONE && a > f->selected)
            break;
    }
    for (int i = 0; i < n; ++i)
        f->start[i] = next[i];
    f->first = first;
    f->length = n;
    f->start_us = now;
}

static inline void stock_scroll(struct stock_flow *f, int steps, uint32_t now)
{
    if (!f->count || f->phase == STOCK_LEAVE)
        return;
    if (steps < -f->selected)
        steps = -f->selected;
    if (steps > f->count - 1 - f->selected)
        steps = f->count - 1 - f->selected;
    if (steps) {
        stock_snapshot(f, now);
        f->selected += steps;
        f->phase = STOCK_SCROLL;
    }
}

static inline void stock_leave(struct stock_flow *f, uint32_t now)
{
    stock_snapshot(f, now);
    f->phase = STOCK_LEAVE;
    f->playback_walls = false;
}

static inline bool stock_animating(const struct stock_flow *f, uint32_t now)
{
    uint32_t duration = STOCK_SCROLL_US;
    if (f->phase == STOCK_REST || !f->count)
        return false;
    if (f->phase == STOCK_LEAVE)
        duration = 250000;
    else if (f->phase == STOCK_MENU_ENTER)
        duration += STOCK_MENU_TURN_US;
    else if (f->phase == STOCK_ENTER)
        duration += (f->selected >= 3 || f->count - f->selected > 3) ? 300000 : 250000;
    return now - f->start_us < duration;
}

static inline int stock_sample(const struct stock_flow *f, uint32_t now,
                               struct stock_visible out[STOCK_MAX_VISIBLE])
{
    int e = stock_scroll_progress(f, now);
    int limit = f->phase == STOCK_SCROLL ? f->count : f->first + f->length;
    int n = 0;
    for (int a = stock_first(f, now, e); a < limit && n < STOCK_MAX_VISIBLE; ++a) {
        struct stock_visible v = stock_current(f, a, now, e);
        if (v.pose.x >= STOCK_BAND * STOCK_ONE && f->phase == STOCK_SCROLL)
            break;
        /* Playback and entry walls remain visible until their last pixels
         * cross the viewport edge. Scrolling recycles at the world band. */
        if (!stock_entering(f) && !f->playback_walls &&
            (v.pose.x <= -STOCK_BAND * STOCK_ONE || v.pose.x >= STOCK_BAND * STOCK_ONE))
            continue;
        int pos = n++;
        while (pos && stock_abs(v.pose.x) > stock_abs(out[pos - 1].pose.x)) {
            out[pos] = out[pos - 1];
            --pos;
        }
        out[pos] = v;
    }
    if (n > 1) {
        int front = n - 1;
        for (int i = n - 2; i >= 0; --i)
            if (out[i].pose.z > out[front].pose.z)
                front = i;
        struct stock_visible v = out[front];
        for (int i = front; i < n - 1; ++i)
            out[i] = out[i + 1];
        out[n - 1] = v;
    }
    return n;
}

/* Keep exactly the row on screen at the playback handoff. The first wall
 * reaches the recycling boundary before its far edge leaves the LCD. */
static inline void stock_leave_to_playing(struct stock_flow *f, uint32_t now)
{
    struct stock_visible visible[STOCK_MAX_VISIBLE];
    int n = stock_sample(f, now, visible);
    int first = f->count;
    for (int i = 0; i < n; ++i)
        if (visible[i].album < first)
            first = visible[i].album;
    for (int i = 0; i < n; ++i)
        f->start[visible[i].album - first] = visible[i].pose;
    f->first = first;
    f->length = n;
    f->start_us = now;
    f->phase = STOCK_LEAVE;
    f->playback_walls = true;
}

/* A and D inherit the selected cover's current pose. At BACK, +180 and
 * -180 project the same panel but start different turn directions. */
enum stock_transition {
    STOCK_TO_BACK, STOCK_TO_FRONT, STOCK_PANEL_TO_PLAYING,
    STOCK_COVER_TO_PLAYING, STOCK_FROM_PLAYING
};

struct stock_flip {
    struct stock_pose start, target;
    uint32_t start_us, duration, delay;
    bool quadratic;
};

static inline struct stock_pose stock_playing_pose(void)
{
    return (struct stock_pose){-43 * STOCK_ONE, -10 * STOCK_ONE,
                               145 * (STOCK_ONE / 2), -9 * STOCK_ONE};
}

static inline void stock_flip_start(struct stock_flip *f,
                                    enum stock_transition transition,
                                    struct stock_pose current, uint32_t now)
{
    const struct stock_pose centre = stock_slot_base(0);
    struct stock_pose back = {0, 632422, 465 * (STOCK_ONE / 4), 180 * STOCK_ONE};
    /* BACK y = 9.65 and z = 116.25 put a 256-pixel texture at native scale. */
    f->start_us = now;
    f->delay = 0;
    f->duration = 500000;
    f->quadratic = false;
    switch (transition) {
    case STOCK_TO_BACK:
        f->start = current;
        if (current.x < 0)
            back.yaw = -back.yaw;
        f->target = back;
        break;
    case STOCK_TO_FRONT:
        f->start = back;
        f->target = centre;
        f->duration = STOCK_SCROLL_US;
        break;
    case STOCK_PANEL_TO_PLAYING:
        back.yaw = -back.yaw;
        current = back;
        /* fall through */
    case STOCK_COVER_TO_PLAYING:
        f->start = current;
        f->target = stock_playing_pose();
        f->quadratic = true;
        break;
    case STOCK_FROM_PLAYING:
        f->start = stock_playing_pose();
        f->target = centre;
        f->duration = STOCK_SCROLL_US;
        f->delay = 250000;
        break;
    }
}

static inline bool stock_flip_done(const struct stock_flip *f, uint32_t now)
{
    return now - f->start_us >= f->delay + f->duration;
}

static inline int32_t stock_lerp(int32_t a, int32_t b, int e)
{
    return ((int64_t)a * (STOCK_ONE - e) + (int64_t)b * e + STOCK_ONE / 2) >> 16;
}

static inline struct stock_pose stock_flip_pose(const struct stock_flip *f,
                                                uint32_t now)
{
    uint32_t elapsed = now - f->start_us;
    if (elapsed <= f->delay)
        return f->start;
    elapsed -= f->delay;
    if (elapsed >= f->duration)
        return f->target;
    int e;
    if (f->quadratic) {
        int t = ((f->duration - elapsed) * (STOCK_ONE / 32)) / (f->duration / 32);
        e = STOCK_ONE - stock_mul(t, t);
    } else {
        e = stock_progress(elapsed, f->duration, false);
    }
    return (struct stock_pose){
        stock_lerp(f->start.x, f->target.x, e),
        stock_lerp(f->start.y, f->target.y, e),
        stock_lerp(f->start.z, f->target.z, e),
        stock_lerp(f->start.yaw, f->target.yaw, e)
    };
}

static inline bool stock_back_face(struct stock_pose pose)
{
    return stock_abs(pose.yaw) > 90 * STOCK_ONE;
}

/* First quadrant at five-degree intervals, with linear interpolation. */
static inline int stock_sin(int degrees)
{
    static const int table[] = {
        0, 5712, 11380, 16962, 22415, 27697, 32768, 37590, 42126, 46341,
        50203, 53684, 56756, 59396, 61584, 63303, 64540, 65287, 65536
    };
    unsigned angle = stock_abs(degrees);
    if (angle > 90 * STOCK_ONE)
        angle = 180 * STOCK_ONE - angle;
    unsigned i = angle / (5 * STOCK_ONE);
    int value = table[i];
    if (i < 18)
        value += stock_mul(table[i + 1] - value, (angle - i * 5 * STOCK_ONE) / 5);
    return degrees < 0 ? -value : value;
}

static inline void stock_project(struct stock_pose p, struct stock_edge edges[2])
{
    int dx = STOCK_HALF * stock_sin(90 * STOCK_ONE - stock_abs(p.yaw));
    int dz = STOCK_HALF * stock_sin(p.yaw);
    for (int i = 0; i < 2; ++i) {
        int depth = STOCK_CAMERA * STOCK_ONE - (p.z + (i ? dz : -dz));
        /* A Q8 denominator keeps the perspective divide in 32 bits on ARM. */
        int scale = (STOCK_CAMERA * STOCK_ONE * 256u) / ((depth + 128) >> 8);
        edges[i] = (struct stock_edge){
            160 * STOCK_ONE + stock_mul(p.x + (i ? dx : -dx), scale),
            120 * STOCK_ONE + stock_mul(p.y - STOCK_HALF * STOCK_ONE, scale),
            120 * STOCK_ONE + stock_mul(p.y + STOCK_HALF * STOCK_ONE, scale),
            depth
        };
    }
}
#endif
