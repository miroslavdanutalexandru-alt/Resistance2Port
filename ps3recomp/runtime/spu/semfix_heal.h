/* R3_SEMFIX_HEAL=<n> (default 3; 0 = off; R2_SEMFIX_HEAL alias): self-heal of
 * the R3_SEMFIX_MS wait cap (spu_channels.c). n capped waits in a row on one
 * label with no SPU store to it in between mean the store the acquire waits for
 * is not coming at all (2026-11-17 5 fps log: after a FIFO desync cascade every
 * label-77 acquire of the following frames waited the full 150 ms cap, the
 * circular-wait detection never fired because no store came even after the RSX
 * moved on). The label then gets the short cap (R3_SEMFIX_MIN_MS, as for a
 * detected circular wait) until an acquire of it is satisfied again. Waiting
 * longer changes nothing in that state: the RSX proceeds after the cap either way. */
#ifndef SEMFIX_HEAL_H
#define SEMFIX_HEAL_H

#include <stdint.h>

typedef struct {
    uint8_t run;      /* capped waits in a row, no store in between */
    uint8_t healed;   /* 1 = short cap until a satisfied acquire */
} semfix_heal_t;

/* a capped wait gave up; 1 = this cap switched the label to the short cap */
static inline int semfix_heal_cap(semfix_heal_t* h, unsigned need)
{
    if (!need) return 0;
    if (h->run < 255u) h->run++;
    if (h->healed || h->run < need) return 0;
    h->healed = 1;
    return 1;
}

/* a capped wait gave up: may it arm the circular-wait detection (a store within
 * 100 ms of the cap -> permanent short cap)? Only a long cap may. A heal short
 * cap (a few ms) also cuts off legitimately slower stores, which would otherwise
 * latch the label on the short cap for the session. Call before semfix_heal_cap. */
static inline int semfix_heal_arms_circ(const semfix_heal_t* h) { return !h->healed; }

/* a store landed on the label (it is still being signalled) */
static inline void semfix_heal_store(semfix_heal_t* h) { h->run = 0; }

/* an acquire of the label was satisfied: back to the normal cap */
static inline void semfix_heal_satisfied(semfix_heal_t* h) { h->run = 0; h->healed = 0; }

/* cap in us: the long cap, or (short mode, min_ms != 0) max(min, 1.25 x longest
 * recent satisfied wait + 1 ms), never above the long cap */
static inline uint64_t semfix_heal_pick_cap(uint64_t cap_us, int short_mode, uint64_t ok_max_us, long min_ms)
{
    if (!short_mode || min_ms <= 0) return cap_us;
    uint64_t c = ok_max_us + (ok_max_us >> 2) + 1000u;
    if (c < (uint64_t)min_ms * 1000u) c = (uint64_t)min_ms * 1000u;
    return c < cap_us ? c : cap_us;
}

#endif
