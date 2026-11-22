/* Owned plugin filesystem operations. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_FILES_H
#define ASTRAPOD_ROCK_FILES_H

#include "file.h"
#include "gcc_extensions.h"
#include "dir.h"
#include "metadata.h"

int rock_open(const char *, int, ...);
int rock_creat(const char *, mode_t);
int rock_close(int);
ssize_t rock_read(int, void *, size_t);
ssize_t rock_write(int, const void *, size_t);
off_t rock_lseek(int, off_t, int);
off_t rock_ffilesize(int);
int rock_fdprintf(int, const char *, ...) ATTRIBUTE_PRINTF(2, 3);
int rock_read_line(int, char *, int);
bool rock_file_exists(const char *);
bool rock_dir_exists(const char *);
int rock_mkdir(const char *);
int rock_rmdir(const char *);
int rock_ftruncate(int, off_t);
int rock_remove(const char *);
int rock_rename(const char *, const char *);
DIR *rock_opendir(const char *);
struct DIRENT *rock_readdir(DIR *);
int rock_closedir(DIR *);
struct dirinfo rock_dir_get_info(DIR *, struct DIRENT *);
int rock_dir_next(DIR *, struct DIRENT **, struct dirinfo *);
int rock_file_sync(int);
void rock_close_files(void);
bool rock_get_metadata(struct mp3entry *id3, int fd, const char *path);

struct bitmap;
struct custom_format;
int rock_read_bmp_fd(int fd, struct bitmap *bm, int maxsize, int format,
                     const struct custom_format *cformat);

#endif
