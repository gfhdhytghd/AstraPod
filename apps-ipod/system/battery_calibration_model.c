/* iPod Classic battery curves and calibration. GPL version 2 or later. */
#include "battery_calibration_model.h"
#include "battery-curve-6g.h"

#define MIN_CHARGE_S (90 * 60)
#define PLATEAU_S (10 * 60)
#define PLATEAU_MIN_SAMPLES 61
#define PLATEAU_SPREAD_MV 12
#define RELAX_START_S 120
#define RELAX_END_S 180
#define RELAX_MIN_SAMPLES 7
#define RELAX_SPREAD_MV 20
#define RELAX_MAX_DROP_MV 200
#define RELAX_MAX_RISE_MV 12
#define FULL_HYSTERESIS_MV 12

bool battery_cal_valid_mv(int mv)
{
    return mv >= BATTERY_CAL_MIN_MV && mv <= BATTERY_CAL_MAX_MV;
}

int battery_cal_smooth(int previous, int mv)
{
    return previous ? (previous + mv + 1) / 2 : mv;
}

int battery_cal_full_mv(int reference, bool charging)
{
    if (!reference)
        return BATTERY_STOCK_FULL_MV;
    return charging ? reference : reference - BATTERY_CAL_FULL_MARGIN_MV;
}

static int scale_voltage(int mv, int full_mv)
{
    const int empty = BATTERY_STOCK_EMPTY_MV;
    const int span = BATTERY_STOCK_FULL_MV - empty;
    return empty + ((mv - empty) * (full_mv - empty) + span / 2) / span;
}

void battery_cal_curve(unsigned short table[11], int full_mv)
{
    static const unsigned short stock[] = { BATTERY_STOCK_DECILES };
    for (unsigned i = 0; i < sizeof(stock) / sizeof(stock[0]); i++)
        table[i] = scale_voltage(stock[i], full_mv);
}

int battery_cal_stock_level(int mv, int full_mv)
{
    static const unsigned short stock[] = {
        BATTERY_STOCK_EMPTY_MV, 3659, 3681, 3716, 3737, 3753, 3767, 3782,
        3798, 3815, 3862, 3905, 3941, 3964, BATTERY_STOCK_FULL_MV
    };
    int n = 0;
    while (n < (int)(sizeof(stock) / sizeof(stock[0])) &&
           mv >= scale_voltage(stock[n], full_mv))
        n++;
    return n;
}

bool battery_cal_full(int mv, int full_mv, bool was_full)
{
    return mv >= full_mv - (was_full ? FULL_HYSTERESIS_MV : 0);
}

static enum battery_cal_event on_charger(struct battery_cal_trace *trace,
    uint32_t now, bool eligible, int mv, struct battery_cal_sample *result)
{
    if (!eligible)
    {
        trace->count = 0;
        trace->plateau_mv = 0;
        return BATTERY_CAL_NONE;
    }

    int low = trace->min_mv < mv ? trace->min_mv : mv;
    int high = trace->max_mv > mv ? trace->max_mv : mv;
    if (!trace->count || high - low > PLATEAU_SPREAD_MV)
    {
        trace->plateau_at = now;
        trace->min_mv = trace->max_mv = mv;
        trace->sum_mv = mv;
        trace->count = 1;
        trace->plateau_mv = 0;
    }
    else
    {
        trace->min_mv = low;
        trace->max_mv = high;
        if (!trace->plateau_mv)
        {
            trace->sum_mv += mv;
            trace->count++;
        }
    }

    if (trace->plateau_mv || now - trace->plug_at < MIN_CHARGE_S ||
        now - trace->plateau_at < PLATEAU_S ||
        trace->count < PLATEAU_MIN_SAMPLES)
        return BATTERY_CAL_NONE;

    trace->plateau_mv = (trace->sum_mv + trace->count / 2) / trace->count;
    if (trace->charge_recorded)
        return BATTERY_CAL_NONE;
    trace->charge_recorded = true;
    *result = (struct battery_cal_sample){ trace->plateau_mv, trace->count };
    return BATTERY_CAL_CHARGE;
}

static enum battery_cal_event on_battery(struct battery_cal_trace *trace,
    uint32_t now, bool eligible, int mv, struct battery_cal_sample *result)
{
    uint32_t age = now - trace->unplug_at;
    if (!trace->plateau_mv || age < RELAX_START_S)
        return BATTERY_CAL_NONE;

    unsigned count = trace->relaxed_count;
    if (eligible && age <= RELAX_END_S &&
        mv <= trace->plateau_mv + RELAX_MAX_RISE_MV &&
        mv >= trace->plateau_mv - RELAX_MAX_DROP_MV &&
        count < sizeof(trace->relaxed) / sizeof(trace->relaxed[0]))
    {
        unsigned i = count;
        while (i > 0 && trace->relaxed[i - 1] > mv)
        {
            trace->relaxed[i] = trace->relaxed[i - 1];
            i--;
        }
        trace->relaxed[i] = mv;
        trace->relaxed_count = ++count;
    }

    if (age < RELAX_END_S)
        return BATTERY_CAL_NONE;
    trace->plateau_mv = 0;
    if (count < RELAX_MIN_SAMPLES ||
        trace->relaxed[count - 1] - trace->relaxed[0] > RELAX_SPREAD_MV)
        return BATTERY_CAL_NONE;

    *result = (struct battery_cal_sample){
        (trace->relaxed[(count - 1) / 2] + trace->relaxed[count / 2] + 1) / 2,
        count
    };
    return BATTERY_CAL_DISCHARGE;
}

enum battery_cal_event battery_cal_step(struct battery_cal_trace *trace,
    uint32_t now, enum battery_supply supply, bool eligible, int mv,
    struct battery_cal_sample *result)
{
    if (!trace->sampling || now - trace->last > BATTERY_CAL_MAX_GAP_S ||
        supply == BATTERY_OTHER_POWER ||
        (supply != trace->supply && supply == BATTERY_ON_CHARGER))
    {
        *trace = (struct battery_cal_trace){
            .sampling = true, .supply = supply, .plug_at = now,
        };
    }
    else if (supply != trace->supply)
    {
        trace->supply = supply;
        trace->unplug_at = now;
    }
    trace->last = now;
    eligible = eligible && battery_cal_valid_mv(mv);

    if (supply == BATTERY_ON_CHARGER)
        return on_charger(trace, now, eligible, mv, result);
    return on_battery(trace, now, eligible, mv, result);
}
