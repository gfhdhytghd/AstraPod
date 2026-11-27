/* iPod Classic battery calibration. GPL version 2 or later. */
#include <string.h>
#include "config.h"
#include "audio.h"
#include "backlight.h"
#include "file.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "storage.h"
#include "system.h"
#include "timefuncs.h"
#include "battery_calibration.h"
#include "battery_calibration_model.h"

#define CAL_FILE ROCKBOX_DIR "/battery-calibration.bin"
#define CAL_TEMP CAL_FILE ".tmp"
#define CAL_VERSION 1

extern unsigned short percent_to_volt_charge[11];
extern unsigned short percent_to_volt_discharge[11];

struct battery_cal_record {
    unsigned version;
    struct battery_cal_reading charge, discharge;
};

static struct mutex calibration_lock;
static struct battery_cal_trace trace;
static struct battery_cal_record saved, pending;
static bool user_override, dirty, display_full;
static unsigned long last_poll;

static bool valid_reading(const struct battery_cal_reading *reading)
{
    if (!reading->mv)
        return !reading->recorded_at && !reading->samples;
    return battery_cal_valid_mv(reading->mv) && reading->samples > 0;
}

static void apply_record(void)
{
    unsigned short charge[11], discharge[11];
    battery_cal_curve(charge, battery_cal_full_mv(saved.charge.mv, true));
    battery_cal_curve(discharge, battery_cal_full_mv(saved.discharge.mv, false));

    /* Publish both arrays together to the 6G's power thread. */
    int oldlevel = disable_irq_save();
    memcpy(percent_to_volt_charge, charge, sizeof(charge));
    memcpy(percent_to_volt_discharge, discharge, sizeof(discharge));
    restore_irq(oldlevel);
}

static bool save_record(const struct battery_cal_record *record)
{
    int fd = open(CAL_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write(fd, record, sizeof(*record)) == sizeof(*record) &&
              fsync(fd) == 0;
    if (close(fd) == 0 && ok && rename(CAL_TEMP, CAL_FILE) == 0)
        return true;
    remove(CAL_TEMP);
    return false;
}

void battery_calibration_init(void)
{
    mutex_init(&calibration_lock);
    user_override = file_exists(BATTERY_LEVELS_USER);
    saved.version = CAL_VERSION;

    int fd = open(CAL_FILE, O_RDONLY);
    if (fd >= 0)
    {
        struct battery_cal_record record;
        if (read(fd, &record, sizeof(record)) == sizeof(record) &&
            record.version == CAL_VERSION && valid_reading(&record.charge) &&
            valid_reading(&record.discharge))
            saved = record;
        close(fd);
    }
    pending = saved;
    if (!user_override)
        apply_record();
}

static void remember_sample(struct battery_cal_reading *reading,
                            const struct battery_cal_sample *sample)
{
    struct tm now = *get_time();
    reading->mv = battery_cal_smooth(reading->mv, sample->mv);
    reading->recorded_at = valid_time(&now) ? mktime(&now) : 0;
    reading->samples = sample->count;
    dirty = true;
}

void battery_calibration_poll(void)
{
    unsigned long now = current_tick;
    if (user_override || now - last_poll < BATTERY_CAL_SAMPLE_S * HZ)
        return;
    last_poll = now;

    mutex_lock(&calibration_lock);
    enum battery_supply supply = charger_inserted() ? BATTERY_ON_CHARGER :
        power_input_present() ? BATTERY_OTHER_POWER : BATTERY_ON_BATTERY;
    unsigned int audio = audio_status();
    bool light_load = (!(audio & AUDIO_STATUS_PLAY) ||
                       (audio & AUDIO_STATUS_PAUSE)) &&
                      !(audio & (AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD)) &&
                      !is_backlight_on(true) && !storage_disk_is_active();
    bool eligible = supply == BATTERY_ON_CHARGER ? !charging_state() :
                    supply == BATTERY_ON_BATTERY && trace.plateau_mv && light_load;
    int mv = eligible ? _battery_voltage() : 0;
    struct battery_cal_sample sample;
    switch (battery_cal_step(&trace, now / HZ, supply, eligible, mv, &sample))
    {
    case BATTERY_CAL_CHARGE:
        remember_sample(&pending.charge, &sample);
        break;
    case BATTERY_CAL_DISCHARGE:
        remember_sample(&pending.discharge, &sample);
        break;
    case BATTERY_CAL_NONE:
        break;
    }
    if (dirty && save_record(&pending))
    {
        saved = pending;
        apply_record();
        dirty = false;
    }
    mutex_unlock(&calibration_lock);
}

bool battery_calibration_reset(void)
{
    const struct battery_cal_record defaults = { .version = CAL_VERSION };
    mutex_lock(&calibration_lock);
    bool ok = save_record(&defaults);
    if (ok)
    {
        saved = pending = defaults;
        trace = (struct battery_cal_trace){0};
        dirty = display_full = false;
        if (!user_override)
            apply_record();
    }
    mutex_unlock(&calibration_lock);
    return ok;
}

void battery_calibration_status(struct battery_cal_status *status)
{
    mutex_lock(&calibration_lock);
    *status = (struct battery_cal_status){
        .charge = saved.charge,
        .discharge = saved.discharge,
        .charge_full_mv = percent_to_volt_charge[10],
        .discharge_full_mv = percent_to_volt_discharge[10],
        .user_override = user_override,
    };
    mutex_unlock(&calibration_lock);
}

int battery_calibration_display(int percent)
{
    if (user_override || percent < 0)
        return percent;

    bool charging = charge_state > DISCHARGING;
    bool hysteresis = charging || saved.discharge.mv;
    int full_mv = charging ? percent_to_volt_charge[10] :
                  percent_to_volt_discharge[10];
    display_full = (charging && charger_inserted() && trace.plateau_mv) ||
        battery_cal_full(battery_voltage(), full_mv, hysteresis && display_full);
    return display_full ? 100 : MIN(percent, 99);
}

int battery_calibration_icon_level(int percent)
{
    if (user_override)
        return percent * 15 / 100;
    if (battery_calibration_display(percent) == 100)
        return 15;
    int full_mv = charge_state > DISCHARGING ? percent_to_volt_charge[10] :
                  percent_to_volt_discharge[10];
    return MIN(battery_cal_stock_level(battery_voltage(), full_mv), 14);
}
