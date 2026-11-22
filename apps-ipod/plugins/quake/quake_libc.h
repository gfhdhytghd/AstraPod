/* C library bindings for staged Quake sources. GPL-2.0-or-later. */
#ifndef ASTRAPOD_QUAKE_LIBC_H
#define ASTRAPOD_QUAKE_LIBC_H

#include "plugin.h"
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>

struct quake_file;
struct quake_file *quake_fopen(const char *path, const char *mode);
int quake_fclose(struct quake_file *file);
int quake_fflush(struct quake_file *file);
bool quake_fslice(struct quake_file *file, long offset, size_t bytes);
int quake_fseek(struct quake_file *file, long offset, int whence);
size_t quake_fread(void *ptr, size_t size, size_t count, struct quake_file *file);
size_t quake_fwrite(const void *ptr, size_t size, size_t count,
                    struct quake_file *file);
int quake_fgetc(struct quake_file *file);
int quake_feof(struct quake_file *file);
int quake_fprintf(struct quake_file *file, const char *fmt, ...);
int quake_scan_line(struct quake_file *file, const char *fmt, void *out,
                    size_t size);
int quake_snprintf(char *dst, size_t bytes, const char *fmt, ...);
int quake_vsnprintf(char *dst, size_t bytes, const char *fmt, va_list args);
int quake_log(const char *fmt, ...);
int quake_unlink(const char *path);

void quake_check_events(void);
bool quake_system_event(long button);
void quake_quit(enum plugin_status status) __attribute__((noreturn));
void quake_close_files(void);
enum quake_data { QUAKE_DATA_OK, QUAKE_DATA_MISSING, QUAKE_DATA_INVALID };
enum quake_data quake_check_data(void);
void quake_audio_pump(void);
void quake_audio_stop(void);

#define QUAKE_BASE ROCKBOX_DIR "/quake"
#define QUAKE_DATA QUAKE_BASE "/id1/"
#define QUAKE_PACK_FILES 2048
#define QUAKE_MAX_FILE (8 * 1024 * 1024)

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
double quake_sin(double x);
double quake_cos(double x);
double quake_floor(double x);
double quake_ceil(double x);
float quake_atan(float x);
float quake_atan2(float y, float x);
float quake_sqrt(float x);
float quake_tan(float x);
float quake_pow(float x, float y);
float quake_logarithm(float x);
#define sin quake_sin
#define cos quake_cos
#define floor quake_floor
#define ceil quake_ceil
#define atan quake_atan
#define atan2 quake_atan2
#define sqrt quake_sqrt
#define tan quake_tan
#define pow quake_pow
#define fabs __builtin_fabs
#else
/* The hosted SDK uses Rockbox's freestanding math.h. */
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double x, double y);
double fabs(double x);
#endif

#define FILE struct quake_file
#define fopen quake_fopen
#define fclose quake_fclose
#define fflush quake_fflush
#define fseek quake_fseek
#define fread quake_fread
#define fwrite quake_fwrite
#define fgetc quake_fgetc
#define feof quake_feof
#define fprintf quake_fprintf
#define fscanf(file, fmt, out) quake_scan_line(file, fmt, out, sizeof(*(out)))
#define sprintf(dst, ...) quake_snprintf(dst, __builtin_object_size(dst, 1), \
                                        __VA_ARGS__)
#define vsprintf(dst, fmt, args) \
    quake_vsnprintf(dst, __builtin_object_size(dst, 1), fmt, args)
#define printf quake_log
#undef LOGF
#define LOGF(...) ((void)0)
#define snprintf quake_snprintf
#define atoi rb->atoi
#define rand rb->rand
#define strcpy rb->strcpy
#define strncpy rb->strncpy
#define strcat rb->strcat
#define strcmp rb->strcmp
#define strncmp rb->strncmp
#define strlen rb->strlen
#define strchr rb->strchr
#define strrchr rb->strrchr
#undef strstr
#define strstr rb->strstr
#define memcpy rb->memcpy
#define memset rb->memset
#define memcmp rb->memcmp
#undef memchr
#define memchr rb->memchr
#define mkdir rb->mkdir
#define unlink quake_unlink
#define assert(test) ((test) ? (void)0 : Sys_Error("Quake engine assertion failed"))
#define M_PI 3.14159265358979323846
#endif
