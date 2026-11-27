"""Offline simulation of R2_DLSS_FG_SCENE_HOLD on an FG_DIAG=3 log (fgdiag.txt): the per-present scene flag replayed through
pfg_streak_gap (R2_DLSS_FG_GAP) + the warm-up + pfg_scene_hold_step, counting Streamline mode switches (on <-> off) per 10 s.
Usage: python tools/fghold_sim.py <fgdiag.txt> [hold=30] [gap=2]"""
import sys
WARM, MINSTREAK = 3, 8

def rows(path):
    out = []
    for l in open(path, encoding="utf-8", errors="replace"):
        p = l.split()
        if len(p) < 15 or not p[0].isdigit():
            continue
        bits = next((x for x in p[10:] if len(x) == 3 and set(x) <= set("01")), None)
        if bits is None:
            continue
        out.append((float(p[1]), int(p[5]), bits[2] == "1", bits[0] == "1"))
    return out

def sim(rs, hold_max, gap_max):
    streak = gap = hold = 0
    res = []
    for t, on_obs, scene, _ in rs:
        held = False
        if not scene:
            if hold:
                hold -= 1; held = True
        if not held:
            if not scene:
                gap = 1 if streak >= MINSTREAK else (gap + 1 if gap and gap < gap_max else 0)
                streak = 0
            else:
                bridge = gap and gap <= gap_max
                gap = 0
                streak = WARM if bridge else streak + 1
        on = (scene or held) and streak >= WARM
        if scene and on:
            hold = hold_max
        res.append((t, on, held))
    return res

def report(name, rs, res):
    t0 = rs[0][0]
    sw = {}
    prev = None
    for (t, on, held) in res:
        w = int((t - t0) // 10000)
        d = sw.setdefault(w, [0, 0, 0, 0])
        d[0] += 1; d[1] += on; d[3] += held
        if prev is not None and on != prev:
            d[2] += 1
        prev = on
    tot = sum(v[2] for v in sw.values())
    print("%-22s switches total %4d | per 10 s window: %s" % (name, tot, " ".join("%d" % sw[w][2] for w in sorted(sw))))
    print("%-22s FG on %% per window: %s" % ("", " ".join("%d" % (100 * sw[w][1] // max(1, sw[w][0])) for w in sorted(sw))))
    return sw

if __name__ == "__main__":
    rs = rows(sys.argv[1])
    hold = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    gap = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    print("%d presents, %.0f s" % (len(rs), (rs[-1][0] - rs[0][0]) / 1000))
    obs = [(t, bool(o), False) for t, o, s, w in rs]
    report("observed (log)", rs, obs)
    report("old logic (sim)", rs, sim(rs, 0, gap))
    sw = report("hold %d (sim)" % hold, rs, sim(rs, hold, gap))
    print("held frames: %d" % sum(v[3] for v in sw.values()))
    # scene-less run lengths
    runs, n = [], 0
    for _, _, s, _ in rs:
        if not s: n += 1
        elif n: runs.append(n); n = 0
    if n: runs.append(n)
    import collections
    c = collections.Counter(min(r, 31) for r in runs)
    print("scene-less run lengths (31 = 31+):", dict(sorted(c.items())))
