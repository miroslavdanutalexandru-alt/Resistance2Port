/* RSX_FIFO_RETURN_CAPTURE (third_party/rpcs3/rpcs3/Emu/RSX/port_fifo_return_capture.h): the puller runs the
 * detour's return JUMP even when the game reused the segment end before the puller saw PUT move.
 *
 * Replays R3 T2 desync #2 (2026-11-17, diner): J 0x39ffd20->0x80a100 (the detour entry), segment to PUT 0x80a138,
 * GET published 0x80a138 == PUT, the game moved PUT on and reused 0x80a138 for data 0x3a949c00, which the puller then
 * ran as an old-style JUMP to 0x1a949c00. */
#include <stdio.h>
#include <stdint.h>
#include "port_fifo_return_capture.h"
#include "port_wait_get.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define ENTRY 0x039FFD20u   /* the JUMP into the segment (G0) */
#define E     0x0080A138u   /* segment end = PUT while the puller is parked */
#define RET   0x239FFD20u   /* old-style JUMP back to G0, written at E before PUT = E */
#define DATA  0x3A949C00u   /* what the game wrote at E after it moved PUT */
#define IO_MAPPED 0x04000000u /* the R3 IO mapping (1 MB MapEaIoAddress blocks from io 0) in this model */

/* RSXFIFO.cpp: the live word is a RET, or a JUMP/CALL whose target is mapped IO */
static int live(uint32_t w)
{
    uint32_t t = 0;
    const int f = port_rc_flow(w, &t);
    return f == 2 || (f == 1 && t < IO_MAPPED);
}
#define SUB(ca, cw, a, n, p, w) port_rc_substitute(ca, cw, a, n, p, w, live(w))

/* The puller parks at E (capture), PUT moves to put_after, the word at E becomes now; returns the word it runs. */
static uint32_t replay(int fix, uint32_t entry, uint32_t parked_word, uint32_t put_after, uint32_t now)
{
    uint32_t cap_addr = PORT_RC_NONE, cap_word = 0;
    if (fix && port_rc_is_return(entry, E, parked_word)) { cap_addr = E; cap_word = parked_word; }
    if (SUB(cap_addr, cap_word, E, E, put_after, now)) return cap_word;
    return now;
}

/* Semaphore-acquire path (FIFO_control::sync_get_for_wait_late + publish_get): the segment ends with an NV406E acquire
 * at E-8 whose label is late; R3_FIFO_WAIT_GET_MS later the acquire publishes the cache end E == PUT. The guest drain
 * reacts at once (worst case): PUT = G0 and E reused before the acquire is released and the puller fetches E.
 * capture_first = the fixed order (capture, then release); 0 = capture after the release (the reviewed bug). */
static uint32_t g_put, g_mem_e;
static void guest_sees_get(uint32_t get)
{
    if (get == g_put) { g_put = 0x034FFD20u; g_mem_e = DATA; }
}
static void publish(uint32_t get, int capture_first, uint32_t* cap_addr, uint32_t* cap_word)
{
    if (capture_first && get == g_put) port_rc_note(ENTRY, get, g_mem_e, cap_addr, cap_word);
    guest_sees_get(get);
    if (!capture_first && get == g_put) port_rc_note(ENTRY, get, g_mem_e, cap_addr, cap_word);
}
static uint32_t replay_acquire(int capture_first)
{
    const uint32_t exec = E - 8u, delay = 20000u;
    uint32_t get = 0x0080A100u, cap_addr = PORT_RC_NONE, cap_word = 0, g;
    g_put = E; g_mem_e = RET;
    if ((g = port_wait_get(get, exec, E, delay, 0)) != 0) { publish(g, capture_first, &cap_addr, &cap_word); get = g; }
    CHECK(get == exec && g_put == E);              /* the early publication releases nothing */
    if ((g = port_wait_get(get, exec, E, delay, 25000u)) != 0) { publish(g, capture_first, &cap_addr, &cap_word); get = g; }
    CHECK(get == E && g_put != E && g_mem_e == DATA); /* the late one does, and E is gone */
    /* acquire released; the puller fetches the header at E */
    return SUB(cap_addr, cap_word, E, E, g_put, g_mem_e) ? cap_word : g_mem_e;
}

int main(void)
{
    /* jump decoding */
    CHECK(port_rc_jump_target(RET) == ENTRY);
    CHECK(port_rc_jump_target(ENTRY | 1u) == ENTRY);           /* new-style JUMP */
    CHECK(port_rc_jump_target(0x82100000u) == PORT_RC_NONE);   /* CALL */
    CHECK(port_rc_jump_target(0x00041FCCu) == PORT_RC_NONE);   /* method header */
    CHECK(port_rc_jump_target(0x00020000u) == PORT_RC_NONE);   /* RET */

    /* the desync: without the capture the puller runs the data word */
    CHECK(replay(0, ENTRY, RET, 0x034FFD20u, DATA) == DATA);
    CHECK(replay(1, ENTRY, RET, 0x034FFD20u, DATA) == RET);
    CHECK(replay(1, ENTRY, RET, ENTRY, DATA) == RET);           /* PUT back at G0 (the routine's own move) */
    CHECK(replay(1, ENTRY, ENTRY | 1u, ENTRY, DATA) == (ENTRY | 1u));

    /* a puller in time reads the unchanged word itself */
    CHECK(!SUB(E, RET, E, E, ENTRY, RET));

    /* the game appended commands at E and moved PUT forward inside the segment: the new word is real */
    CHECK(replay(1, ENTRY, RET, E + 0x40u, 0x00041FCCu) == 0x00041FCCu);
    CHECK(replay(1, ENTRY, RET, E + PORT_RC_APPEND_WINDOW, 0x00041FCCu) == 0x00041FCCu);

    /* nothing captured unless the parked word jumps back to the entry */
    CHECK(!port_rc_is_return(ENTRY, E, DATA));                  /* data that decodes as a JUMP elsewhere */
    CHECK(!port_rc_is_return(ENTRY, E, 0x00041FCCu));
    CHECK(!port_rc_is_return(PORT_RC_NONE, E, RET));            /* no JUMP taken yet */
    CHECK(!port_rc_is_return(E, E, 0x20000000u | E));           /* a park on a self-JUMP */
    CHECK(replay(1, 0x03590684u, RET, 0x034FFD20u, DATA) == DATA);

    /* the capture only applies to the parked address as a command header */
    CHECK(!SUB(E, RET, E + 4u, E + 4u, ENTRY, DATA));
    CHECK(!SUB(E, RET, E, E - 4u, ENTRY, DATA));
    CHECK(!SUB(PORT_RC_NONE, RET, E, E, ENTRY, DATA));

    /* R3 T1 freeze (run.log 11/17): capture JUMP 0x39ffd20 at E 0xaba538, then the game re-linked E as JUMP 0x80a100
     * with PUT 0x80a138. Running the capture looped the RSX over old ring content forever: the live word must win. */
    {
        const uint32_t e = 0x00ABA538u, relink = 0x2080A100u, put = 0x0080A138u;
        CHECK(live(relink));
        CHECK(!SUB(e, RET, e, e, put, relink));
        CHECK(!port_rc_substitute(e, RET, e, e, put, relink, 0)); /* PUT is far from the captured target's ring too */
        CHECK(!SUB(e, RET, e, e, 0x034FFD20u, relink));           /* even with PUT in the ring */
        CHECK(!SUB(E, RET, E, E, 0x034FFD20u, 0x0080A101u));      /* new-style JUMP */
        CHECK(!SUB(E, RET, E, E, 0x034FFD20u, 0x0080A102u));      /* CALL */
        CHECK(!SUB(E, RET, E, E, 0x034FFD20u, 0x00020000u));      /* RET */
        /* data that only decodes as a JUMP (unmapped target) is still overridden: T2 desyncs #1/#2/#4 */
        CHECK(!live(DATA) && !live(0x37EFB400u) && !live(0x3AA60580u));
        CHECK(SUB(E, RET, E, E, 0x034FFD20u, 0x37EFB400u));
        CHECK(SUB(E, RET, E, E, 0x034FFD20u, 0x3AA60580u));
        /* PUT just below E: the game restarted the segment, its new word is real */
        CHECK(!SUB(E, RET, E, E, E - 0x38u, DATA));
        /* PUT far from the ring the capture returns to */
        CHECK(!SUB(E, RET, E, E, ENTRY + PORT_RC_TARGET_SPAN, DATA));
        CHECK(!SUB(E, RET, E, E, ENTRY - PORT_RC_TARGET_SPAN, DATA));
    }

    /* CALL target uses RSX_METHOD_CALL_OFFSET_MASK (0x1ffffffc) */
    {
        uint32_t tg = 0;
        CHECK(port_rc_flow(0xA0100002u, &tg) == 1 && tg == 0x00100000u);
        CHECK(port_rc_flow(0x00020000u, &tg) == 2);
        CHECK(port_rc_flow(0x00041FCCu, &tg) == 0);
    }

    /* loop detector (progress based). T1 freeze: the substituted JUMP lands on 0x39ffd20 (the ring's wrap JUMP), the
     * puller jumps to the ring start, runs the old ring and jumps to the ring start again with PUT unchanged: a loop. */
    {
        const uint32_t e = 0x00ABA538u, put = 0x0080A138u, ring = 0x03000000u;
        port_rc_loop l;
        port_rc_loop_arm(&l, e, RET, put, DATA);
        CHECK(!port_rc_loop_jump(&l, ENTRY, put));      /* the substituted JUMP itself */
        CHECK(!port_rc_loop_jump(&l, ring, put));       /* first lap */
        CHECK(port_rc_loop_jump(&l, ring, put));        /* second lap: trip, resume at E */
        CHECK(l.e == e && !l.armed);
        CHECK(!port_rc_loop_jump(&l, ring, put));       /* disarmed after the trip */

        port_rc_loop_arm(&l, e, RET, put, DATA);              /* coming back to the target itself also trips */
        CHECK(!port_rc_loop_jump(&l, ENTRY, put));
        CHECK(!port_rc_loop_jump(&l, ring, put));
        CHECK(port_rc_loop_jump(&l, ENTRY, put));
    }
    {
        /* where the trip resumes: E only if the game re-linked it with a JUMP/CALL to mapped IO, else PUT */
        const uint32_t e = 0x00ABA538u, put = 0x0080A138u;
        port_rc_loop l;
        port_rc_loop_arm(&l, e, RET, put, DATA);
        CHECK(port_rc_resume(&l, 0x2080A100u, live(0x2080A100u), put) == e); /* T1: re-linked as JUMP 0x80a100 */
        CHECK(port_rc_resume(&l, 0x00020000u, live(0x00020000u), put) == put); /* RET: no CALL left after the reset */
        CHECK(port_rc_resume(&l, 0xA0100002u, live(0xA0100002u), put) == e); /* CALL to mapped IO */
        CHECK(port_rc_resume(&l, 0x00041FCCu, live(0x00041FCCu), put) == put); /* a changed method word: PUT did not move, not a link */
        CHECK(port_rc_resume(&l, 0u, live(0u), put) == put);                 /* zero / NOP */
        CHECK(port_rc_resume(&l, DATA, live(DATA), put) == put);             /* unchanged: would fault into the old lap */
        CHECK(port_rc_resume(&l, 0x3AA60580u, live(0x3AA60580u), put) == put); /* other garbage (unmapped JUMP bits) */
        port_rc_loop_arm(&l, e, RET, put, 0x00041FCCu);
        CHECK(port_rc_resume(&l, 0x00041FCCu, live(0x00041FCCu), put) == put); /* same stale header: old segment */
    }
    {
        /* legitimate: a long semaphore wait (no jumps, any duration) and then the puller reaches PUT */
        const uint32_t put = 0x034FFD20u, ring = 0x03000000u;
        port_rc_loop l;
        port_rc_loop_arm(&l, E, RET, put, DATA);
        CHECK(!port_rc_loop_jump(&l, ENTRY, put));
        CHECK(!port_rc_loop_jump(&l, ring, put));
        CHECK(!port_rc_loop_jump(&l, 0x0080A100u, put)); /* a detour segment on the way, then its return */
        CHECK(!port_rc_loop_jump(&l, 0x03123450u, put));
        /* ... NV406E acquire blocks here for seconds: no jumps, no trip ... */
        port_rc_loop_parked(&l);                         /* GET == PUT */
        CHECK(!port_rc_loop_jump(&l, ring, put));        /* the next lap is a new frame, not a loop */

        /* the game moving PUT disarms it as well */
        port_rc_loop_arm(&l, E, RET, put, DATA);
        CHECK(!port_rc_loop_jump(&l, ENTRY, put));
        CHECK(!port_rc_loop_jump(&l, ring, put));
        CHECK(!port_rc_loop_jump(&l, ring, put + 0x100u));
        CHECK(!port_rc_loop_jump(&l, ring, put + 0x100u));
    }

    /* semaphore acquire: captured before its GET is visible, not after */
    CHECK(replay_acquire(1) == RET);
    CHECK(replay_acquire(0) == DATA);

    /* idle polls: a parked word rewritten while PUT still points at it drops the capture */
    {
        uint32_t ca = PORT_RC_NONE, cw = 0;
        port_rc_note(ENTRY, E, RET, &ca, &cw);
        CHECK(ca == E && cw == RET);
        port_rc_note(ENTRY, E, 0x00041FCCu, &ca, &cw);
        CHECK(ca == PORT_RC_NONE);
    }

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("fifo return capture: all checks passed\n");
    return 0;
}
