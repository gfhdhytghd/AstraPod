/* PodLink Shell foreground plugin tests. GNU GPL version 2 or later. */
#ifndef PODLINK_PLUGIN_TEST_H
#define PODLINK_PLUGIN_TEST_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(IPOD_6G) && defined(HAVE_USB_PODLINK) && \
    defined(ASTRAPOD_ROCK_COMPAT) && !defined(BOOTLOADER)
#define HAVE_PODLINK_PLUGIN_TEST
#endif

#ifdef HAVE_PODLINK_PLUGIN_TEST
#define PODLINK_TEST_CHUNK_BASE 0x100
#define PODLINK_TEST_CHUNK_SIZE 256

struct usb_ctrlrequest;

bool podlink_plugin_test_matches(const struct usb_ctrlrequest *req);
void podlink_plugin_test_request(const struct usb_ctrlrequest *req,
                                uint8_t *data, size_t capacity, bool allowed);
bool podlink_plugin_test_launch(void);

bool podlink_plugin_test_claim(bool shell);
void podlink_plugin_test_release(void);
void podlink_plugin_test_started(void);
void podlink_plugin_test_stopped(void);
void podlink_plugin_test_usb_taken(void);

bool podlink_plugin_test_keys_active(void);
bool podlink_plugin_test_key(unsigned key, unsigned edge);
int podlink_plugin_test_buttons(void);
void podlink_plugin_test_clear_keys(void);

void podlink_plugin_test_lcd_init(void);
void podlink_plugin_test_lcd_present(const uint16_t *pixels, int x, int y,
                                    int width, int height);
bool podlink_plugin_test_lcd_begin(void);
size_t podlink_plugin_test_lcd_read(unsigned value, unsigned length,
                                    uint8_t *reply);
/* Implemented by the target driver, which owns the presentation lock. */
void lcd_copy_presented_for_test(uint16_t *pixels);
void podlink_plugin_test_lcd_copy(uint16_t *pixels);
#endif

#endif
