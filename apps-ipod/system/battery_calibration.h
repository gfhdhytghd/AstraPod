/* iPod Classic battery calibration. GPL version 2 or later. */
#ifndef BATTERY_CALIBRATION_H
#define BATTERY_CALIBRATION_H

#include <stdbool.h>

struct battery_cal_reading {
    unsigned mv, recorded_at, samples;
};

struct battery_cal_status {
    struct battery_cal_reading charge, discharge;
    unsigned charge_full_mv, discharge_full_mv;
    bool user_override;
};

void battery_calibration_init(void);
void battery_calibration_poll(void);
bool battery_calibration_reset(void);
void battery_calibration_status(struct battery_cal_status *status);
int battery_calibration_display(int percent);
int battery_calibration_icon_level(int percent);

#endif
