/* Duke3D engine integration. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_DUKE_BUILD_H
#define ASTRAPOD_DUKE_BUILD_H

#include "plugin.h"
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include "lib/stdio_compat.h"

#define DUKE_DIR ROCKBOX_DIR "/duke3d"
#define DUKE_SAVES DUKE_DIR "/saves"

bool duke_in_menu(void);
void duke_service(void);
void duke_cleanup(void);
void duke_pcm_pump(void);
int duke_open(const char *name, int flags, ...);
int duke_remove(const char *name);
bool duke_file_exists(const char *name);
FILE *duke_fopen(const char *name, const char *mode);
int duke_fclose(FILE *file);
unsigned int duke_pcm_rate(void);

#endif
