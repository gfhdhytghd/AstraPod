/* Boot policy for the isolated SM1 research firmware. */
#ifndef SM1_RECOVERY_H
#define SM1_RECOVERY_H
#include <stdint.h>
#include <stdbool.h>
enum recovery_phase { REC_NORMAL, REC_CRASH, REC_DISK, REC_HOLD };
#define REC_LIMIT 3u
#define REC_MAGIC 0x52454332u
struct recovery_record {
    uint32_t boot, count, phase, check;
};
static inline uint32_t recovery_check(uint32_t boot, uint32_t count, uint32_t phase)
{
    return REC_MAGIC ^ boot ^ (count << 8) ^ (phase << 16);
}
static inline bool recovery_valid(const volatile struct recovery_record *r)
{
    return r->phase <= REC_HOLD && r->count <= REC_LIMIT &&
           r->check == recovery_check(r->boot, r->count, r->phase);
}
static inline void recovery_store(volatile struct recovery_record *r, uint32_t boot,
                                  unsigned count, enum recovery_phase phase)
{
    r->check = 0;
    r->boot = boot;
    r->count = count;
    r->phase = phase;
    r->check = recovery_check(boot, count, phase);
}
static inline void recovery_boot(volatile struct recovery_record *r, uint32_t boot,
                                 bool previous_unclean)
{
    unsigned count = 0;
    enum recovery_phase phase = REC_NORMAL;
    if (previous_unclean && recovery_valid(r) && r->boot + 1u == boot) {
        if (r->phase == REC_CRASH) {
            count = r->count;
            if (count == REC_LIMIT)
                phase = REC_DISK;
        } else if (r->phase == REC_DISK || r->phase == REC_HOLD) {
            phase = REC_HOLD;
        }
    }
    recovery_store(r, boot, count, phase);
}
static inline bool recovery_fault(volatile struct recovery_record *r, uint32_t boot,
                                  bool rapid)
{
    if (!recovery_valid(r) || r->boot != boot || r->phase != REC_NORMAL)
        return false;
    recovery_store(r, boot, rapid ? r->count + 1 : 0, REC_CRASH);
    return true;
}
#endif
