#ifndef COVERS_TURN_H
#define COVERS_TURN_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Hinges, coefficients and seconds use Q24. The carrier coordinate and
 * projected edge coordinates use Q16. Global album indices are zero-based. */
#define TURN_ONE (1 << 24)
#define TURN_RADIUS 15
#define TURN_CARDS (2 * TURN_RADIUS + 1)
#define TURN_HISTORY_RADIUS 32
#define TURN_RING 128
#define TURN_LOCAL_RING 32
#define TURN_LOCAL_RADIUS 12

struct turn_hinge { int32_t g, b; };
struct turn_slot { int id; unsigned stamp; struct turn_hinge hinge; };
struct turn_row {
    int count, target, margin_delta;
    int64_t p;
    int32_t age, hold, weight, shock, bank_shock;
    int32_t previous_gap;
    int32_t carrier_rate; /* Q24 reciprocal seconds */
    int64_t carrier_offset; /* Q16 items */
    unsigned stamp;
    struct turn_slot slot[TURN_RING], local_slot[TURN_LOCAL_RING];
    struct turn_hinge far[2];
};
struct turn_card {
    int id;
    int32_t x[2], top[2], bottom[2], z[2], world_x[2];
};

#define TURN_FREE 96327395
#define TURN_IMPULSE 820320
#define TURN_HOLD 755079
#define TURN_WINDOW 671089
#define TURN_INV_WINDOW 419430400
#define TURN_INV_TAU 125335167
#define TURN_INV_BANK_TAU 242545359
#define TURN_FIELD 60833554
#define TURN_BANK 15792525
#define TURN_LOCAL 81220475
#define TURN_BANK_IMPULSE 767060
#define TURN_EXTRA 87153
#define TURN_TAIL 528415

/* Samples of 1-exp(-i/128), sin(pi*i/256) and 1/(1+(i/r)^4). */
static const int32_t turn_release_table[] = {
    0, 130561, 260107, 388644, 516181, 642725, 768285, 892867,
    1016480, 1139131, 1260828, 1381577, 1501387, 1620264, 1738217, 1855251,
    1971375, 2086595, 2200918, 2314352, 2426903, 2538578, 2649384, 2759327,
    2868415, 2976655, 3084051, 3190612, 3296344, 3401253, 3505346, 3608628,
    3711107, 3812788, 3913678, 4013783, 4113109, 4211662, 4309448, 4406473,
    4502743, 4598263, 4693041, 4787080, 4880388, 4972970, 5064832, 5155978,
    5246415, 5336149, 5425184, 5513526, 5601181, 5688153, 5774449, 5860073,
    5945031, 6029328, 6112969, 6195959, 6278303, 6360006, 6441073, 6521510,
    6601320, 6680510, 6759083, 6837044, 6914400, 6991153, 7067308, 7142871,
    7217846, 7292238, 7366051, 7439289, 7511957, 7584060, 7655602, 7726587,
    7797019, 7866904, 7936244, 8005045, 8073311, 8141045, 8208253, 8274937,
    8341102, 8406752, 8471892, 8536524, 8600654, 8664284, 8727420, 8790064,
    8852220, 8913893, 8975086, 9035803, 9096047, 9155822, 9215132, 9273981,
    9332372, 9390308, 9447793, 9504831, 9561425, 9617579, 9673296, 9728579,
    9783432, 9837858, 9891860, 9945443, 9998608, 10051360, 10103701, 10155634,
    10207164, 10258292, 10309023, 10359359, 10409303, 10458859, 10508028, 10556816,
    10605223,
};

static const int32_t turn_sine[] = {
    0, 205882, 411733, 617523, 823219, 1028791, 1234209, 1439440,
    1644455, 1849222, 2053710, 2257890, 2461729, 2665197, 2868265, 3070900,
    3273072, 3474752, 3675909, 3876512, 4076531, 4275936, 4474698, 4672785,
    4870169, 5066819, 5262706, 5457801, 5652074, 5845495, 6038037, 6229669,
    6420363, 6610090, 6798821, 6986529, 7173184, 7358759, 7543226, 7726557,
    7908725, 8089701, 8269459, 8447972, 8625213, 8801154, 8975771, 9149035,
    9320922, 9491405, 9660458, 9828057, 9994176, 10158790, 10321873, 10483403,
    10643353, 10801701, 10958422, 11113493, 11266890, 11418590, 11568571, 11716809,
    11863283, 12007971, 12150850, 12291899, 12431097, 12568423, 12703856, 12837376,
    12968963, 13098597, 13226258, 13351928, 13475586, 13597215, 13716797, 13834313,
    13949745, 14063077, 14174291, 14283370, 14390298, 14495059, 14597637, 14698017,
    14796184, 14892122, 14985817, 15077256, 15166424, 15253308, 15337895, 15420172,
    15500126, 15577747, 15653022, 15725939, 15796488, 15864658, 15930439, 15993821,
    16054795, 16113350, 16169479, 16223173, 16274424, 16323224, 16369565, 16413442,
    16454846, 16493773, 16530216, 16564169, 16595628, 16624588, 16651044, 16674992,
    16696429, 16715352, 16731757, 16745643, 16757007, 16765847, 16772163, 16775953,
    16777216,
};

static const int32_t turn_near[] = {
    16777216, 9229167, 1191094, 249492, 79751, 32758, 15814, 8540,
    5007, 3126, 2051, 1401, 989, 718, 534, 405,
    313, 246, 195, 157, 128, 105, 88, 73,
    62, 53, 45, 39, 33, 29, 25, 22,
    20, 17, 15, 14, 12, 11, 10, 9,
    8, 7, 7, 6, 5, 5, 5, 4,
    4, 4, 3, 3, 3, 3, 2, 2,
    2, 2, 2, 2, 2, 1, 1, 1,
    1,
};

static inline int32_t turn_abs(int32_t a) { return a < 0 ? -a : a; }
static inline int32_t turn_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t turn_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t turn_mul(int32_t a, int32_t b)
{
    return ((int64_t)a * b + (TURN_ONE / 2)) >> 24;
}

static int32_t turn_lookup(const int32_t *table, int32_t x)
{
    if (x >= TURN_ONE)
        return table[128];
    int n = x >> 17;
    return table[n] + (((int64_t)(table[n+1] - table[n]) *
                        (x & 0x1ffff) + 65536) >> 17);
}

static int32_t turn_release(int32_t x)
{
    unsigned halves = 0;
    while (x > TURN_ONE) {
        x >>= 1;
        halves++;
    }
    int32_t q = turn_lookup(turn_release_table, x);
    /* Correct the curvature between exponential samples. Small gain errors
     * accumulate while the carrier follows a rapidly advancing target. */
    int32_t phase = (x & 0x1ffff) >> 5;
    int32_t curve = (phase * (4096-phase) + 16384) >> 15;
    q += turn_mul(TURN_ONE-q, curve);
    while (halves--)
        q += turn_mul(q, TURN_ONE - q);
    return q;
}

static inline int turn_center(const struct turn_row *r)
{
    return (r->p + 32768) >> 16;
}

static void turn_reset(struct turn_row *r, int selected, int count)
{
    memset(r, 0, sizeof(*r));
    r->count = count;
    r->target = selected;
    r->p = (int64_t)selected * 65536;
    r->age = TURN_ONE;
    r->hold = TURN_HOLD;
    r->far[0] = (struct turn_hinge){ -TURN_ONE, TURN_ONE };
    r->far[1] = (struct turn_hinge){ TURN_ONE, TURN_ONE };
    r->stamp = 1;
    for (int s = 0; s < TURN_LOCAL_RING; s++)
        r->local_slot[s].id = -1;
    for (int s = 0; s < TURN_RING; s++)
        r->slot[s].id = -1;
    for (int i = turn_max(0, selected - TURN_HISTORY_RADIUS);
         i <= turn_min(count-1, selected + TURN_HISTORY_RADIUS); i++) {
        struct turn_slot *s = &r->slot[i % TURN_RING];
        s->id = i;
        s->stamp = r->stamp;
        if (i != selected)
            s->hinge = r->far[i > selected];
    }
}

static inline int64_t turn_error(const struct turn_row *r)
{
    return (int64_t)r->target * 65536 - r->p;
}

static int32_t turn_integral(int32_t age, int32_t hold)
{
    int32_t x = turn_max(0, age - hold);
    if (x >= TURN_WINDOW)
        return x - TURN_WINDOW/2;
    return turn_mul(turn_mul(x, TURN_INV_WINDOW), x) / 2;
}

static void turn_target(struct turn_row *r, int target)
{
    if (!r->count)
        return;
    target = turn_min(r->count - 1, turn_max(0, target));
    if (r->target == target)
        return;
    int64_t error = turn_error(r);
    if (error < 0)
        error = -error;
    r->weight = error < 65536 ? (int32_t)error * 256 : TURN_ONE;
    r->hold = TURN_HOLD + TURN_EXTRA * turn_min(4, turn_abs(target-r->target)-1);
    r->target = target;
    r->shock += turn_mul(TURN_IMPULSE, r->weight);
    r->bank_shock += turn_mul(TURN_BANK_IMPULSE, r->weight);
    /* Average the interrupted carrier gain over the last accepted interval.
     * The interrupted part uses an offset proportional to the distance at
     * acceptance. Both speed laws decrease with the remaining distance. */
    int32_t gap = r->age;
    /* Blend two wheel intervals through the hold boundary. History fades
     * over 1/128 second at either end of its half-second validity. */
    int32_t history = r->previous_gap;
    if (history && gap > r->hold) {
        int32_t edge = turn_min(turn_min(history, gap - r->hold),
                                TURN_ONE/2 - turn_max(gap, history));
        int32_t confidence = turn_min(TURN_ONE, turn_max(0, edge) * 128);
        int32_t forecast = turn_max(r->hold + TURN_WINDOW, (gap + history)/2);
        gap += turn_mul(confidence, forecast - gap);
    }
    r->previous_gap = r->age;
    int32_t rate;
    if (gap) {
        int32_t rec = turn_integral(gap, r->hold);
        int32_t gain = turn_mul(TURN_FREE, gap + turn_mul(r->weight, rec-gap))
                     + turn_mul(r->shock, turn_release(turn_mul(gap, TURN_INV_TAU)));
        rate = (int64_t)gain * TURN_ONE / gap;
    } else {
        rate = turn_mul(TURN_FREE, TURN_ONE-r->weight) + turn_mul(r->shock, TURN_INV_TAU);
    }
    error = turn_error(r);
    int64_t distance = error < 0 ? -error : error;
    r->carrier_offset = (TURN_TAIL >> 8) + ((distance * r->weight) >> 24);
    r->carrier_rate = (int64_t)rate * TURN_ONE / (TURN_ONE + r->weight);
    r->age = 0;
}

/* The local goal equals the bank goal throughout most of the visible bank. */
static void turn_hinge_step(struct turn_hinge *h, int32_t local, int32_t bank,
                            int32_t c, int32_t b)
{
    int32_t total = c + b;
    if (!total)
        return;
    int32_t g = bank, depth = turn_abs(bank);
    if (c && local != bank) {
        g += (int64_t)c * (local - bank) / total;
        depth += (int64_t)c * (turn_abs(local) - depth) / total;
    }
    int32_t dg = g - h->g, db = depth - h->b;
    int32_t d = turn_max(turn_abs(dg), turn_abs(db));
    if (!d)
        return;
    int32_t travel = turn_mul(turn_release(total), d + TURN_TAIL);
    if (travel >= d) {
        h->g = g;
        h->b = depth;
    } else {
        h->g += turn_abs(dg) == d ? (dg < 0 ? -travel : travel) :
                (int32_t)((int64_t)travel * dg / d);
        h->b += turn_abs(db) == d ? (db < 0 ? -travel : travel) :
                (int32_t)((int64_t)travel * db / d);
    }
}

static int32_t turn_clip_goal(int64_t q16)
{
    if (q16 <= -65536) return -TURN_ONE;
    if (q16 >= 65536) return TURN_ONE;
    return (int32_t)q16 * 256;
}

static void turn_step_slot(struct turn_row *r, int i, struct turn_slot *s,
                           const struct turn_slot *other, int center,
                           const int32_t banks[2], int32_t local_rate,
                           int32_t common)
{
    if (s->id != i || s->stamp != r->stamp - 1) {
        s->id = i;
        s->hinge = other->id == i && other->stamp == r->stamp - 1
                   ? other->hinge : r->far[i > center];
    }
    s->stamp = r->stamp;
    int u = i - r->target;
    int32_t goal = u == 0 ? 0 : (u < 0 ? -TURN_ONE : TURN_ONE);
    int64_t rel = (int64_t)i * 65536 - r->p;
    int32_t bank = rel < -(TURN_FIELD >> 8) ? banks[0] :
                   rel > (TURN_FIELD >> 8) ? banks[1] : goal;
    if (goal == bank && s->hinge.g == goal && s->hinge.b == turn_abs(goal))
        return;
    int a = turn_abs(u);
    int32_t n = a < 65 ? turn_near[a] : 0;
    turn_hinge_step(&s->hinge, goal, bank, turn_mul(local_rate, n),
                    turn_mul(common, TURN_ONE-n));
}

static void turn_step(struct turn_row *r, int32_t dt)
{
    int32_t z = turn_release(turn_mul(dt, TURN_INV_TAU));
    int32_t zb = turn_release(turn_mul(dt, TURN_INV_BANK_TAU));
    int32_t fast = turn_mul(r->shock, z);
    int32_t bank_fast = turn_mul(r->bank_shock, zb);
    int32_t rec = turn_integral(r->age + dt, r->hold) -
                  turn_integral(r->age, r->hold);
    r->age = turn_min(TURN_ONE, r->age + dt);
    int32_t free_gain = turn_mul(TURN_FREE, dt + turn_mul(r->weight, rec-dt));
    r->shock -= fast;
    r->bank_shock -= bank_fast;
    int64_t error = turn_error(r);
    int64_t distance = error < 0 ? -error : error;
    int32_t fraction = turn_release(turn_mul(TURN_FREE, dt));
    int64_t travel = ((distance + (TURN_TAIL >> 8)) * fraction + TURN_ONE/2) >> 24;
    if (r->weight) {
        fraction = turn_release(turn_mul(r->carrier_rate, dt));
        int64_t limited = ((distance + r->carrier_offset) * fraction + TURN_ONE/2) >> 24;
        if (limited < travel)
            travel = limited;
    }
    if (travel > distance)
        travel = distance;
    int previous_center = turn_center(r);
    r->p += error < 0 ? -travel : travel;
    int center = turn_center(r);
    int32_t common = turn_mul(free_gain + bank_fast, TURN_BANK);
    int32_t local_rate = turn_mul(dt, TURN_LOCAL);
    int64_t carrier = -turn_error(r);
    int32_t banks[2] = { turn_clip_goal(carrier - (TURN_FIELD >> 8)),
                         turn_clip_goal(carrier + (TURN_FIELD >> 8)) };
    /* Retain the history between the carrier and target during long sweeps.
     * A distant target has its own small window until the carrier catches up. */
    int first = turn_min(center-TURN_HISTORY_RADIUS, r->target-TURN_LOCAL_RADIUS);
    int last = turn_max(center+TURN_HISTORY_RADIUS, r->target+TURN_LOCAL_RADIUS);
    first = turn_max(first, center+TURN_HISTORY_RADIUS+1-TURN_RING);
    last = turn_min(last, first+TURN_RING-1);
    first = turn_max(0, first);
    last = turn_min(r->count-1, last);
    r->stamp++;
    for (int i = first; i <= last; i++)
        turn_step_slot(r, i, &r->slot[i % TURN_RING],
                       &r->local_slot[i % TURN_LOCAL_RING], previous_center,
                       banks, local_rate, common);
    for (int i = turn_max(0, r->target-TURN_LOCAL_RADIUS);
         i <= turn_min(r->count-1, r->target+TURN_LOCAL_RADIUS); i++)
        if (i < first || i > last)
            turn_step_slot(r, i, &r->local_slot[i % TURN_LOCAL_RING],
                           &r->slot[i % TURN_RING], previous_center,
                           banks, local_rate, common);
    for (int j = 0; j < 2; j++)
        turn_hinge_step(&r->far[j], banks[j], banks[j], 0, common);
}

static bool turn_moving(const struct turn_row *r)
{
    if (turn_error(r))
        return true;
    for (int i = turn_max(0, r->target-TURN_HISTORY_RADIUS);
         i <= turn_min(r->count-1, r->target+TURN_HISTORY_RADIUS); i++) {
        int g = i == r->target ? 0 : (i < r->target ? -TURN_ONE : TURN_ONE);
        const struct turn_hinge *h = &r->slot[i % TURN_RING].hinge;
        if (h->g != g || h->b != turn_abs(g))
            return true;
    }
    return r->far[0].g != -TURN_ONE || r->far[1].g != TURN_ONE ||
           r->far[0].b != TURN_ONE || r->far[1].b != TURN_ONE;
}

static void turn_advance(struct turn_row *r, unsigned elapsed_us)
{
    /* Convert once so a two-tick interval has the same total elapsed time. */
    int64_t left = ((int64_t)elapsed_us * TURN_ONE + 500000) / 1000000;
    const int nominal = (TURN_ONE + 30)/60;
    /* Divide a missed TE evenly: each half then follows a full-rate step.
     * A 60 Hz period rounds to 16667 when sampled in whole microseconds. */
    const int bound = (INT64_C(16667) * TURN_ONE + 500000) / 1000000;
    int steps = left <= bound ? 1 : left <= 2*bound ? 2 : (left+bound-1)/bound;
    int32_t dt = steps == 1 ? left : steps == 2 ? left >> 1 : left / steps;
    int short_steps = steps - (left - (int64_t)steps * dt);
    while (left > 0) {
        if (left > 8*nominal && !turn_moving(r)) {
            /* A stationary row can skip intermediate poses after suspension. */
            int t = left > TURN_ONE * 4 ? TURN_ONE * 4 : (int32_t)left;
            int q = TURN_ONE - turn_release(turn_mul(t, TURN_INV_TAU));
            int qb = TURN_ONE - turn_release(turn_mul(t, TURN_INV_BANK_TAU));
            r->shock = turn_mul(r->shock, q);
            r->bank_shock = turn_mul(r->bank_shock, qb);
            r->age = turn_min(TURN_ONE, r->age + t);
            break;
        }
        int32_t step = dt + (short_steps-- <= 0);
        turn_step(r, step);
        left -= step;
    }
}

struct turn_segment {
    int32_t x[2], z[2];
};

#define TURN_CLEARANCE 84 /* .00128 world unit in Q16 */

/* Q24 quarter-turns: acos(cos(pi*u/2)^2) / (pi/2). The two opening cards
 * have complementary horizontal half-widths and fit inside one stock arm. */
static const int32_t turn_aperture[129] = {
    0, 185361, 370709, 556029, 741306, 926528, 1111680, 1296748,
    1481717, 1666575, 1851306, 2035896, 2220330, 2404596, 2588677, 2772559,
    2956228, 3139668, 3322865, 3505804, 3688469, 3870844, 4052915, 4234666,
    4416081, 4597143, 4777837, 4958147, 5138054, 5317544, 5496598, 5675200,
    5853331, 6030975, 6208112, 6384726, 6560797, 6736306, 6911235, 7085564,
    7259273, 7432342, 7604751, 7776479, 7947505, 8117808, 8287366, 8456157,
    8624158, 8791347, 8957699, 9123192, 9287802, 9451503, 9614271, 9776081,
    9936906, 10096720, 10255497, 10413209, 10569828, 10725327, 10879677, 11032848,
    11184811, 11335535, 11484991, 11633147, 11779971, 11925431, 12069495, 12212129,
    12353300, 12492974, 12631116, 12767691, 12902663, 13035997, 13167656, 13297603,
    13425801, 13552212, 13676799, 13799522, 13920343, 14039223, 14156123, 14271003,
    14383824, 14494544, 14603125, 14709525, 14813706, 14915625, 15015244, 15112521,
    15207417, 15299892, 15389906, 15477419, 15562394, 15644791, 15724571, 15801699,
    15876136, 15947846, 16016794, 16082946, 16146267, 16206725, 16264288, 16318925,
    16370608, 16419307, 16464996, 16507650, 16547244, 16583757, 16617166, 16647454,
    16674601, 16698593, 16719414, 16737054, 16751501, 16762746, 16770783, 16775608,
    16777216,
};

#define TURN_PITCH 1820328 /* 0.217 S in Q16 world coordinates */
#define TURN_ARM 4806672 /* 0.573 S */
#define TURN_CONTACT_DIAMETER 838864 /* 0.10 S, rounded up */

static void turn_fit_side(const int32_t half[], int count, int direction,
                          const int32_t wanted[], int32_t fitted[])
{
    int first = direction > 0 ? 0 : count - 1;
    int32_t far_edge = INT32_MIN / 2;
    for (int j = 0; j < count; j++) {
        int i = first + direction * j;
        int prev = i - direction;
        int32_t x = direction * wanted[i];
        if (j >= 1) {
            /* Only the outer tenth of both half-widths can overlap. */
            int32_t gap = half[i] + half[prev] - turn_min(half[i], half[prev]) / 10;
            x = turn_max(x, direction * fitted[prev] + gap + TURN_CLEARANCE);
        }
        if (j >= 2) {
            /* A disk touching three cards must span two non-neighbours.
             * Keep their intervals over one disk diameter apart, even at 90°. */
            int older = prev - direction;
            far_edge = turn_max(far_edge, direction * fitted[older] + half[older]);
            x = turn_max(x, far_edge + half[i] + TURN_CONTACT_DIAMETER + 16);
        }
        fitted[i] = direction * x;
    }
}

static int turn_segments(const struct turn_row *r, struct turn_segment seg[TURN_CARDS],
                         int *first)
{
    int center = turn_center(r);
    *first = turn_max(0, center - TURN_RADIUS);
    int count = turn_min(r->count - 1, center + TURN_RADIUS) - *first + 1;
    if (count <= 0)
        return 0;

    int32_t fan[TURN_CARDS], half[TURN_CARDS], track[TURN_CARDS], wanted[TURN_CARDS];
    int32_t low[TURN_CARDS], high[TURN_CARDS], left[TURN_CARDS], right[TURN_CARDS];
    int32_t arm = TURN_ARM + r->margin_delta * 65536;
    int32_t reach = turn_max(TURN_ARM, arm);
    int32_t bank = (r->far[0].g + r->far[1].g) / 2;
    int32_t local_p = r->p - (int64_t)*first * 65536;
    for (int i = 0; i < count; i++) {
        int32_t d = i * 65536 - local_p;
        int32_t phase = turn_min(TURN_ONE, turn_max(-TURN_ONE, 256*d + bank));
        int32_t angle = turn_lookup(turn_aperture, turn_abs(phase));
        fan[i] = phase < 0 ? -angle : angle;
        int32_t native = r->slot[(*first+i) % TURN_RING].hinge.g;
        /* Certify the widest pose of the whole yaw interpolation. */
        int32_t least = (fan[i] < 0) == (native < 0) ? turn_min(angle, turn_abs(native)) : 0;
        half[i] = (turn_lookup(turn_sine, TURN_ONE - least) + 2) >> 2;
        track[i] = (int64_t)d * TURN_PITCH >> 16;
        wanted[i] = track[i] + turn_mul(native, arm);
        low[i] = turn_max(track[i] - reach, wanted[i] - TURN_PITCH);
        high[i] = turn_min(track[i] + reach, wanted[i] + TURN_PITCH);
    }

    turn_fit_side(half, count, 1, low, left);
    /* Recover native yaw continuously as two world units of slack open. */
    int32_t room = 2 * 65536;
    for (int i = 0; i < count; i++)
        room = turn_min(room, high[i] - left[i]);
    int32_t restore = turn_max(0, room) * 128;
    for (int i = 0; i < count; i++) {
        const struct turn_hinge *h = &r->slot[(*first+i) % TURN_RING].hinge;
        int32_t yaw = fan[i] + turn_mul(h->g - fan[i], restore);
        int32_t a = turn_abs(yaw);
        int32_t hx = (turn_lookup(turn_sine, TURN_ONE - a) + 2) >> 2;
        int32_t hz = (turn_lookup(turn_sine, a) + 2) >> 2;
        int32_t z = (156 << 16) + turn_mul(h->b, 3883926);
        if (yaw > 0)
            hz = -hz;
        seg[i] = (struct turn_segment){{-hx, hx}, {z - hz, z + hz}};
        half[i] = hx;
        low[i] = track[i] - reach;
        high[i] = track[i] + reach;
    }

    /* Both directional fits satisfy the same linear spacing constraints.
     * Their midpoint shares each correction and preserves feasible native X. */
    turn_fit_side(half, count, 1, low, left);
    turn_fit_side(half, count, -1, high, right);
    for (int i = 0; i < count; i++)
        wanted[i] = turn_max(left[i], turn_min(right[i], wanted[i]));
    turn_fit_side(half, count, 1, wanted, left);
    turn_fit_side(half, count, -1, wanted, right);
    for (int i = 0; i < count; i++) {
        int32_t x = (left[i] + right[i]) / 2;
        seg[i].x[0] += x;
        seg[i].x[1] += x;
    }
    return count;
}

/* For d normalized to [2^31,2^32), m approximates 2^62/d. Bin midpoints
 * seed two fixed-point Newton iterations. Callers correct the final quotient
 * against the original integer operands. */
static const uint16_t turn_recip_seed[128] = {
    65281, 64777, 64281, 63792, 63310, 62836, 62369, 61909, 61455, 61008, 60568, 60133,
    59705, 59283, 58867, 58457, 58053, 57654, 57260, 56872, 56489, 56111, 55738, 55370,
    55007, 54649, 54295, 53946, 53601, 53261, 52925, 52593, 52265, 51942, 51622, 51306,
    50995, 50686, 50382, 50081, 49784, 49490, 49200, 48913, 48630, 48349, 48072, 47798,
    47528, 47260, 46995, 46733, 46474, 46218, 45965, 45714, 45467, 45222, 44979, 44739,
    44502, 44267, 44035, 43805, 43577, 43352, 43129, 42908, 42690, 42474, 42260, 42048,
    41838, 41631, 41425, 41222, 41020, 40820, 40623, 40427, 40233, 40041, 39851, 39662,
    39476, 39291, 39108, 38926, 38746, 38568, 38392, 38217, 38044, 37872, 37702, 37533,
    37366, 37200, 37036, 36873, 36712, 36552, 36393, 36236, 36080, 35926, 35772, 35620,
    35470, 35320, 35172, 35026, 34880, 34735, 34592, 34450, 34309, 34169, 34031, 33893,
    33757, 33622, 33487, 33354, 33222, 33091, 32961, 32832,
};

struct turn_recip {
    uint32_t m;
    unsigned norm;
};

static inline struct turn_recip turn_reciprocal(uint32_t denominator)
{
    unsigned norm = __builtin_clz(denominator);
    uint32_t d = denominator << norm;
    uint32_t m = (uint32_t)turn_recip_seed[(d >> 24)-128] << 15;
    for (int i = 0; i < 2; i++) {
        int32_t error = (1 << 30) - (uint32_t)((uint64_t)d*m >> 32);
        m += (int32_t)((int64_t)m*error >> 30);
    }
    return (struct turn_recip){ m, norm };
}

/* The two edges of a fully turned, fully recessed cover share their camera
 * scales with the rest of that bank, regardless of the direction of travel. */
static int32_t turn_camera_scale(int32_t z)
{
    const int32_t near = ((156 - 64) << 16) + 3883926;
    const int32_t far = ((156 + 64) << 16) + 3883926;
    const int64_t focal = INT64_C(156) << 40;
    if (z == near)
        return (focal + near/2) / near;
    if (z == far)
        return (focal + far/2) / far;
    struct turn_recip r = turn_reciprocal(z);
    uint32_t guess = ((uint64_t)156 * r.m) >> (22-r.norm);
    int32_t rem = (uint32_t)(focal + z/2) - guess*(uint32_t)z;
    return guess + (rem >= z) - (rem < 0);
}

static int turn_project(const struct turn_row *r, struct turn_card out[TURN_CARDS])
{
    static struct turn_segment seg[TURN_CARDS];
    int first;
    int count = turn_segments(r, seg, &first);
    for (int n = 0; n < count; n++) {
        struct turn_card *c = &out[n];
        c->id = first+n;
        for (int j = 0; j < 2; j++) {
            int32_t z = seg[n].z[j];
            /* Q24 preserves reciprocal-depth ordering at near-coincident edges. */
            int32_t scale = turn_camera_scale(z);
            c->z[j] = z;
            c->world_x[j] = seg[n].x[j];
            c->x[j] = (160 << 16) + turn_mul(seg[n].x[j], scale);
            c->top[j] = (120 << 16) + turn_mul(-5175771, scale);
            c->bottom[j] = (120 << 16) + turn_mul(3212837, scale);
        }
    }
    return count;
}

#endif
