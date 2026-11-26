"""R2_LIVE reader / controller (see RSX_MV_DUMP / R2_LIVE in ps3recomp/patches/r2-rpcs3.patch, VKR2Upscaler.cpp).

  python tools\\r2live.py                 per-second object-motion counters of the running Resistance2.exe
  python tools\\r2live.py set l           switch the live A/B variant (needs RSX_MV_DUMP in the game's environment)
  python tools\\r2live.py watch 30 [csv]  30 s of per-second lines (optionally also appended to a csv file)

Read-only apart from the one request byte; it never suspends or stops the game.
"""
import ctypes, ctypes.wintypes as wt, subprocess, sys, time, struct

NAMES = ["late_start", "held", "hold_hist", "hold_full", "nohist", "nh_noslot", "slot_full", "deform_used", "deform_miss",
         "deform_noprev", "om_frames", "skip_cond", "def_frames", "nohist_frame", "ls_age_long", "hist"]
VARS = {"a": "baseline", "b": "FG mv dilated", "c": "FG camera from game VP", "d": "FG mv zeroed on effects",
        "e": "SR jitter sign flipped", "f": "FG camera identity", "g": "FG sky as direction", "h": "SR 8 phases",
        "i": "NV mip bias", "j": "SR preset M", "k": "FG OFF", "l": "FG camera motion only", "m": "SR jitter off"}
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wt.HANDLE
k32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Resistance2.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    pids = [int(l.split('","')[1]) for l in out.splitlines() if l.startswith('"Resistance2.exe"')]
    return pids


def open_blk(pid):
    h = k32.OpenFileMappingW(0x0002 | 0x0004, False, "Local\\R2Live_%d" % pid)  # FILE_MAP_WRITE | FILE_MAP_READ
    if not h:
        return None
    p = k32.MapViewOfFile(h, 0x0002 | 0x0004, 0, 0, 4096)
    return p


def read(p):
    raw = ctypes.string_at(p, 8 * 4 + 32 * 4)
    hdr = struct.unpack_from("<8I", raw, 0)
    ctr = struct.unpack_from("<32I", raw, 32)
    return hdr, ctr


def main():
    pids = find_pid()
    blk = None
    for pid in pids:
        blk = open_blk(pid)
        if blk:
            break
    if not blk:
        sys.exit("no Resistance2.exe with R2_LIVE=1 found (pids %s)" % pids)
    hdr, _ = read(blk)
    if hdr[0] != 0x564C3252:
        sys.exit("bad magic")
    args = sys.argv[1:]
    if args[:1] == ["set"]:
        v = args[1].lower()[:1]
        ctypes.c_uint32.from_address(blk + 7 * 4).value = ord(v)
        time.sleep(0.5)
        hdr, _ = read(blk)
        print("pid %d frame %d variant now %s (%s)" % (hdr[2], hdr[3], chr(hdr[6]) if hdr[6] else "-", VARS.get(chr(hdr[6]) if hdr[6] else "", "RSX_MV_DUMP off")))
        return
    secs = int(args[1]) if args[:1] == ["watch"] and len(args) > 1 else 10
    csv = open(args[2], "a") if args[:1] == ["watch"] and len(args) > 2 else None
    hdr0, c0 = read(blk)
    print("pid %d; columns per second: frames %s" % (hdr0[2], " ".join(NAMES)))
    for _ in range(secs):
        time.sleep(1.0)
        hdr, c = read(blk)
        d = [(b - a) if b >= a else b for a, b in zip(c0, c)]  # a drop = the stats line reset it
        fr = hdr[3] - hdr0[3]
        line = "f%-7d var %s scene %d  fps %3d  " % (hdr[3], chr(hdr[6]) if hdr[6] else "-", hdr[5], fr) + " ".join(
            "%s=%d" % (n, d[i]) for i, n in enumerate(NAMES) if n not in ("nohist_frame",))
        print(line, flush=True)
        if csv:
            csv.write("%f,%d,%s,%d,%s\n" % (time.time(), hdr[3], chr(hdr[6]) if hdr[6] else "-", fr, ",".join(str(x) for x in d[:len(NAMES)])))
            csv.flush()
        hdr0, c0 = hdr, c


if __name__ == "__main__":
    main()
