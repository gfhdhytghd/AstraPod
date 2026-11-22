/* Scoped redirects for staged Duke3D units. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_DUKE_REDIRECT_H
#define ASTRAPOD_DUKE_REDIRECT_H
#undef open
#define open duke_open
#undef fopen
#define fopen duke_fopen
#undef fclose
#define fclose duke_fclose
#endif
