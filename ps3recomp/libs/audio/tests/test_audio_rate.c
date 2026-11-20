/* audio_rate.h: the [audio] "rate" check (produced vs consumed guest audio).
 * clang-cl /nologo /O1 -MD libs\audio\tests\test_audio_rate.c /Fetest_audio_rate.exe */
#include "../audio_rate.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* 5 s at realtime, nothing concealed: 937.5 blocks */
    CHECK(fabs(audio_rate_pct(938, 0, 5000.0) - 100.05) < 0.01);
    CHECK(!audio_rate_slow(audio_rate_pct(938, 0, 5000.0)));
    /* the old dev-machine gameplay log: ~350 of 938 blocks concealed */
    double p = audio_rate_pct(938, 350, 5000.0);
    CHECK(fabs(p - 62.72) < 0.01);
    CHECK(audio_rate_slow(p));
    /* the late-block wait slipped the clock: 85 % as many reads, all written */
    p = audio_rate_pct(797, 0, 5000.0);
    CHECK(p > 84.9 && p < 85.1);
    CHECK(audio_rate_slow(p));
    /* within the 3 % catch-up: not slow */
    CHECK(!audio_rate_slow(audio_rate_pct(915, 0, 5000.0)));
    CHECK(audio_rate_slow(audio_rate_pct(905, 0, 5000.0)));
    /* paused windows: the paused ticks were taken out of active_ms */
    CHECK(audio_rate_pct(0, 0, 0.0) < 0.0 && !audio_rate_slow(audio_rate_pct(0, 0, 0.0)));
    CHECK(audio_rate_pct(150, 0, 800.0) < 0.0);
    CHECK(fabs(audio_rate_pct(375, 0, 2000.0) - 100.0) < 1e-9);
    /* more concealed than read (idle port counted earlier): 0, not wrap-around */
    CHECK(audio_rate_pct(10, 20, 5000.0) == 0.0);
    if (!fails) puts("audio rate: 12 checks passed");
    return fails != 0;
}
