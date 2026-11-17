/* FU51 breadcrumbs are firmware-only (firmware/export/crumbs.h); the host
 * liveness build compiles the PodLink platform with them as no-ops. */
#ifndef CRUMBS_H
#define CRUMBS_H
#define crumb(id, value) ((void)(value))
#define crumb_status(i, value) ((void)(value))
#endif
