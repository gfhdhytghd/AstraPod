/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * PodLink Shell settings request and reply handoff.
 ****************************************************************************/
#ifndef PODLINK_SHELL_SETTINGS_H
#define PODLINK_SHELL_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHELL_SETTINGS_INDEX 0x4953
#define SHELL_SETTINGS_EVENT 0x20000u
#define SHELL_SETTINGS_OUT_SIZE 704
#define SHELL_SETTINGS_PAGE_SIZE 128
#define SHELL_SETTINGS_PAGE_HEADER 6
#define SHELL_SETTINGS_REPLY_SIZE 8192

enum shell_settings_op {
    SHELL_SETTINGS_LIST = 1,
    SHELL_SETTINGS_GET,
    SHELL_SETTINGS_SET,
    SHELL_SETTINGS_ACTION,
};

struct usb_ctrlrequest;
void shell_settings_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed);
unsigned shell_settings_operation(void);
char *shell_settings_body(void);
unsigned shell_settings_ordinal(void);
void shell_settings_publish(const char *json, size_t length);
long shell_settings_dispatch(void);

#endif
