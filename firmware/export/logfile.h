/* PodBox disk-log budget. GPL v2 or later. */
#ifndef PODBOX_LOGFILE_H
#define PODBOX_LOGFILE_H
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include "gcc_extensions.h"
#define LOGFILE_TOTAL_BYTES (5u * 1024 * 1024)
#define LOGFILE_PART_BYTES (LOGFILE_TOTAL_BYTES / 2)
/* Call on the main thread before any PodBox disk logger. Only that thread's
 * disk operations are relayed; storage callbacks run directly, never waiting
 * for a worker that might need the storage thread. No calls from IRQs. */
void logfile_init(void);
/* Internal storage-queue dispatch, including the USB ownership gate. */
void logfile_storage_request(void *request, bool available);
int logfile_open(const char *path);
int logfile_open_header(const char *path, const char *header);
ssize_t logfile_write(int fd, const void *data, size_t size);
int logfile_printf(int fd, const char *format, ...) ATTRIBUTE_PRINTF(2, 3);
off_t logfile_size(int fd);
int logfile_close(int fd);
#endif
