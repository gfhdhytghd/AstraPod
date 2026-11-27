/* iPod Classic battery curves and calibration. GPL version 2 or later. */
#ifndef BATTERY_CALIBRATION_MODEL_H
#define BATTERY_CALIBRATION_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_CAL_SAMPLE_S 5
#define BATTERY_CAL_MAX_GAP_S 15
#define BATTERY_CAL_MIN_MV 3900
#define BATTERY_CAL_MAX_MV 4250
#define BATTERY_CAL_FULL_MARGIN_MV 80

enum battery_supply {
    BATTERY_ON_BATTERY,
    BATTERY_ON_CHARGER,
    BATTERY_OTHER_POWER,
};

enum battery_cal_event {
    BATTERY_CAL_NONE,
    BATTERY_CAL_CHARGE,
    BATTERY_CAL_DISCHARGE,
};

struct battery_cal_sample {
    int mv;
    unsigned count;
};

struct battery_cal_trace {
    bool sampling, charge_recorded;
    enum battery_supply supply;
    uint32_t last, plug_at, plateau_at, unplug_at;
    int plateau_mv, min_mv, max_mv, sum_mv;
    unsigned count;
    int relaxed[13];
    unsigned relaxed_count;
};

enum battery_cal_event battery_cal_step(struct battery_cal_trace *trace,
    uint32_t now, enum battery_supply supply, bool eligible, int mv,
    struct battery_cal_sample *result);

bool battery_cal_valid_mv(int mv);
int battery_cal_smooth(int previous, int mv);
int battery_cal_full_mv(int reference, bool charging);
void battery_cal_curve(unsigned short table[11], int full_mv);
int battery_cal_stock_level(int mv, int full_mv);
bool battery_cal_full(int mv, int full_mv, bool was_full);

#endif
