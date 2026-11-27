/* Shared shuffle state for USB iAP and iAP2. GPL version 2+. */
#ifndef USB_IAP_SHUFFLE_MODE_H
#define USB_IAP_SHUFFLE_MODE_H

#include "settings.h"
#include "iap_library.h"
#include "playlist.h"

/* Both wire formats number Off, Songs and Albums in that order. */
static inline unsigned usb_iap_shuffle_mode(void)
{
    return global_settings.shuffle_mode;
}

static inline bool usb_iap_set_shuffle_mode(unsigned mode)
{
    if (iap_library_queue_lent() || !playlist_set_shuffle_mode(mode))
        return false;

    settings_save();
    return true;
}

#endif
