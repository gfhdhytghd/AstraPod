/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IPOD6G_BUTTON_IDLE_BOOST_H
#define IPOD6G_BUTTON_IDLE_BOOST_H

/* Status and worker wakes share the input queue and can retire an expiring
 * boost. Only input events renew it. */
static inline bool button_idle_boost_wake(long event)
{
    if (event != BUTTON_NONE)
        return false;
    button_boost(false);
    return true;
}

#endif
