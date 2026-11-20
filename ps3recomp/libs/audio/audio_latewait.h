/*
 * ps3recomp - the mix thread's late-block wait budget (cellAudio.c, PORT_AUDIO_LATE_WAIT_MS).
 *
 * After a concealed guest block the mixer may wait for a block that is still
 * unwritten at its read. Every wait slips the guest audio clock, and so drains
 * the host buffer by the time waited. The floor (PORT_AUDIO_LATE_KEEP_PCT, % of
 * the target buffer, default = the stretcher threshold + 15, i.e. 90) stops the
 * waits before the buffer reaches the stretch zone; the margin covers the level
 * falling a further device period (10 ms) + block after a wait ends (75 alone
 * still dipped to ~64 ms / 0.75x in the simulation). With the old 4-block floor
 * (~21 ms, PORT_AUDIO_LATE_KEEP_PCT=0) a run of late blocks (heavy scenes, ADS
 * with an expensive upscaler) pinned the buffer at the floor and the
 * SoundTouch stage at ~0.5x tempo ("broken"/slow audio) for as long as blocks
 * kept being concealed; only a pause (F10) refilled it. test_audio_latewait.cpp
 * simulates both.
 */
#ifndef PS3RECOMP_AUDIO_LATEWAIT_H
#define PS3RECOMP_AUDIO_LATEWAIT_H

#define AUDIO_LATE_KEEP_MARGIN_PCT  15
#define AUDIO_LATE_KEEP_PCT_DEFAULT (75 + AUDIO_LATE_KEEP_MARGIN_PCT)

/* frames the wait must leave in the host buffer */
static inline unsigned audio_late_keep_frames(unsigned target_frames, int keep_pct, unsigned block_frames)
{
    unsigned keep = 4u * block_frames;
    if (keep_pct > 0 && keep_pct <= 100) {
        const unsigned k2 = (unsigned)((unsigned long long)target_frames * (unsigned)keep_pct / 100u);
        if (k2 > keep) keep = k2;
    }
    return keep;
}

/* microseconds the mixer may wait for a late block in total (compare with the time already waited) */
static inline long long audio_late_wait_budget_us(unsigned queued, unsigned keep, int late_ms, unsigned rate)
{
    long long budget = (long long)late_ms * 1000;
    const long long cushion = queued > keep ? (long long)(queued - keep) * 1000000 / rate : 0;
    return budget < cushion ? budget : cushion;
}
/* PORT_AUDIO_STALL_FREEZE_MS decision (cellAudio.c audio_stall_hold): hold the guest audio
 * clock while no game frame has completed for `ms` AND a port that had been playing has been
 * concealed for at least `ms`, its concealment having started at most 200 ms before the last
 * frame (a port the game simply left idle is concealed forever and must not count). Released
 * on the next frame or after AUDIO_STALL_HOLD_MAX_MS (then not again until a frame completes).
 * runs[p]: consecutive concealed blocks of port p, 0 for a port that never played.
 * Returns 1 = hold this tick; *event: 1 started, 2 released by a frame, 3 released by the cap. */
#define AUDIO_STALL_HOLD_MAX_MS 3000
typedef struct { long frame; unsigned long long frame_t, hold_t; int holding, cooldown, init; } AudioStallState;
static inline int audio_stall_step(AudioStallState* s, unsigned long long now_ms, long frame,
                                   const unsigned* runs, int nports, unsigned block_frames, unsigned rate,
                                   int ms, int* event)
{
    *event = 0;
    if (ms <= 0) return 0;
    if (!s->init || frame != s->frame) {
        s->init = 1; s->frame = frame; s->frame_t = now_ms; s->cooldown = 0;
        if (s->holding) { s->holding = 0; *event = 2; }
        return 0;
    }
    if (s->holding) {
        if (now_ms - s->hold_t < AUDIO_STALL_HOLD_MAX_MS) return 1;
        s->holding = 0; s->cooldown = 1; *event = 3;
        return 0;
    }
    const unsigned long long stalled = now_ms - s->frame_t;
    if (s->cooldown || frame <= 0 || stalled < (unsigned long long)ms) return 0;
    const unsigned long long bpms = (unsigned long long)block_frames * 1000;   /* need / most: blocks of block_frames at rate */
    const unsigned need = (unsigned)(((unsigned long long)ms * rate + bpms - 1) / bpms);
    const unsigned most = (unsigned)((stalled + 200) * rate / bpms);
    for (int p = 0; p < nports; p++)
        if (runs[p] >= need && runs[p] <= most) { s->holding = 1; s->hold_t = now_ms; *event = 1; return 1; }
    return 0;
}
#endif
