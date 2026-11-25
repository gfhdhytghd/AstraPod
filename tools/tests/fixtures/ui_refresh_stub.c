/* The nominal 66.96 Hz panel refresh, for host builds of the animation
 * clock. GPL version 2+. */
#include <stdint.h>

uint32_t ui_frame_period_q8(void);

uint32_t ui_frame_period_q8(void)
{
    return 3823178u;
}
