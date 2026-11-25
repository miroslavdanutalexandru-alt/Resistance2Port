/* Resistance 2: walk-start step above 60 fps (R2_MOVE_START_FIX, GitHub R2 #21).
 *
 * Hero ground update func_001270C8: the velocity (+0xF0) is reset every frame to the
 * measured one (+0x110), then func_000F0E30 steers it toward the stick's target by at
 * most rate * dt (dt = the game timer 0x012F73A8: +0 scale, +4 dt * scale), and the
 * test at 0x001279B0 zeroes it when |v| < 0.3 * scale if the measured velocity was
 * ~zero (largest component < 0.0002), else < 0.2 * scale. That threshold is not
 * scaled by dt: from a standstill, the first step (rate * dt) falls below 0.3 m/s once
 * frames get short enough (~75-100 fps), the velocity is zeroed every frame and the
 * hero cannot start walking (a jump's landing carries speed past the test).
 *
 * r2_move_start_boost(): the factor applied to the steering rates while standing
 * still, so the first step is as large as at 60 fps; 1.0 at 60 fps and below (the
 * game's own behaviour), up to 64 for very short frames. */
#ifndef R2_MOVESTART_H
#define R2_MOVESTART_H

#define R2_MOVE_START_REF_S (1.0 / 60.0)
#define R2_MOVE_STILL_EPS 0.0002   /* the game's "was standing still" test (pool -0x7FDC) */

static inline double r2_move_start_boost(double dt_scaled, double scale)
{
    if (!(dt_scaled > 0.0) || !(scale > 0.0)) return 1.0;
    const double real = dt_scaled / scale;
    if (!(real < R2_MOVE_START_REF_S)) return 1.0;
    const double k = R2_MOVE_START_REF_S / real;
    return k > 64.0 ? 64.0 : k;
}

#endif
