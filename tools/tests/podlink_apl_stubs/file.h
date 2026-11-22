/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The offline APL1 disk worker uses the host fixture's own filesystem. */
#include <fcntl.h>
#include <unistd.h>

#define MAX_PATH 260

ssize_t apl_test_write(int fd, const void *data, size_t size);
int apl_test_close(int fd);
int apl_test_rename(const char *from, const char *to);

#define write apl_test_write
#define close apl_test_close
#define rename apl_test_rename
