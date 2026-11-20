/*
 * Offline test for the mix thread's late-block wait (cellAudio.c, audio_latewait.h)
 * feeding the SoundTouch output stage (audio_out.cpp).
 *
 * Models the steady mix loop: a 48 kHz device playing 480 frames every 10 ms,
 * the guest block clock (256 frames, 3 % catch-up below the target buffer,
 * resync when > 50 ms behind), the late wait (armed 5 s after a concealed
 * block, 0.5 ms steps, budget from audio_late_wait_budget_us) and a guest
 * producer that writes block k a fixed lead after the notify of block k-6
 * plus a processing latency. The latency is normal (1 ms), then a heavy phase
 * (aiming with an expensive upscaler: 20-70 ms with 120 ms spikes), then
 * normal again. Checks the buffer stays out of the stretch zone with the
 * default floor, and that the old 4-block floor reproduces the ~0.5x state.
 */
#include "../audio_out.h"
#include "../audio_latewait.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <deque>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static const unsigned RATE = 48000, PERIOD = 480, BLOCK = 256, TARGET_MS = 100, LEAD = 6, READ_DELAY = 6;
static const int LATE_MS = 22;

struct Rng { unsigned s; double next() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; } };

struct Result {
    double tempo_min = 1.0, stretched_pct = 0, heavy_tempo_avg = 0;
    double level_min_ms = 1e9, recover_ms = -1;
    unsigned concealed = 0, waits = 0, caught = 0, underruns = 0, blocks = 0;
};

/* processing latency (us) of the block whose notify came at t_us. Heavy profile 0: every
 * block 20-70 ms late with 120 ms spikes; 1 (mild): 15 % of blocks 30-50 ms, 1 % 80 ms. */
static int g_profile = 0;
static long long latency_us(long long t_us, long long heavy0, long long heavy1, Rng& r)
{
    if (t_us < heavy0 || t_us >= heavy1) return 1000;
    if (g_profile == 1) {
        const double x = r.next();
        return x < 0.01 ? 80000 : x < 0.15 ? 30000 + (long long)(20000 * r.next()) : 5000;
    }
    if (r.next() < 0.04) return 120000;
    return 20000 + (long long)(50000 * r.next());
}

static Result simulate(int keep_pct, unsigned seed, long long heavy0, long long heavy1, long long total)
{
    Result R;
    AudioOutConfig cfg;
    audio_out_default_config(&cfg, RATE, TARGET_MS);
    audio_out_env_config(&cfg, PERIOD);
    AudioOut* ao = audio_out_create(&cfg);
    const unsigned target = TARGET_MS * RATE / 1000;
    const unsigned keep = audio_late_keep_frames(target, keep_pct, BLOCK);
    const long long period_us = 1000000LL * BLOCK / RATE;   /* 5333 */
    Rng rng{seed};

    std::vector<long long> notify;   /* tick time of block k */
    std::vector<long long> written;  /* when block k lands */
    std::deque<float> dev;
    std::vector<float> blk(BLOCK * 2), tmp(8192 * 2);
    long long due = 0, wait0 = -1, last_conceal = -10000000, next_dev = 10000, last_stretch = -1;
    unsigned k = 0, ticks_heavy = 0; double tsum_heavy = 0; unsigned stretch_ticks = 0, all_ticks = 0;
    double ph = 0;

    auto write_time = [&](unsigned b) -> long long {
        while (written.size() <= b) {
            const unsigned j = (unsigned)written.size();
            if (j < LEAD) { written.push_back(0); continue; }
            if (notify.size() <= j - LEAD) return -1;      /* its notify has not happened yet */
            const long long n = notify[j - LEAD];
            written.push_back(n + latency_us(n, heavy0, heavy1, rng));
        }
        return written[b];
    };

    for (long long t = 0; t <= total; t += 500) {
        /* device consumes one period every 10 ms */
        if (t >= next_dev) {
            next_dev += 10000;
            for (unsigned i = 0; i < PERIOD && !dev.empty(); i++) { dev.pop_front(); dev.pop_front(); }
        }
        /* keep the device topped up from the output stage */
        for (;;) {
            const unsigned dq = (unsigned)dev.size() / 2;
            if (dq >= cfg.device_frames) break;
            const unsigned n = audio_out_render(ao, tmp.data(), cfg.device_frames - dq, dq);
            if (!n) break;
            dev.insert(dev.end(), tmp.begin(), tmp.begin() + n * 2);
        }
        const unsigned dq = (unsigned)dev.size() / 2;
        const unsigned queued = audio_out_level(ao, dq);
        if (t < due) continue;
        if (wait0 < 0 && t - due > 50000) due = t;
        /* late wait */
        const long long wt = write_time(k);
        const bool pending = wt < 0 || wt > t;
        if (t - last_conceal < 5000000 && pending && k > 64) {
            if (wait0 < 0) { wait0 = t; R.waits++; }
            if (t - wait0 < audio_late_wait_budget_us(queued, keep, LATE_MS, RATE)) continue;
        } else if (wait0 >= 0) R.caught++;
        if (wait0 >= 0) { const long long slip = t - wait0; due = due + slip < t ? due + slip : t; wait0 = -1; }
        long long per = period_us;
        if (queued < target) per -= period_us * 3 / 100;
        else if (queued > target + target / 2) per += period_us * 3 / 100;
        due += per;
        /* consume block k: played if it lands before the delayed snapshot, else concealed (silence) */
        const long long w = write_time(k);
        const bool real = k < LEAD || (w >= 0 && w <= t + (long long)READ_DELAY * period_us);
        for (unsigned i = 0; i < BLOCK; i++) {
            const float v = real ? 0.5f * (float)sin(ph) : 0.0f;
            ph += 2 * 3.14159265358979 * 440.0 / RATE;
            blk[i * 2] = blk[i * 2 + 1] = v;
        }
        if (!real && k > 64) { R.concealed++; last_conceal = t; }
        audio_out_push(ao, blk.data(), BLOCK);
        audio_out_tick(ao, dq);
        notify.push_back(t);
        k++; R.blocks++;
        /* stats once playback is running */
        if (t > 1000000) {
            const double tp = audio_out_tempo(ao), lms = queued * 1000.0 / RATE;
            if (tp < R.tempo_min) R.tempo_min = tp;
            if (lms < R.level_min_ms) R.level_min_ms = lms;
            all_ticks++; if (tp < 1.0) { stretch_ticks++; last_stretch = t; }
            if (t >= heavy0 && t < heavy1) { ticks_heavy++; tsum_heavy += tp; }
        }
    }
    AudioOutStats st; audio_out_take_stats(ao, &st);
    R.underruns = st.underruns;
    R.stretched_pct = all_ticks ? 100.0 * stretch_ticks / all_ticks : 0;
    R.heavy_tempo_avg = ticks_heavy ? tsum_heavy / ticks_heavy : 1.0;
    R.recover_ms = last_stretch < heavy1 ? 0 : (last_stretch - heavy1) / 1000.0;
    audio_out_destroy(ao);
    return R;
}

int main()
{
    printf("test_audio_latewait: late-block wait vs the SoundTouch stage (target %u ms, wait %d ms)\n", TARGET_MS, LATE_MS);
    const long long H0 = 3000000, H1 = 13000000, TOTAL = 20000000;
    {   /* no heavy phase: nothing late, nothing stretched */
        Result r = simulate(AUDIO_LATE_KEEP_PCT_DEFAULT, 1, TOTAL + 1, TOTAL + 1, 8000000);
        printf("steady       : tempo min %.2f, concealed %u, waits %u, level min %.0f ms\n", r.tempo_min, r.concealed, r.waits, r.level_min_ms);
        CHECK(r.tempo_min == 1.0 && r.concealed == 0 && r.waits == 0, "steady: tempo %.2f concealed %u waits %u", r.tempo_min, r.concealed, r.waits);
    }
    if (getenv("LW_SWEEP")) /* LW_SWEEP=0|1: floor sweep for a profile */ { g_profile = atoi(getenv("LW_SWEEP")); for (int p = 0; p <= 100; p += 5) { Result s = simulate(p, 12345, H0, H1, TOTAL);
        printf("pct %3d: tempo min %.2f avg heavy %.2f level min %.0f concealed %u waits %u caught %u\n", p, s.tempo_min, s.heavy_tempo_avg, s.level_min_ms, s.concealed, s.waits, s.caught); } }
    const unsigned seeds[] = { 12345, 7, 4242 };
    for (int prof = 0; prof < 2; prof++) for (unsigned seed : seeds) {
        g_profile = prof;
        Result o = simulate(0, seed, H0, H1, TOTAL);
        Result n = simulate(AUDIO_LATE_KEEP_PCT_DEFAULT, seed, H0, H1, TOTAL);
        printf("%s seed %5u old floor (KEEP_PCT=0): tempo min %.2f, avg while heavy %.2f, stretched %.0f%%, level min %.0f ms, "
               "concealed %u, waits %u (caught %u), underruns %u, stretched until %.0f ms after\n",
               prof ? "mild " : "heavy", seed, o.tempo_min, o.heavy_tempo_avg, o.stretched_pct, o.level_min_ms, o.concealed, o.waits, o.caught, o.underruns, o.recover_ms);
        printf("%s seed %5u default (KEEP_PCT=%d): tempo min %.2f, avg while heavy %.2f, stretched %.0f%%, level min %.0f ms, "
               "concealed %u, waits %u (caught %u), underruns %u, stretched until %.0f ms after\n",
               prof ? "mild " : "heavy", seed, AUDIO_LATE_KEEP_PCT_DEFAULT, n.tempo_min, n.heavy_tempo_avg, n.stretched_pct, n.level_min_ms, n.concealed, n.waits, n.caught, n.underruns, n.recover_ms);
        /* the reported state: waits pin the buffer near the 4-block floor, ~0.5x */
        CHECK(o.tempo_min < 0.7, "old floor did not reproduce the slow state (tempo min %.2f)", o.tempo_min);
        /* the fix: the waits never take the buffer into the stretch zone */
        CHECK(n.level_min_ms >= TARGET_MS * 0.75 - 6.0 - 0.0, "default: level fell to %.0f ms", n.level_min_ms);
        CHECK(n.tempo_min > 0.95, "default: stretched to %.2f", n.tempo_min);
        CHECK(n.heavy_tempo_avg > o.heavy_tempo_avg, "default: no better than the old floor");
        CHECK(n.underruns == 0, "default: %u underruns", n.underruns);
        CHECK(n.recover_ms < 1000, "default: still stretched %.0f ms after the heavy phase", n.recover_ms);
    }
    {   /* PORT_AUDIO_STALL_FREEZE_MS: hold only for a frame stall that starves a playing port */
        const int MS = 150; const unsigned need = (MS * RATE / 1000 + BLOCK - 1) / BLOCK;   /* 29 blocks */
        unsigned runs[4] = { 0, 0, 0, 0 }; int ev = 0;
        AudioStallState s = {};
        long frame = 100; unsigned long long t = 0;
        for (; t < 1000; t += 16) { frame++; CHECK(!audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev), "held while frames run"); }
        /* hitch: frames stop 300 ms but audio keeps up -> no hold */
        const unsigned long long h0 = t;
        for (; t < h0 + 300; t += 5) CHECK(!audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev), "held on a hitch with audio OK");
        frame++; audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev);
        /* idle port concealed forever (run 1000) + 300 ms hitch -> no hold */
        runs[2] = 1000;
        const unsigned long long h1 = t;
        for (; t < h1 + 300; t += 5) CHECK(!audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev), "held for an idle port");
        frame++; audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev);
        /* real stall (alt-tab + upscaler re-create): frames stop and port 0 starves with them */
        const unsigned long long s0 = t; int held = 0, started = 0;
        for (; t < s0 + 2400; t += 5) {
            runs[0] = (unsigned)((t - s0) * RATE / 1000 / BLOCK);
            const int h = audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev);
            if (ev == 1) { started++; CHECK(runs[0] >= need, "held before %u blocks", need); }
            if (h) held++;
        }
        CHECK(started == 1 && held > 400, "stall: started %d, held %d ticks", started, held);
        frame++; CHECK(!audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev) && ev == 2, "not released by a frame");
        runs[0] = 0;
        /* a stall longer than the cap: released after AUDIO_STALL_HOLD_MAX_MS, then not again */
        const unsigned long long s1 = t; int caps = 0, starts = 0;
        for (; t < s1 + 8000; t += 5) {
            runs[0] = need + 5;
            audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, MS, &ev);
            if (ev == 1) starts++; if (ev == 3) caps++;
        }
        CHECK(starts == 1 && caps == 1, "cap: starts %d caps %d", starts, caps);
        CHECK(!audio_stall_step(&s, t, frame, runs, 4, BLOCK, RATE, 0, &ev), "PORT_AUDIO_STALL_FREEZE_MS=0 held");
        printf("stall hold   : hitch/idle port not held, stall held %d ticks, cap released once\n", held);
    }
    printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
