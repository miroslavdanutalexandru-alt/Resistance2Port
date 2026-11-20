/* Resistance 2 port: [perf] slow-window verdict (R2 #15, the swarm FPS drop).
 *
 * The [thrcpu] monitor (debugtools.inc, every PS3_THREAD_CPU s, default 5) already
 * measures every host thread's CPU use. This adds the guest frame rate of the same
 * window and, when it falls well under the best rate seen in this run, one [perf]
 * line naming what limits it: the main game thread (PPU game code), the RSX render
 * thread, a SPURS lane (SPU jobs), every core busy, or no thread saturated (GPU or a
 * wait). Plain C++, no Windows calls: unit tested in project/tests/perf_verdict_test.cpp.
 *   PORT_PERF_VERDICT=0   no fps in [thrcpu] and no [perf] lines */
#ifndef PORT_PERFVERDICT_H
#define PORT_PERFVERDICT_H
#include <stdio.h>

namespace port_pv {

enum { MAX_LINES = 200 };
static const double SAT = 85.0;          /* a thread this busy (% of one core) limits the frame rate */
static const double SLOW = 0.8;          /* slow: under 80% of the best window */
static const double MIN_FPS = 8.0;       /* fewer frames: loading, pause or a movie, not gameplay */

struct Window {
    double secs;
    unsigned flips;                      /* guest flips in the window */
    bool movie;                          /* VSYNC flip mode (movies play at their own rate) */
    double main_busy, rsx_busy, spurs_max, total_busy; /* % of one core; total = sum over all threads */
    int ncpu;
};

struct State { double best_fps; int windows; int lines; };

/* fps of the window; writes a [perf] line into out (and returns true) when it is slow. */
static inline bool classify(State* st, const Window& w, double* fps_out, char* out, size_t cap)
{
    const double fps = w.secs > 0.0 ? (double)w.flips / w.secs : 0.0;
    if (fps_out) *fps_out = fps;
    if (out && cap) out[0] = 0;
    if (w.movie || fps < MIN_FPS) return false;
    st->windows++;
    if (st->windows <= 1) return false;  /* the first window still has boot / level load in it */
    const double best = st->best_fps;
    if (fps > st->best_fps) st->best_fps = fps;
    if (best < 20.0 || fps >= SLOW * best || st->lines >= MAX_LINES) return false;
    st->lines++;
    const char* what;
    double v = 0.0;
    if (w.main_busy >= SAT && w.main_busy >= w.rsx_busy && w.main_busy >= w.spurs_max) {
        what = "main game thread saturated (PPU game code: AI / animation / physics)"; v = w.main_busy;
    } else if (w.rsx_busy >= SAT && w.rsx_busy >= w.spurs_max) {
        what = "RSX render thread saturated (draw submission)"; v = w.rsx_busy;
    } else if (w.spurs_max >= SAT) {
        what = "a SPURS lane saturated (SPU jobs)"; v = w.spurs_max;
    } else if (w.ncpu > 0 && w.total_busy >= 90.0 * w.ncpu) {
        what = "all CPU cores busy"; v = w.total_busy / w.ncpu;
    } else {
        what = "no CPU thread saturated (GPU-bound or waiting)";
        v = w.main_busy > w.rsx_busy ? w.main_busy : w.rsx_busy;
    }
    if (out && cap)
        snprintf(out, cap, "[perf] slow window: %.0f fps (best %.0f) -> %s, %.0f%% | main %.0f%% render %.0f%% SPURS max %.0f%% cpus %.2f/%d\n",
                 fps, best, what, v, w.main_busy, w.rsx_busy, w.spurs_max, w.total_busy / 100.0, w.ncpu);
    return true;
}

} /* namespace port_pv */

#endif /* PORT_PERFVERDICT_H */
