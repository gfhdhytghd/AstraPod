/* Foreground Lua event delivery. GNU GPL version 2 or later. */
#include <limits.h>
#include "bindings.h"
#include "lauxlib.h"
#undef yield

enum lua_event_id { EVENT_ACTION, EVENT_TIMER, EVENT_TRACK, EVENT_COUNT };
static const char *const names[] = {"action", "timer", "playback", NULL};

static struct lua_event {
    int ref;
    int period;
    unsigned long due;
    bool suspended;
    bool pending;
    int value;
    int last_action;
} events[EVENT_COUNT];

static bool dispatching;
static unsigned int track_sequence;

void lua_reset_events(void)
{
    rb->lua_track_event(false);
    for (int i = 0; i < EVENT_COUNT; ++i)
        events[i] = (struct lua_event){ .ref = LUA_NOREF };
    dispatching = false;
}

static int event_id(lua_State *L)
{
    return luaL_checkoption(L, 1, NULL, names);
}

static int event_register(lua_State *L)
{
    int id = event_id(L);
    struct lua_event *event = &events[id];
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int period = 0;
    if (id == EVENT_TIMER)
    {
        period = luaL_checkint(L, 3);
        luaL_argcheck(L, period > 0 && period <= 3600 * HZ, 3,
                      "invalid timer period");
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_unref(L, LUA_REGISTRYINDEX, event->ref);
    *event = (struct lua_event) {
        .ref = ref,
        .period = period,
        .due = (unsigned long)*rb->current_tick + period
    };
    if (id == EVENT_TRACK)
        track_sequence = rb->lua_track_event(true);
    lua_pushstring(L, names[id]);
    return 1;
}

static int event_unregister(lua_State *L)
{
    int id = event_id(L);
    luaL_unref(L, LUA_REGISTRYINDEX, events[id].ref);
    events[id].ref = LUA_NOREF;
    if (id == EVENT_TRACK)
        rb->lua_track_event(false);
    return 0;
}

static int event_suspend(lua_State *L)
{
    int first = 0, end = EVENT_COUNT;
    if (!lua_isnoneornil(L, 1))
    {
        first = event_id(L);
        end = first + 1;
    }
    bool suspended = lua_isnone(L, 2) || lua_toboolean(L, 2);
    for (int id = first; id < end; ++id)
        events[id].suspended = suspended;
    return 0;
}

static int event_trigger(lua_State *L)
{
    int id = event_id(L);
    struct lua_event *event = &events[id];
    event->pending = lua_isnone(L, 2) || lua_toboolean(L, 2);
    if (id == EVENT_TIMER && !event->pending && !lua_isnoneornil(L, 3))
        event->due = (unsigned long)luaL_checkinteger(L, 3);
    else
        event->value = luaL_optint(L, 3, 0);
    return 0;
}

static bool receives(enum lua_event_id id)
{
    return events[id].ref != LUA_NOREF && !events[id].suspended;
}

static void dispatch(lua_State *L, int timeout)
{
    if (dispatching)
    {
        if (timeout)
            rb->sleep(timeout);
        lua_check_exit();
        return;
    }

    dispatching = true;
    int action = ACTION_NONE;
    if (receives(EVENT_ACTION))
        action = lua_poll_action(timeout);
    else if (timeout)
        rb->sleep(timeout);
    if (receives(EVENT_TRACK))
    {
        unsigned int sequence = rb->lua_track_event(true);
        if (sequence != track_sequence)
        {
            events[EVENT_TRACK].pending = true;
            events[EVENT_TRACK].value = PLAYBACK_EVENT_TRACK_CHANGE;
        }
        track_sequence = sequence;
    }

    for (int id = 0; id < EVENT_COUNT; ++id)
    {
        struct lua_event *event = &events[id];
        if (!receives(id))
            continue;
        unsigned long now = *rb->current_tick;
        bool timer = id == EVENT_TIMER && (long)(now - event->due) >= 0;
        bool input = id == EVENT_ACTION && action != ACTION_UNKNOWN &&
                     (action != ACTION_NONE ||
                      (event->last_action != ACTION_NONE &&
                       rb->button_status() == BUTTON_NONE));
        if (!input && !event->pending && !timer)
            continue;
        int value = input ? action : event->pending ? event->value : 0;
        if (input)
            event->last_action = action;
        else
            event->pending = false;
        if (id == EVENT_TIMER)
            event->due = now + event->period;
        lua_rawgeti(L, LUA_REGISTRYINDEX, event->ref);
        lua_pushinteger(L, value);
        if (lua_pcall(L, 1, 0, 0))
        {
            dispatching = false;
            lua_error(L);
        }
        lua_check_exit();
    }
    dispatching = false;
}

int lua_sleep_ticks(lua_State *L)
{
    int ticks = luaL_checkint(L, 1);
    luaL_argcheck(L, ticks >= 0 && ticks <= 3600 * HZ, 1, "invalid sleep duration");
    unsigned long deadline = (unsigned long)*rb->current_tick + ticks;
    for (long left = ticks; left > 0; left = deadline - *rb->current_tick)
        dispatch(L, MIN(left, 2));
    lua_check_exit();
    return 0;
}

int lua_yield_to_firmware(lua_State *L)
{
    dispatch(L, 0);
    rb->yield();
    lua_check_exit();
    return 0;
}

void lua_register_events(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"register", event_register},
        {"unregister", event_unregister},
        {"suspend", event_suspend},
        {"trigger", event_trigger},
        {NULL, NULL}
    };
    lua_reset_events();
    luaL_register(L, "rockev", methods);
    lua_pop(L, 1);
}
