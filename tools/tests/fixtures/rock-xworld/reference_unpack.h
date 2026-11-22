/* Checked copy of the upstream XWorld decompressor. GPL-2.0-or-later. */
#ifndef XWORLD_REFERENCE_UNPACK_H
#define XWORLD_REFERENCE_UNPACK_H

#include <stddef.h>
#include <stdint.h>

enum { REF_OK, REF_BAD_CRC, REF_FAULT };

/* Decode in place. On REF_OK or REF_BAD_CRC, *residue is the CRC fold. */
int reference_unpack(uint8_t *buf, size_t packed, size_t unpacked,
                     uint32_t *residue);

#endif
