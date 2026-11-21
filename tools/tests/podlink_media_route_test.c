/* Exercise the resident media filter without a disk or a USB session.
 * From the repository root:
 * cc -DCHECKWPS -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *   -Itools/tests/podlink_stubs -Ifirmware/export -idirafter firmware/include \
 *   -Ifirmware/target/hosted -Iapps-ipod -Iapps-ipod/screens/system \
 *   tools/tests/podlink_media_route_test.c \
 *   apps-ipod/screens/system/podlink_media.c -o /tmp/podlink_media_route_test
 */

#include <assert.h>
#include <string.h>
#include "podlink_media.h"

static bool enqueue(const char *command)
{
    struct podlink_message message = {
        .type = PODLINK_TEXT,
        .length = strlen(command),
        .data = (const unsigned char *)command,
    };
    return podlink_media_enqueue(&message);
}

int main(void)
{
    podlink_media_init();
    assert(enqueue("MS2 LIST"));
    podlink_media_disconnect();
    assert(!enqueue("vw\t1"));
    assert(enqueue("MS3 DETAIL /x"));
    return 0;
}
