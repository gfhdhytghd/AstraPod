/* Dock tap/hold input. Copyright (C) 2026. GPL version 2+. */
#include "config.h"
#include "button.h"
#include "kernel.h"
#include "input/agents_remote.h"
#include "remote_log.h"

bool agents_remote_pending(const struct agents_remote_state *s)
{
    return s->queued || s->tap_pending || s->hold[0].down || s->hold[1].down ||
           s->hold[2].down || s->hold[3].down;
}

static void emit(struct agents_remote_state *s, int button)
{
    s->replay[s->queued++] = button;
}

static int take(struct agents_remote_state *s)
{
    if (!s->queued) return BUTTON_NONE;
    int button = s->replay[0];
    for (unsigned int i = 1; i < s->queued; i++)
        s->replay[i - 1] = s->replay[i];
    s->queued--;
    return button;
}

static void play_tap(struct agents_remote_state *s, unsigned long now)
{
    remote_log_event(REMOTE_LOG_TAP, now, s->play_context, 0, NULL, 0);
    /* Agents already discarded the short recording on physical release. */
    if (s->play_context == REMOTE_AGENTS_FRUIT)
        emit(s, BUTTON_MULTIMEDIA_PLAYPAUSE);
    else if (s->play_context == REMOTE_AGENTS || s->play_context == REMOTE_COVERS ||
        s->play_context == REMOTE_ALBUM_LIST)
    {
        emit(s, BUTTON_SELECT);
        emit(s, BUTTON_SELECT | BUTTON_REL);
    }
    else
    {
        emit(s, BUTTON_RC_PLAY);
        emit(s, BUTTON_RC_PLAY | BUTTON_REL);
    }
    s->tap_pending = false;
    s->taps = 0;
}

static void play_taps(struct agents_remote_state *s, unsigned long now)
{
    if (s->taps == 2)
    {
        remote_log_event(REMOTE_LOG_DOUBLE, now, s->play_context, 0, NULL, 0);
        emit(s, REMOTE_SWITCH_BUTTON);
        s->tap_pending = false;
        s->taps = 0;
    }
    else play_tap(s, now);
}

static unsigned long play_hold_ticks(int context)
{
    return context == REMOTE_AGENTS || context == REMOTE_AGENTS_FRUIT ||
           context == REMOTE_WPS ?
           (3 * HZ + 9) / 10 : (3 * HZ + 4) / 5;
}

static bool play_button(struct agents_remote_state *s, int context, int button,
                        int held, unsigned long now)
{
    struct agents_remote_hold *h = &s->hold[3];
    bool event = (button & BUTTON_REMOTE) == BUTTON_RC_PLAY;
    bool release = event && (button & BUTTON_REL);
    unsigned long window = REMOTE_TAP_WINDOW_TICKS;
    if (context != s->play_context)
    {
        s->tap_pending = s->second = false;
        s->taps = 0;
        if (h->down) h->fired = true;
    }
    if (s->tap_pending && !s->second && now - s->released > window)
        play_taps(s, now);
    /* A sampled release can precede a queued repeat. Only a physical down
     * edge may start the next tap or hold. */
    if (!h->down && event && !(button & (BUTTON_REL | BUTTON_REPEAT)))
    {
        h->down = true;
        h->fired = false;
        h->at = now;
        s->second = s->tap_pending;
        s->play_context = context;
        if (context == REMOTE_AGENTS)
            emit(s, BUTTON_RC_PLAY);
    }
    if (h->down && (event || button == BUTTON_NONE))
    {
        bool ended = release || (button == BUTTON_NONE && !(held & BUTTON_RC_PLAY));
        if (!h->fired && now - h->at >= play_hold_ticks(context))
        {
            if (s->tap_pending) play_taps(s, now);
            remote_log_event(REMOTE_LOG_HOLD, now, context, 0, NULL, 0);
            s->second = false;
            h->fired = true;
            /* WPS and Lyrics share one gesture state across the transition. */
            if (context == REMOTE_WPS)
                emit(s, REMOTE_LYRICS_BUTTON);
            else if (context != REMOTE_AGENTS && context != REMOTE_AGENTS_FRUIT)
            {
                emit(s, BUTTON_PLAY);
                emit(s, BUTTON_PLAY | BUTTON_REPEAT);
            }
        }
        if (h->fired && event && !release && (button & BUTTON_REPEAT) &&
            context == s->play_context && context != REMOTE_AGENTS &&
            context != REMOTE_AGENTS_FRUIT && context != REMOTE_WPS && !s->queued)
            emit(s, BUTTON_PLAY | BUTTON_REPEAT);
        if (ended)
        {
            h->down = false;
            if (!h->fired)
            {
                if (context == REMOTE_AGENTS)
                    emit(s, BUTTON_RC_PLAY | BUTTON_REL | BUTTON_REPEAT);
                if (s->second && s->taps == 2)
                {
                    remote_log_event(REMOTE_LOG_TRIPLE, now, context, 0,
                                     NULL, 0);
                    s->tap_pending = s->second = false;
                    s->taps = 0;
                    emit(s, REMOTE_COVERS_BUTTON);
                }
                else
                {
                    s->taps = s->second ? 2 : 1;
                    s->second = false;
                    s->tap_pending = true;
                    s->released = now;
                }
            }
            else if (context == s->play_context && context != REMOTE_WPS)
                emit(s, context == REMOTE_AGENTS_FRUIT ?
                        BUTTON_MULTIMEDIA_PLAYPAUSE : BUTTON_PLAY | BUTTON_REL);
        }
    }
    if (event && (button & BUTTON_MAIN))
        emit(s, (button & BUTTON_MAIN) |
                (button & (BUTTON_REL | BUTTON_REPEAT)));
    return event;
}

/* QuickScreen must still receive Play as Select immediately, which can close
 * the overlay on the first tap. Observe the physical triple independently of
 * that context change so its third release always reaches Cover Flow. */
static bool global_triple(struct agents_remote_state *s, int context,
                          int button, int held, unsigned long now)
{
    if (s->triple_down && (button & BUTTON_REMOTE) != BUTTON_RC_PLAY &&
        !(held & BUTTON_RC_PLAY))
    {
        /* A modal teardown may clear its queued release. The sampled up
         * edge still belongs to this tap, even after QuickScreen closes. */
        button = BUTTON_RC_PLAY | BUTTON_REL;
    }
    if (!s->triple_down && now - s->triple_released > REMOTE_TAP_WINDOW_TICKS)
        s->triple_taps = 0;
    if ((button & BUTTON_REMOTE) != BUTTON_RC_PLAY) return false;
    if (!(button & (BUTTON_REL | BUTTON_REPEAT)))
    {
        if (!s->triple_down)
        {
            s->triple_pressed = now;
            s->triple_context = context;
            s->triple_down = true;
        }
    }
    else if ((button & BUTTON_REL) && s->triple_down)
    {
        unsigned long hold = play_hold_ticks(s->triple_context);
        s->triple_down = false;
        s->triple_released = now;
        if (now - s->triple_pressed >= hold) s->triple_taps = 0;
        else if (++s->triple_taps == 3)
        {
            s->triple_taps = s->taps = 0;
            s->tap_pending = s->second = false;
            s->hold[3].down = s->hold[3].fired = false;
            remote_log_event(REMOTE_LOG_TRIPLE, now, context, 0, NULL, 0);
            return true;
        }
    }
    return false;
}

int agents_remote_button(struct agents_remote_state *s, int context,
                         int button, int held, unsigned long now)
{
    /* The photo viewer classifies holds itself. Do not replay a gesture
     * from the previous screen or turn a held Select into Play. */
    if (context == REMOTE_NATIVE)
    {
        *s = (struct agents_remote_state){0};
        return button;
    }
    if (s->queued) return take(s);
    if (global_triple(s, context, button, held, now)) return REMOTE_COVERS_BUTTON;
    /* Quick screen centre is Select, including a hold; no tap gesture may
     * turn into the physical BOTTOM key. The global triple may leave it. */
    if (context == REMOTE_QUICKSCREEN &&
        ((button & BUTTON_REMOTE) == BUTTON_RC_PLAY ||
         (button & BUTTON_REMOTE) == BUTTON_RC_SELECT))
        return BUTTON_RC_SELECT | (button & (BUTTON_REL | BUTTON_REPEAT));
    if ((button & BUTTON_REMOTE) == BUTTON_RC_SELECT && context == REMOTE_AGENTS_FRUIT)
    {
        /* Queue behind any gesture emitted by the Play classifier. */
        emit(s, button);
        return take(s);
    }
    if ((button & BUTTON_REMOTE) == BUTTON_RC_SELECT)
    {
        struct agents_remote_hold *h = &s->hold[0];
        if (button & BUTTON_REL)
        {
            emit(s, h->fired ? BUTTON_PLAY | BUTTON_REL : button);
            h->fired = false;
        }
        else if (button & BUTTON_REPEAT)
        {
            if (!h->fired) emit(s, BUTTON_PLAY);
            h->fired = true;
            emit(s, BUTTON_PLAY | BUTTON_REPEAT);
        }
        else { h->fired = false; emit(s, button); }
        return take(s);
    }
    if (play_button(s, context, button, held, now)) return take(s);
    /* Menu/Select retain their native press/repeat/release meanings. Direction
     * keys retain their normal press/repeat/release meanings everywhere. */
    if ((context == REMOTE_AGENTS || context == REMOTE_AGENTS_FRUIT) &&
        ((button & BUTTON_REMOTE) == BUTTON_RC_LEFT ||
         (button & BUTTON_REMOTE) == BUTTON_RC_RIGHT))
        button = (button & (BUTTON_REL | BUTTON_REPEAT)) |
                 ((button & BUTTON_REMOTE) == BUTTON_RC_LEFT ?
                  BUTTON_LEFT : BUTTON_RIGHT);
    if (button != BUTTON_NONE) emit(s, button);
    return take(s);
}
