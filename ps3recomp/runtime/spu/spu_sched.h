/* spu_sched.h -- PORT_SPU_SCHED: how long a blocked SPU channel read sleeps
 * on ch_wait_cv before it re-checks its predicate (spu_ch_wait, spu_channels.c).
 *
 * A wait on a lost-reservation event (rdch SPU_RdEventStat with 0x400 in the
 * mask after a GETLLAR -- the SPURS/ig job queue and lock pattern) is woken by
 * spu_coh_notify_write, which every coherent writer calls: PPU stores to a
 * reserved line, SPU PUT/PUTLLC, cellSync/cellGcm HLE. A writer that bypasses
 * that path (a host memcpy into guest memory, a syscall writing guest memory)
 * is only noticed when the waiter polls the line (spu_resv_lost_poll), and the
 * waiter used to poll once per 10 ms CV timeout. PORT_SPU_SCHED (default on)
 * re-polls those waits every PORT_SPU_SCHED_MS (default 1, 1..10), so a missed
 * wake costs <= 1 ms instead of <= 10 ms. Purely a latency change: the
 * predicate is re-checked either way and a wait still ends only when it holds.
 * Other waits (mailbox, signal, other events) keep the 10 ms timeout -- their
 * producers all call spu_ch_wake.
 *
 * Shared runtime: the Resistance 1/2 copies of ps3recomp have the same wait. */
#pragma once
#include <stdint.h>
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

enum { SPU_SCHED_SLOW_MS = 10, SPU_SCHED_LR_EVENT = 0x400u };

/* One switch parser in every module: port_native_parse_bool (1/on/true/yes,
 * 0/off/false/no; anything else = def). */
static inline int spu_sched_parse_bool(const char* e, int def) { return port_native_parse_bool(e, def); }

/* PORT_SPU_SCHED_MS: 1..10, anything else = 1. */
static inline int spu_sched_parse_ms(const char* e)
{
    int v = 0, n = 0;
    if (!e) return 1;
    for (; *e >= '0' && *e <= '9' && n < 3; e++, n++) v = v * 10 + (*e - '0');
    if (*e || !n || v < 1 || v > SPU_SCHED_SLOW_MS) return 1;
    return v;
}

/* CV timeout for one sleep of spu_ch_wait; is_event_read: the read is SPU_RdEventStat. */
static inline unsigned spu_sched_wait_ms(int on, int ms, int is_event_read,
                                         uint32_t event_mask, int resv_valid)
{
    if (on && is_event_read && (event_mask & SPU_SCHED_LR_EVENT) && resv_valid) return (unsigned)ms;
    return SPU_SCHED_SLOW_MS;
}

/* PORT_SPU_SCHED_IDLE_MS (opt-in; default 1 = off, 0/off = off, 2..10 = cap):
 * idle backoff of that re-poll. Off by default: a missed wake (gameplay sees
 * ~0.5/s on one line) is then caught up to the cap late instead of 1 ms, and
 * the saving is only ~1-2% of a core. A notified writer wakes the CV at once, so the
 * timeout only bounds how late a write that bypassed spu_coh_notify_write is
 * seen. Idle SPURS kernels (img 2) sleep in this wait all the time: ~3.5k
 * timer wakes/s in the menu (39k sleeps per 10 s, 35k of them timeouts, zero
 * missed wakes). After SPU_SCHED_IDLE_AFTER lost-reservation sleeps in a row
 * on one thread that all ended on the timeout (truly idle), the timeout
 * doubles per further SPU_SCHED_IDLE_AFTER such sleeps up to the cap; a
 * normal wake (busy worker) resets the streak to the short poll, and a missed
 * wake drops that thread back to it for SPU_SCHED_IDLE_HOLD sleeps. Latency
 * only: the predicate is re-checked on every wake and timeout as before. */
enum { SPU_SCHED_IDLE_AFTER = 64, SPU_SCHED_IDLE_HOLD = 1024, SPU_SCHED_IDLE_DEF_MS = 1 };

/* PORT_SPU_SCHED_IDLE_MS: 0/off/false/no = off, 1..10, anything else = the
 * default (1 = off). */
static inline int spu_sched_parse_idle_ms(const char* e)
{
    if (e && !port_native_parse_bool(e, 1)) return 0;
    if (!e || !*e) return SPU_SCHED_IDLE_DEF_MS;
    int v = 0, n = 0;
    for (; *e >= '0' && *e <= '9' && n < 3; e++, n++) v = v * 10 + (*e - '0');
    if (*e || !n || v > SPU_SCHED_SLOW_MS) return SPU_SCHED_IDLE_DEF_MS;
    return v;
}

/* Timeout of a lost-reservation sleep after `quiet` such sleeps in a row that
 * ended on the timeout (negative: holding after a missed wake). */
static inline unsigned spu_sched_idle_ms(unsigned ms, int quiet, int idle_ms)
{
    if (idle_ms <= (int)ms || quiet < SPU_SCHED_IDLE_AFTER) return ms;
    unsigned v = ms;
    for (int q = quiet; q >= SPU_SCHED_IDLE_AFTER && v < (unsigned)idle_ms; q -= SPU_SCHED_IDLE_AFTER) v *= 2;
    return v < (unsigned)idle_ms ? v : (unsigned)idle_ms;
}

/* The per-thread streak after one lost-reservation sleep; timed_out = it
 * ended on the timeout rather than a notify. A woken sleep ends the streak
 * (a hold in progress keeps its place). */
static inline int spu_sched_idle_next(int quiet, int timed_out, int missed)
{
    if (missed) return -SPU_SCHED_IDLE_HOLD;
    if (!timed_out) return quiet < 0 ? quiet : 0;
    return quiet < (1 << 20) ? quiet + 1 : quiet;
}
