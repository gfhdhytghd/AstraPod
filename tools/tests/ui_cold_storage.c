/* Delay the first read of each simulator storage file. GPL version 2+. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static ssize_t (*read_next)(int, void *, size_t);
static unsigned (*sleep_ticks)(unsigned);
static char cwd[PATH_MAX];
static const char *root;
static size_t root_len;
static const char *suffix;
static size_t suffix_len;
static struct timespec delay;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct file_seen {
    dev_t device;
    ino_t inode;
    struct timespec ready;
    struct file_seen *next;
};
static struct file_seen *seen;

static void __attribute__((constructor)) init(void)
{
    read_next = dlsym(RTLD_NEXT, "read");
    suffix = getenv("PODBOX_COLD_READ_SUFFIX");
    suffix_len = suffix ? strlen(suffix) : 0;
    if (getenv("PODBOX_COLD_READ_COOPERATIVE"))
    {
        /* The firmware exports sleep(ticks). Yield the Rockbox thread so
         * a delayed read can overlap UI animation on sigaltstack builds. */
        sleep_ticks = dlsym(RTLD_DEFAULT, "sleep");
        if (!sleep_ticks || sleep_ticks == dlsym(RTLD_NEXT, "sleep"))
        {
            fputs("cold storage: Rockbox sleep is unavailable\n", stderr);
            abort();
        }
    }
    const char *ms = getenv("PODBOX_COLD_READ_MS");
    long value = ms ? strtol(ms, NULL, 10) : 0;
    /* A caller can supply the host path if the simulator exports getcwd. */
    root = getenv("PODBOX_COLD_READ_ROOT");
    if (!root)
        root = getcwd(cwd, sizeof(cwd));
    if (value > 0 && root)
    {
        root_len = strlen(root);
        delay = (struct timespec){value / 1000, value % 1000 * 1000000};
    }
}

static void wait_for_file(const struct stat *file)
{
    pthread_mutex_lock(&lock);
    struct file_seen *entry = seen;
    while (entry && (entry->device != file->st_dev || entry->inode != file->st_ino))
        entry = entry->next;
    if (!entry)
    {
        if (suffix_len)
            fprintf(stderr, "cold storage: delaying first %s read\n", suffix);
        entry = malloc(sizeof(*entry));
        if (entry)
        {
            entry->device = file->st_dev;
            entry->inode = file->st_ino;
            clock_gettime(CLOCK_MONOTONIC, &entry->ready);
            entry->ready.tv_sec += delay.tv_sec;
            entry->ready.tv_nsec += delay.tv_nsec;
            if (entry->ready.tv_nsec >= 1000000000)
            {
                entry->ready.tv_nsec -= 1000000000;
                entry->ready.tv_sec++;
            }
            entry->next = seen;
            seen = entry;
        }
    }
    struct timespec ready = entry ? entry->ready : (struct timespec){0};
    pthread_mutex_unlock(&lock);
    if (sleep_ticks)
    {
        struct timespec now;
        while (!clock_gettime(CLOCK_MONOTONIC, &now) &&
               (now.tv_sec < ready.tv_sec ||
                (now.tv_sec == ready.tv_sec && now.tv_nsec < ready.tv_nsec)))
            sleep_ticks(1);
        return;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ready, NULL) == EINTR)
        ;
}

ssize_t read(int fd, void *buffer, size_t bytes)
{
    struct stat file;
    if (root_len && bytes && !fstat(fd, &file) && S_ISREG(file.st_mode))
    {
        char path[PATH_MAX], link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t length = readlink(link, path, sizeof(path));
        if (length > (ssize_t)root_len && path[root_len] == '/' &&
            !memcmp(path, root, root_len) &&
            (!suffix_len || ((size_t)length >= suffix_len &&
             !memcmp(path + length - suffix_len, suffix, suffix_len))))
            wait_for_file(&file);
    }
    return read_next(fd, buffer, bytes);
}
