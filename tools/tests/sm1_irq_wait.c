/* Host regression for the actual IRQ18 wake code; run with a native C compiler. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define HZ 100
#define SM1_IRQ (1u << 18)
#define SM1_REG(offset) latch
#define OBJ_WAIT_SUCCEEDED 0
static unsigned VIC0INTENABLE, VIC0INTENCLEAR, latch;
static uint32_t USEC_TIMER;
struct semaphore { unsigned token; };
static unsigned mode, sleeps;
static int disable_irq_save(void) { return 0; }
static void restore_irq(int old) { (void)old; }
static void semaphore_release(struct semaphore *s) { s->token = 1; }
static int semaphore_wait(struct semaphore *s, int ticks);
#define SM1_IRQ_HOST_TEST
static void semaphore_init(struct semaphore *s,int max,int start) { (void)max;s->token=start; }
#include "../../firmware/target/arm/s5l8702/ipod6g/sm1-irq-6g.c"
static int semaphore_wait(struct semaphore *s, int ticks)
{
    if (ticks && !s->token)
    {
        sleeps++;
        USEC_TIMER += (mode == 1 || (mode == 2 && sleeps == 1)) ? 200 : ticks * 10000;
        if (mode == 1 || (mode == 2 && sleeps == 2))
            latch = 4;
        if (mode == 1 || (mode == 2 && sleeps == 1))
            INT_IRQ18();
    }
    if (!s->token)
        return -1;
    s->token = 0;
    return OBJ_WAIT_SUCCEEDED;
}
static void arm(unsigned scenario)
{
    mode = scenario;
    sleeps = latch = 0;
    sm1_irq_prepare();
    assert(sm1_irq_armed && !sm1_done.token);
}
int main(void)
{
    sm1_irq_begin();
    arm(1); /* ISR during wait */
    assert(sm1_irq_wait(50000) && sleeps == 1 && sm1_sem_wakes == 1);
    arm(0); /* completion arrived before wait; stale token drained on rearm */
    latch = 4; INT_IRQ18();
    assert(sm1_irq_wait(50000) && sleeps == 0 && sm1_done.token == 1);
    arm(2); /* spurious IRQ cannot manufacture a completed packet */
    assert(sm1_irq_wait(50000) && sleeps == 2 && !sm1_irq_armed);
    arm(0); /* cold completion with no interrupt */
    latch = 4;
    assert(sm1_irq_wait(50000) && !sm1_irq_armed);
    arm(0); /* timer wrap and timeout */
    USEC_TIMER = UINT32_MAX - 9000;
    assert(!sm1_irq_wait(30000) && sleeps == 1);
    unsigned count = sm1_irq_count;
    INT_IRQ18();
    assert(!sm1_done.token && sm1_irq_count == count);
    arm(1); sm1_irq_disarm(); INT_IRQ18();
    assert(!sm1_done.token && !sm1_irq_armed);
    arm(1); assert(sm1_irq_wait(50000));
}
