// Unit test for the R2 walk-start step (project/r2_movestart.h, GitHub R2 #21; no game).
// clang-cl /nologo /O1 /EHsc /std:c++17 /I project project\tests\move_start_test.cpp /Fe:scratch\move_start_test.exe /Fo:scratch\move_start_test.obj
#include "r2_movestart.h"
#include <cstdio>
#include <initializer_list>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// The game's first step from a standstill: rate * dt_scaled, zeroed below 0.3 * scale.
static bool starts(double fps, double scale, double rate, bool fix)
{
    const double dt_scaled = scale / fps;
    const double k = fix ? r2_move_start_boost(dt_scaled, scale) : 1.0;
    return rate * dt_scaled * k >= 0.3 * scale;
}

int main()
{
    const double rate = 25.0;   // starts at 75 fps, not at 100 (the reported limit)
    const double fps[] = {20, 30, 60, 75, 100, 120, 144, 165, 240, 360, 1000};
    for (double scale : {1.0, 0.5, 2.0}) {
        for (double f : fps) {
            CHECK(starts(f, scale, rate, true));
            if (f <= 60.0) CHECK(r2_move_start_boost(scale / f, scale) == 1.0);
        }
        CHECK(!starts(100.0, scale, rate, false));
        CHECK(!starts(240.0, scale, rate, false));
    }
    CHECK(r2_move_start_boost(1.0 / 144.0, 1.0) > 2.39 && r2_move_start_boost(1.0 / 144.0, 1.0) < 2.41);
    CHECK(r2_move_start_boost(1e-6, 1.0) == 64.0);
    CHECK(r2_move_start_boost(0.0, 1.0) == 1.0);    // paused / first frame
    CHECK(r2_move_start_boost(0.01, 0.0) == 1.0);   // time scale 0
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
