/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The scratch buffer, as a plain array.
 *
 * app_buffer.c uses pluginbuf for the same lease as the hardware. Hosted
 * plugins load through SDL and use this array as scratch.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <stdint.h>

_Alignas(uintptr_t) unsigned char pluginbuf[PLUGIN_BUFFER_SIZE];

/* Same story for the codec region (audio/codecs.c). CODEC_SIZE is 0 off
 * target, because a sim loads codecs as host shared objects and never copies
 * one into a buffer -- the array exists to satisfy the reference, and
 * codec_get_buffer_callback() correctly reports no room in it. */
unsigned char codecbuf[CODEC_SIZE];

#endif /* SIMULATOR */
