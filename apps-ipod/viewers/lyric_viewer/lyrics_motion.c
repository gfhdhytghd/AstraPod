/* GPL version 2 or later. No screen, playback or allocation dependencies. */
#include "lyrics_motion.h"

int lyrics_lerp(int a, int b, int p) { return a + (b - a) * p / 256; }
int lyrics_ease(int p)
{
    if (p <= 0) return 0;
    if (p >= 256) return 256;
    int q = 256 - p;
    return 256 - q * q * q / 65536;
}
bool lyrics_moving(const struct lyrics_motion *m, uint32_t now)
{
    return m->from != m->to && now - m->at < m->duration;
}
int lyrics_motion_at(const struct lyrics_motion *m, uint32_t now)
{
    if (!lyrics_moving(m, now)) return m->to;
    return lyrics_lerp(m->from, m->to,
                      lyrics_ease((now - m->at) * 256 / m->duration));
}
void lyrics_retarget(struct lyrics_motion *m, int to, uint32_t now, int duration)
{
    m->from = lyrics_motion_at(m, now);
    m->to = to;
    m->at = now;
    m->duration = duration > 0 ? duration : 1;
}
int lyrics_focus_weight(int line, int position)
{
    int d = line * 256 - position;
    if (d < 0) d = -d;
    return d >= 256 ? 0 : 256 - d;
}
int lyrics_opacity(int d, int nearest)
{
    if (d < 0) d = -d;
    if (d <= 256) return lyrics_lerp(256, nearest, d);
    if (d <= 512) return lyrics_lerp(nearest, 90, d - 256);
    if (d <= 768) return lyrics_lerp(90, 51, d - 512);
    return 51;
}
int lyrics_edge_opacity(int y, int height)
{
    int d = y - 30;
    if (height - 6 - y < d) d = height - 6 - y;
    if (d <= 0) return 0;
    return d >= 24 ? 256 : d * 256 / 24;
}
int lyrics_line_height(int small, int large, int weight)
{
    return lyrics_lerp(small, large, weight) + 12;
}
int lyrics_line_y(int anchor, int fraction, int h0, int h1)
{
    return anchor - (h0 + h1) * fraction / 512;
}
int lyrics_word_progress(long elapsed, long start, long end, int width)
{
    if (elapsed < start) return 0;
    if (end <= start || elapsed >= end) return width;
    return (int)((int64_t)width * (elapsed - start) / (end - start));
}
int lyrics_progress_ticks(long elapsed, long length, int width, int hz)
{
    if (length <= 0 || elapsed >= length || width <= 0) return hz / 2;
    long pixel = (int64_t)elapsed * width / length;
    long next = ((int64_t)(pixel + 1) * length + width - 1) / width;
    long ticks = ((next - elapsed) * hz + 999) / 1000;
    return ticks > 0 ? ticks : 1;
}
int lyrics_wheel_step(struct lyrics_mode *m, int direction, uint32_t now, int hz)
{
    if (m->direction == direction && now - m->input < (uint32_t)(hz / 8))
    { if (m->burst < 12) m->burst++; }
    else m->burst = 0;
    m->direction = direction;
    m->input = now;
    return 1 + m->burst / 4;
}
void lyrics_mode_scroll(struct lyrics_mode *m, int dir, int count,
                        uint32_t now, int hz)
{
    int step = lyrics_wheel_step(m, dir, now, hz);
    m->manual = true;
    m->focus += step * dir;
    if (m->focus < 0) m->focus = 0;
    if (m->focus >= count) m->focus = count > 0 ? count - 1 : 0;
}
bool lyrics_mode_follow(struct lyrics_mode *m, bool timed, int sung,
                        uint32_t now, int hz)
{
    if (!timed) return false;
    if (m->manual && now - m->input >= (uint32_t)(4 * hz)) m->manual = false;
    if (m->manual || sung < 0 || sung == m->focus) return false;
    m->focus = sung;
    return true;
}
int lyrics_idle_ticks(long elapsed, long next, bool paused, bool animated,
                      bool words, const struct lyrics_mode *m,
                      bool timed, uint32_t now, int hz)
{
    int ticks = hz / 2;
    if (animated || (!paused && words)) return hz / 30 > 0 ? hz / 30 : 1;
    if (!paused && timed && !m->manual && next > elapsed && next - elapsed < 500)
        ticks = ((next - elapsed) * hz + 999) / 1000;
    if (timed && m->manual)
    {
        uint32_t age = now - m->input;
        int left = age >= (uint32_t)(4 * hz) ? 1 : 4 * hz - age;
        if (left < ticks) ticks = left;
    }
    return ticks > 0 ? ticks : 1;
}
