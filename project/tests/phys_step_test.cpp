// Unit test for the R2 physics step fill (project/r2_physstep.h, GitHub R2 #27; no game).
// clang-cl /nologo /O1 /EHsc /std:c++17 /I project project\tests\phys_step_test.cpp /Fe:scratch\phys_step_test.exe /Fo:scratch\phys_step_test.obj
#include "r2_physstep.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// The game's accumulator in float: acc += dt; steps = floor(acc / step); acc -= steps * step.
struct sim { float acc = 0.0f; double debt = 0.0; double real = 0.0, phys = 0.0; int zero = 0, frames = 0; };

static void frame(sim& s, double dt, double step, bool fix)
{
    const double eff = fix ? (double)(float)r2_phys_fill_dt(dt, s.acc, step, &s.debt) : dt;
    float a = (float)(s.acc + (float)eff);
    const int n = (int)(a / (float)step);
    a = (float)(a - (float)n * (float)step);
    s.acc = a;
    s.real += dt; s.phys += eff; ++s.frames;
    if (n == 0) ++s.zero;
}

int main()
{
    const double step = R2_PHYS_STEP_S;
    std::srand(27);
    // 60 fps with jitter (frames just under and over 1/60 s): almost no zero-step frames with the fix
    // (a few remain when the one-step borrow cap is reached; physics never drifts ahead of real time).
    sim off, on;
    for (int i = 0; i < 6000; ++i) {
        const double dt = step * (0.85 + 0.3 * (std::rand() / (double)RAND_MAX));
        frame(off, dt, step, false);
        frame(on, dt, step, true);
    }
    CHECK(off.zero > 150);
    CHECK(on.zero * 8 < off.zero);
    CHECK(std::fabs(on.phys - on.real) <= step * 1.01);   // physics time stays in step with real time
    // 30 fps (PS3): the fix changes nothing.
    sim ps3;
    for (int i = 0; i < 600; ++i) frame(ps3, 1.0 / 30.0, step, true);
    CHECK(ps3.zero == 0 && ps3.debt == 0.0 && std::fabs(ps3.phys - ps3.real) < 1e-4);
    // 144 fps: borrowing is capped at one step, so physics never runs ahead by more.
    sim fast;
    for (int i = 0; i < 1440; ++i) frame(fast, 1.0 / 144.0, step, true);
    CHECK(fast.debt <= step + 1e-12);
    CHECK(std::fabs(fast.phys - fast.real) <= step * 1.01);
    // Paused / bad input: untouched.
    double d = 0.0;
    CHECK(r2_phys_fill_dt(0.0, 0.0, step, &d) == 0.0 && d == 0.0);
    CHECK(r2_phys_fill_dt(0.01, 0.0, 0.0, &d) == 0.01 && d == 0.0);
    CHECK(r2_phys_fill_dt(0.01, -1.0, step, &d) == 0.01 && d == 0.0);
    std::printf("%d checks, %d failed (60 fps jitter: %d -> %d zero-step frames of %d)\n", g_checks, g_fail, off.zero, on.zero, on.frames);
    return g_fail ? 1 : 0;
}
