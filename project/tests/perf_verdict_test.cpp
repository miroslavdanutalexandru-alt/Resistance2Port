// port_perfverdict.h: the [perf] slow-window verdict (R2 #15).
// clang-cl /std:c++20 /EHsc project\tests\perf_verdict_test.cpp
#include "../port_perfverdict.h"
#include <cstdio>
#include <cstring>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static port_pv::Window win(unsigned flips, double main_b, double rsx_b, double spurs_b, double total = 300.0)
{
    port_pv::Window w = {};
    w.secs = 5.0;
    w.flips = flips;
    w.main_busy = main_b;
    w.rsx_busy = rsx_b;
    w.spurs_max = spurs_b;
    w.total_busy = total;
    w.ncpu = 12;
    return w;
}

int main()
{
    port_pv::State st = {};
    char ln[512];
    double fps = 0;
    CHECK(!port_pv::classify(&st, win(300, 99, 10, 10), &fps, ln, sizeof ln) && fps == 60.0, "first window (boot) is never slow, fps %f", fps);
    CHECK(!port_pv::classify(&st, win(300, 60, 40, 30), &fps, ln, sizeof ln), "60 fps sets the best");
    CHECK(st.best_fps == 60.0, "best %f", st.best_fps);
    CHECK(!port_pv::classify(&st, win(250, 60, 40, 30), &fps, ln, sizeof ln) && !ln[0], "50 fps is within 80%% of 60");

    CHECK(port_pv::classify(&st, win(150, 97, 50, 40), &fps, ln, sizeof ln) && std::strstr(ln, "main game thread"), "main: %s", ln);
    CHECK(std::strstr(ln, "30 fps (best 60)") != nullptr, "fps text: %s", ln);
    CHECK(port_pv::classify(&st, win(150, 50, 92, 40), &fps, ln, sizeof ln) && std::strstr(ln, "RSX render thread"), "rsx: %s", ln);
    CHECK(port_pv::classify(&st, win(150, 50, 60, 99), &fps, ln, sizeof ln) && std::strstr(ln, "SPURS lane"), "spurs: %s", ln);
    CHECK(port_pv::classify(&st, win(150, 70, 70, 70, 1150), &fps, ln, sizeof ln) && std::strstr(ln, "all CPU cores"), "cores: %s", ln);
    CHECK(port_pv::classify(&st, win(150, 40, 30, 20), &fps, ln, sizeof ln) && std::strstr(ln, "GPU-bound"), "gpu: %s", ln);
    CHECK(port_pv::classify(&st, win(150, 95, 96, 20), &fps, ln, sizeof ln) && std::strstr(ln, "RSX render thread"), "busiest wins: %s", ln);

    // loads / pauses / movies are not gameplay windows and do not move the best
    CHECK(!port_pv::classify(&st, win(20, 99, 10, 10), &fps, ln, sizeof ln), "4 fps = loading");
    port_pv::Window m = win(150, 99, 10, 10);
    m.movie = true;
    CHECK(!port_pv::classify(&st, m, &fps, ln, sizeof ln), "movie flips");
    CHECK(st.best_fps == 60.0, "best unchanged %f", st.best_fps);

    // the output is capped per run
    port_pv::State capped = {};
    capped.best_fps = 60.0; capped.windows = 5; capped.lines = port_pv::MAX_LINES;
    CHECK(!port_pv::classify(&capped, win(150, 97, 10, 10), &fps, ln, sizeof ln), "line cap");

    // a low best (menus at 15 fps) never reports
    port_pv::State low = {};
    port_pv::classify(&low, win(75, 10, 10, 10), &fps, ln, sizeof ln);
    port_pv::classify(&low, win(75, 10, 10, 10), &fps, ln, sizeof ln);
    CHECK(!port_pv::classify(&low, win(45, 10, 10, 10), &fps, ln, sizeof ln), "best under 20 fps");

    if (!fails) std::printf("perf_verdict: all checks passed\n");
    return fails != 0;
}
