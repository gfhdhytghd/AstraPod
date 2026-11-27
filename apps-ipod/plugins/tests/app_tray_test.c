/* AstraPod app reminder scheduler and disk store tests. GPL v2+. */
#include "plugins/app_tray.h"
#include "crc32.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char app[] = "/rocks/games/Game.rock";
static const struct rock_tray_icon12 frames[] = {{{0x11}}, {{0x22}}, {{0x44}}};
static const struct rock_tray_icon12 changed[] = {{{0x88}}, {{0x99}}, {{0xaa}}};
static const struct rock_tray_bitmap small[] = {{{0xff, 0, 0, 0, 0, 0, 0, 0x81}}};
static const struct rock_tray_rule schedule[] = {
    {30, 1}, {60, 2}, {90, ROCK_TRAY_HIDDEN},
};

static struct tm when(int month, int day, int hour, int minute, int sec)
{
    return (struct tm){
        .tm_year = 124, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_sec = sec,
    };
}

int main(void)
{
    struct tm start = when(2, 29, 23, 59, 45);
    struct tm before = when(3, 1, 0, 0, 14);
    struct tm first = when(3, 1, 0, 0, 15);
    struct tm second = when(3, 1, 0, 0, 45);
    struct tm end = when(3, 1, 0, 1, 15);
    struct tm invalid = {.tm_year = 70};
    struct app_tray_view view;

    app_tray_init();
    assert(app_tray_get(app, &start) == -1);
    assert(!app_tray_register("two.parts.rock", frames, 3));
    assert(!app_tray_register(app, frames, ROCK_TRAY_MAX_FRAMES + 1));
    assert(app_tray_register(app, frames, 3));
    assert(app_tray_get(app, &start) == ROCK_TRAY_HIDDEN);
    assert(!app_tray_set(app, 256));
    assert(app_tray_set(app, 0));
    assert(!app_background_schedule(app, schedule, 3, &invalid));

    struct rock_tray_rule bad[] = {{30, 1}, {59, 2}};
    assert(!app_background_schedule(app, bad, 2, &start));
    bad[1] = (struct rock_tray_rule){1, 2};
    assert(!app_background_schedule(app, bad, 2, &start));
    bad[1] = (struct rock_tray_rule){60, 3};
    assert(!app_background_schedule(app, bad, 2, &start));
    assert(app_background_schedule(app, schedule, 3, &start));

    assert(app_tray_get(app, &before) == 0);
    assert(app_tray_get(app, &first) == 1);
    assert(app_tray_get(app, &second) == 2);
    assert(app_tray_get(app, &end) == ROCK_TRAY_HIDDEN);
    assert(app_tray_get(app, &invalid) == 0);
    assert(app_tray_get(app, &start) == 0);

    assert(!app_tray_register(app, changed, 2));
    assert(app_tray_register(app, changed, 3));
    app_tray_init();
    assert(app_tray_get("/other/game.rock", &first) == 1);
    app_tray_snapshot(&second, &view);
    assert(view.count == 1 && view.icons[0].columns[0] == 0xaa);

    assert(app_tray_register("other.rock", frames, 3));
    assert(app_tray_register("third.rock", frames, 3));
    assert(app_tray_register("fourth.rock", frames, 3));
    assert(!app_tray_register("overflow.rock", frames, 3));
    assert(app_tray_set("other.rock", 1));
    assert(app_tray_set("third.rock", 2));
    app_tray_snapshot(&second, &view);
    assert(view.count == 2 && view.icons[0].columns[0] == 0xaa &&
           view.icons[1].columns[0] == 0x22);
    app_tray_snapshot(&end, &view);
    assert(view.count == 2 && view.icons[0].columns[0] == 0x22 &&
           view.icons[1].columns[0] == 0x44);

    assert(app_background_schedule(app, NULL, 0, &second));
    assert(app_tray_get(app, &end) == 2);
    assert(app_background_schedule(app, schedule, 3, &start));
    assert(app_tray_set(app, ROCK_TRAY_HIDDEN));
    app_tray_init();
    assert(app_tray_get(app, &first) == ROCK_TRAY_HIDDEN);

    assert(mkdir("app-tray.dat.tmp", 0700) == 0);
    assert(!app_tray_set(app, 1));
    assert(app_tray_get(app, &end) == ROCK_TRAY_HIDDEN);
    app_tray_init();
    assert(app_tray_get(app, &end) == ROCK_TRAY_HIDDEN);
    assert(rmdir("app-tray.dat.tmp") == 0);

    assert(app_tray_register(app, NULL, 0));
    assert(app_tray_register("overflow.rock", frames, 3));
    app_tray_init();
    assert(app_tray_get(app, &end) == -1);
    assert(app_tray_get("overflow.rock", &end) == ROCK_TRAY_HIDDEN);

    int fd = open("app-tray.dat", O_WRONLY);
    assert(fd >= 0 && pwrite(fd, "X", 1, 12) == 1 && close(fd) == 0);
    app_tray_init();
    app_tray_snapshot(&end, &view);
    assert(view.count == 0 && app_tray_get("other.rock", &end) == -1);
    assert(app_tray_register(app, frames, 3));
    assert(truncate("app-tray.dat", 10) == 0);
    app_tray_init();
    assert(app_tray_get(app, &end) == -1);

    /* An 8 x 8 frame is centred in 12 x 12. */
    assert(!app_tray_register8(app, small, ROCK_TRAY_MAX_FRAMES + 1));
    assert(app_tray_register8(app, small, 1) && app_tray_set(app, 0));
    app_tray_snapshot(&end, &view);
    assert(view.count == 1 && view.icons[0].columns[0] == 0 &&
           view.icons[0].columns[2] == 0x3fc &&
           view.icons[0].columns[9] == 0x204 &&
           view.icons[0].columns[11] == 0);
    assert(app_tray_register8(app, NULL, 0));

    /* Bits past the 12th row are dropped. */
    const struct rock_tray_icon12 tall[] = {{{0xf801}}};
    assert(app_tray_register(app, tall, 1) && app_tray_set(app, 0));
    app_tray_snapshot(&end, &view);
    assert(view.count == 1 && view.icons[0].columns[0] == 0x801);
    assert(app_tray_register(app, NULL, 0));

    /* A V1 file, 8 x 8 frames, is read and widened. */
    unsigned char v1[380] = "ASTRY01";
    unsigned char *slot = v1 + 8;
    memcpy(slot, "old", 3);
    slot[32 + 4 + 16] = 0x0f;            /* frames[0].columns[0] */
    slot[32 + 4 + 16 + 8 + 7] = 0x80;    /* frames[1].columns[7] */
    slot[32 + 4 + 16 + 32 + 4] = 2;      /* count */
    slot[32 + 4 + 16 + 32 + 4 + 1] = 1;  /* frame */
    uint32_t crc = crc_32(v1, sizeof(v1) - 4, 0xffffffffu);
    memcpy(v1 + sizeof(v1) - 4, &crc, 4);
    fd = open("app-tray.dat", O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, v1, sizeof(v1)) == sizeof(v1) &&
           close(fd) == 0);
    app_tray_init();
    assert(app_tray_get("old.rock", &end) == 1);
    app_tray_snapshot(&end, &view);
    assert(view.count == 1 && view.icons[0].columns[9] == 0x200 &&
           view.icons[0].columns[2] == 0);
    assert(app_tray_set("old.rock", 0));
    app_tray_init();
    app_tray_snapshot(&end, &view);
    assert(view.count == 1 && view.icons[0].columns[2] == 0x3c);
    v1[20] ^= 1;
    fd = open("app-tray.dat", O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, v1, sizeof(v1)) == sizeof(v1) &&
           close(fd) == 0);
    app_tray_init();
    assert(app_tray_get("old.rock", &end) == -1);
    puts("PASS tray deadlines, snapshots, slot limits, reload, failed saves, "
         "8 x 8 frames, V1 files");
}
