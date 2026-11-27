/* Resistance 2: no physics frames without a step (R2_PHYS_STEP_FILL, stuck dead enemies, GitHub R2 #27).
 *
 * The physics frame func_006F3C64 (called from func_002C28AC with the game dt, clock
 * 0x012F7308 + 0xA4) feeds each physics world (0x0134E100 / 0x0134E104 / 0x0134E108)
 * an accumulator (+0x54 += dt) and a fixed step of 1/60 s * time scale; the SPU job
 * runs floor(acc / step) steps and the remainder is kept. On PS3 (30 fps) every frame
 * had two steps. Above ~60 fps, or at 60 fps with jitter, frames come with zero steps.
 *
 * r2_phys_fill_dt(): when the frame would run zero steps, the physics dt is topped up
 * to just reach one step; the borrowed time (never more than one step in total) is
 * paid back from later frames that step anyway, without making them step-less. */
#ifndef R2_PHYSSTEP_H
#define R2_PHYSSTEP_H

#define R2_PHYS_STEP_S (1.0 / 60.0)
#define R2_PHYS_STEP_MARGIN 1.0e-4   /* fraction of a step added so float rounding still yields one */

static inline double r2_phys_fill_dt(double dt, double acc, double step, double* debt)
{
    if (!(dt > 0.0) || !(step > 0.0) || !(acc >= 0.0) || !(acc < 1.0)) return dt;
    const double need = (step - acc) + step * R2_PHYS_STEP_MARGIN;
    if (acc + dt >= step) {
        if (!(*debt > 0.0)) return dt;
        double eff = dt - *debt;
        if (eff < need) eff = need;
        if (eff > dt) eff = dt;
        *debt -= dt - eff;
        if (*debt < 0.0) *debt = 0.0;
        return eff;
    }
    const double borrow = need - dt;
    if (*debt + borrow > step) return dt;
    *debt += borrow;
    return need;
}

#endif
