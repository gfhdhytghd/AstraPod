/* Animated lyrics geometry and input policy. GPL version 2 or later. */
#ifndef LYRICS_MOTION_H
#define LYRICS_MOTION_H
#include <stdbool.h>
#include <stdint.h>

/* Positions and opacities use 8 fractional bits. The legacy path uses kernel
 * ticks. With frame_clock set, at and now are USEC_TIMER readings and duration
 * is in animation frames. Unsigned subtraction handles clock wrap. */
struct lyrics_motion {
    int from, to;
    uint32_t at, duration;
    bool frame_clock;
};
struct lyrics_mode { bool manual; int focus, direction, burst; uint32_t input; };
int lyrics_ease(int phase);
int lyrics_lerp(int from, int to, int phase);
int lyrics_motion_at(const struct lyrics_motion *m, uint32_t now);
void lyrics_retarget(struct lyrics_motion *m, int to, uint32_t now, unsigned duration);
bool lyrics_moving(const struct lyrics_motion *m, uint32_t now);
int lyrics_focus_weight(int line, int position);
int lyrics_opacity(int distance, int nearest);
int lyrics_edge_opacity(int y, int height);
int lyrics_line_height(int small, int large, int weight);
int lyrics_line_y(int anchor, int fraction, int current_height, int next_height);
int lyrics_word_progress(long elapsed, long start, long end, int width);
int lyrics_progress_ticks(long elapsed, long length, int width, int hz);
int lyrics_wheel_step(struct lyrics_mode *m, int direction, uint32_t now, int hz);
bool lyrics_mode_follow(struct lyrics_mode *m, bool timed, int sung,
                        uint32_t now, int hz);
void lyrics_mode_scroll(struct lyrics_mode *m, int direction, int count,
                        uint32_t now, int hz);
int lyrics_idle_ticks(long elapsed, long next_line, bool paused, bool animated,
                      bool word_timed, const struct lyrics_mode *m,
                      bool timed, uint32_t now, int hz);
#endif
