/* Compile the MPEGPlayer copy of libmad entirely in DRAM. GPL-2.0-or-later. */
#include "config.h"
#undef USE_IRAM
#undef ICODE_ATTR
#undef IDATA_ATTR
#undef ICONST_ATTR
#undef IBSS_ATTR
#define ICODE_ATTR
#define IDATA_ATTR
#define ICONST_ATTR
#define IBSS_ATTR
