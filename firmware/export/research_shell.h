/* SPDX-License-Identifier: GPL-2.0-or-later
 * FU88 research shell: command core and the platform port it calls.
 * The core is plain C and also builds on the host for tests.
 */
#ifndef RESEARCH_SHELL_H
#define RESEARCH_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS_FRAME_MAX   16384   /* PodLink message payload limit */
#define RS_TEXT_MAX    1000    /* header + text part of a reply */
#define RS_DATA_MAX    12288   /* binary part of a request or reply */
#define RS_NAME_MAX    32

enum rs_guard { RS_ALLOW = 0, RS_SOFT, RS_HARD };

/* Execute one request ("RS\t1\t<id>\t<line>[\n<data>]"). Writes the reply
 * ("RS\t1\t<id>\t<OK|ERR>\t<text>[\n<data>]") to out, which holds at least
 * RS_FRAME_MAX bytes. Returns its length; *binary is set when it has data. */
size_t rs_exec(const uint8_t *req, size_t len, uint8_t *out, bool *binary);
/* The guard class of a write to [addr, addr + len) */
enum rs_guard rs_guard(uint32_t addr, uint32_t len);
/* The last command line (at most 23 characters), for crash records */
extern char rs_last_command[24];
/* Set by "reboot": the port reboots once the reply has gone out */
extern bool rs_reboot_requested;

/* Port: supplied by the target (or the host test) */
uint32_t rs_port_read(uint32_t addr, int width);
void rs_port_write(uint32_t addr, int width, uint32_t value);
uint32_t rs_port_scratch(uint32_t *size);
bool rs_port_in_firmware(uint32_t addr, uint32_t len);
uint32_t rs_port_call(uint32_t addr, const uint32_t args[4]);
uint32_t rs_port_usec(void);
void rs_port_yield(void);
void rs_port_cache_sync(void);
int rs_port_info(char *text, size_t cap);
void rs_port_boost(bool on);
void rs_port_sm1_power(bool on);
void rs_port_mark(uint32_t value);
void rs_port_reboot(void);
int rs_port_file_get(const char *name, uint32_t off, uint8_t *buf, uint32_t len);
int rs_port_file_put(const char *name, uint32_t off, const uint8_t *buf, uint32_t len);
int rs_port_file_rm(const char *name);
int rs_port_file_ls(char *text, size_t cap);

#endif
