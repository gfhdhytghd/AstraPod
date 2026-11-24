#!/usr/bin/env python3
"""Exercise the production Settings EP0 handler with an in-memory UI queue."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / "firmware/usbstack/podlink_shell_settings.c"
harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usb_ch9.h"
#include "podlink-shell-settings.h"
#include "sm1-test-control.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SYS_SM1_TEST_CONTROL 42
enum { USB_CONTROL_ACK, USB_CONTROL_STALL };
volatile unsigned sm1_test_submitted;
volatile unsigned sm1_test_completed;
static bool record_active;
static bool queue_accept = true;
static unsigned posts;
static int response;
static int response_size;
static unsigned irq_level;

static int disable_irq_save(void)
{
    assert(!irq_level);
    return irq_level++;
}

static void restore_irq(int level)
{
    assert(irq_level == 1 && level == 0);
    irq_level = level;
}

bool sm1_test_record_active(void)
{
    return record_active;
}

static bool button_queue_try_post(long id, intptr_t data)
{
    assert(id == SYS_SM1_TEST_CONTROL && data == SHELL_SETTINGS_EVENT);
    ++posts;
    return queue_accept;
}

static void usb_core_control_response(int result, void *buffer, int size)
{
    assert(irq_level == 0);
    assert((size == 0) == (buffer == NULL));
    response = result;
    response_size = size;
}

#include "production.inc"

static uint8_t buffer[SHELL_SETTINGS_OUT_SIZE];

static int request(int type, unsigned value, unsigned len, bool allowed)
{
    struct usb_ctrlrequest req = {
        .bRequestType = type,
        .bRequest = SM1_TEST_USB_REQUEST,
        .wIndex = SHELL_SETTINGS_INDEX,
        .wValue = value,
        .wLength = len,
    };
    response = -1;
    shell_settings_request(&req, buffer, sizeof(buffer), allowed);
    assert(response != -1);
    return response;
}

static int submit_request(unsigned op, uint32_t cookie, const char *body,
                          size_t len)
{
    memset(buffer, 0xff, sizeof(buffer));
    for (unsigned i = 0; i < 4; ++i)
        buffer[i] = cookie >> (8 * i);
    memcpy(buffer + 4, body, len);
    return request(0x40, op, len + 4, true);
}

static void read_reply(unsigned page, uint32_t cookie, size_t length)
{
    memset(buffer, 0xff, sizeof(buffer));
    assert(request(0xc0, page, SHELL_SETTINGS_PAGE_SIZE, true) ==
           USB_CONTROL_ACK);
    assert(response_size == SHELL_SETTINGS_PAGE_SIZE);
    for (unsigned i = 0; i < 4; ++i)
        assert(buffer[i] == (uint8_t)(cookie >> (8 * i)));
    assert((size_t)(buffer[4] + (buffer[5] << 8)) == length);
}

int main(void)
{
    const unsigned payload = SHELL_SETTINGS_PAGE_SIZE - SHELL_SETTINGS_PAGE_HEADER;
    const uint32_t cookie = 0x8f0952f4;
    char reply[SHELL_SETTINGS_PAGE_SIZE * 2];
    for (size_t i = 0; i < sizeof(reply); ++i)
        reply[i] = 'a' + i % 26;

    read_reply(0, 0, 0);
    assert(request(0xc0, 0, SHELL_SETTINGS_PAGE_SIZE, false) == USB_CONTROL_STALL);
    assert(request(0xc0, 0, SHELL_SETTINGS_PAGE_SIZE - 1, true) == USB_CONTROL_STALL);
    assert(request(0x40, 1, sizeof(buffer) + 1, true) == USB_CONTROL_STALL);
    assert(submit_request(SHELL_SETTINGS_GET, 0, "shuffle", 7) == USB_CONTROL_STALL);
    assert(submit_request(SHELL_SETTINGS_LIST, cookie, "1", 1) == USB_CONTROL_STALL);
    assert(submit_request(SHELL_SETTINGS_SET, cookie, "shuffle:\non", 11) ==
           USB_CONTROL_STALL);
    assert(submit_request(SHELL_SETTINGS_SET, cookie, "shuffle:\0on", 11) ==
           USB_CONTROL_STALL);
    assert(submit_request(SHELL_SETTINGS_ACTION, cookie, "\x02/action", 8) ==
           USB_CONTROL_STALL);
    assert(posts == 0);

    assert(submit_request(SHELL_SETTINGS_LIST, cookie, "\x02\x01", 2) ==
           USB_CONTROL_ACK);
    assert(posts == 1 && sm1_test_submitted == 1);
    assert(shell_settings_operation() == SHELL_SETTINGS_LIST);
    assert(shell_settings_ordinal() == 258);
    assert(submit_request(SHELL_SETTINGS_GET, cookie + 1, "shuffle", 7) ==
           USB_CONTROL_STALL);
    read_reply(0, 0, 0);

    shell_settings_publish(reply, sizeof(reply));
    for (unsigned page = 0; page < 3; ++page)
    {
        read_reply(page, cookie, sizeof(reply));
        size_t slice = MIN(payload, sizeof(reply) - page * payload);
        assert(memcmp(buffer + SHELL_SETTINGS_PAGE_HEADER,
                      reply + page * payload, slice) == 0);
        for (size_t i = slice; i < payload; ++i)
            assert(buffer[SHELL_SETTINGS_PAGE_HEADER + i] == 0);
    }
    read_reply(UINT16_MAX, cookie, sizeof(reply));
    for (unsigned i = SHELL_SETTINGS_PAGE_HEADER; i < SHELL_SETTINGS_PAGE_SIZE; ++i)
        assert(buffer[i] == 0);

    ++sm1_test_completed;
    assert(submit_request(SHELL_SETTINGS_GET, cookie, "shuffle", 7) ==
           USB_CONTROL_STALL);
    record_active = true;
    assert(submit_request(SHELL_SETTINGS_GET, cookie + 1, "shuffle", 7) ==
           USB_CONTROL_STALL);
    record_active = false;
    queue_accept = false;
    assert(submit_request(SHELL_SETTINGS_GET, cookie + 1, "shuffle", 7) ==
           USB_CONTROL_STALL);
    assert(sm1_test_submitted == sm1_test_completed);
    read_reply(0, cookie, sizeof(reply));

    queue_accept = true;
    assert(submit_request(SHELL_SETTINGS_SET, cookie + 1, "shuffle: on", 11) ==
           USB_CONTROL_ACK);
    assert(shell_settings_operation() == SHELL_SETTINGS_SET);
    assert(strcmp(shell_settings_body(), "shuffle: on") == 0);
    read_reply(0, 0, 0);
    const char *overflow = "{\"error\":\"Settings reply is too large\"}";
    shell_settings_publish(NULL, SHELL_SETTINGS_REPLY_SIZE + 1);
    read_reply(0, cookie + 1, strlen(overflow));
    assert(memcmp(buffer + SHELL_SETTINGS_PAGE_HEADER, overflow,
                  strlen(overflow)) == 0);
    ++sm1_test_completed;

    assert(submit_request(SHELL_SETTINGS_ACTION, cookie + 2, "\x01/action", 8) ==
           USB_CONTROL_ACK);
    assert(shell_settings_operation() == SHELL_SETTINGS_ACTION);
    assert(strcmp(shell_settings_body() + 1, "/action") == 0);
    shell_settings_publish("{}", 2);
    read_reply(0, cookie + 2, 2);
    assert(submit_request(SHELL_SETTINGS_GET, cookie + 3, "shuffle", 7) ==
           USB_CONTROL_STALL);
    ++sm1_test_completed;

    char max_body[SHELL_SETTINGS_OUT_SIZE - 4];
    memset(max_body, 'a', sizeof(max_body));
    assert(submit_request(SHELL_SETTINGS_SET, cookie + 3,
                          max_body, sizeof(max_body)) == USB_CONTROL_ACK);
    assert(strlen(shell_settings_body()) == sizeof(max_body));
    assert(memcmp(shell_settings_body(), max_body, sizeof(max_body)) == 0);
    puts("Settings EP0 request, busy slot and reply-page checks passed");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="podlink-shell-settings-") as directory:
    out = Path(directory)
    (out / "production.inc").write_text(
        re.sub(r'^#include "[^"]+"\n', "", source.read_text(), flags=re.M))
    (out / "test.c").write_text(harness)
    binary = out / "test"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I", str(root / "firmware/export"), str(out / "test.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
