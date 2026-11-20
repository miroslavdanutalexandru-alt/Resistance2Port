/*
 * ps3recomp - guest audio rate check for the [audio] log line (log only).
 *
 * The PS3 consumes one 256-frame block every 5.33 ms: 187.5 blocks/s at
 * 48 kHz. When the guest producer (R2: the SPU synth job list) cannot write
 * blocks that fast, the late-block wait slips the mixer clock and the output
 * stretcher plays the rest slower (pitch kept): "slow audio". This measures
 * it: guest blocks with real data per second of unpaused time, as a percent
 * of realtime.
 */
#ifndef PS3RECOMP_AUDIO_RATE_H
#define PS3RECOMP_AUDIO_RATE_H

#define AUDIO_RATE_BLOCKS_PER_S 187.5   /* 48000 / 256 */

/* consumed: guest blocks the mixer read (unpaused ticks); concealed: those
 * that had no guest data in time; active_ms: wall time minus paused time.
 * Returns the percent of realtime the guest produced (100 = keeps up), or
 * -1 when the window is too short to say. */
static inline double audio_rate_pct(unsigned consumed, unsigned concealed, double active_ms)
{
    if (active_ms < 1000.0) return -1.0;
    const unsigned written = concealed < consumed ? consumed - concealed : 0u;
    return written * 100000.0 / (active_ms * AUDIO_RATE_BLOCKS_PER_S);
}

/* Below realtime enough to hear: under 97 % the 3 % catch-up cannot make it
 * up, so the clock slips and the output plays stretched. */
static inline int audio_rate_slow(double pct)
{
    return pct >= 0.0 && pct < 97.0;
}

#endif /* PS3RECOMP_AUDIO_RATE_H */
