#!/usr/bin/env python3
# stream_e2e.py -- end-to-end test of the TCP video stream.
#
# The monitor is the SERVER: it binds+listens and a viewer connects to it. So for
# each foreign interface this:
#   1. builds+runs the streaming sim (make stream-<sim>) in the background, with
#      NO frame limit so it serves continuously until we stop it,
#   2. waits for the sim to bind+listen, then starts ffmpeg as the viewer: connect
#      to the port, grab exactly ONE frame off the socket, write it as a PPM,
#   3. compares ffmpeg's grabbed frame to the golden, byte for byte,
#   4. stops the sim (it would otherwise serve forever).
#
# Starting the sim first and the viewer second is just *this test* pinning a
# deterministic order; the link itself is order-insensitive (the viewer may
# attach, drop, and reattach whenever). This exercises the real path --
# simulator -> socket -> standard viewer -- rather than a bespoke reader.
#
# Simulators (each maps to one `make` target):
#   dpi   -> Verilator (SystemVerilog / DPI-C)
#   vhpi  -> GHDL gcc/llvm (VHDL / VHPIDIRECT, library linked at elaboration)
#   mcode -> GHDL mcode    (VHDL / VHPIDIRECT, library loaded via _mcode wrapper;
#                           --dist only, since it loads the prebuilt library)
#   nvc   -> NVC       (VHDL / VHPIDIRECT)
#   vpi   -> Icarus    (Verilog / VPI)
# A simulator whose tool is not installed is SKIPPED (so the same run works on
# Linux/macOS/Windows, each with a different subset of FOSS simulators).
#
#   python3 tests/stream_e2e.py                 # build from source, all sims
#   python3 tests/stream_e2e.py --sim dpi,nvc   # a subset
#   python3 tests/stream_e2e.py --dist build/dist
#       # link/load the PREBUILT artifacts in that dir instead of recompiling
#       # the backend -- i.e. test exactly what `make dist` / CI ship.

import argparse
import filecmp
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT   = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "golden" / "gradient_640x480.ppm"
W, H   = 640, 480

# Optional pattern variants: each maps to a `make` target suffix and its own
# golden. "" is the default 8-bit gradient; "c4" drives the gradient through a
# 4-bit color width (COLOR_BITS=4) -- see the Makefile's stream-*-c4 targets.
VARIANTS = {
    "":   ("",     GOLDEN),
    "c4": ("-c4",  ROOT / "golden" / "gradient_640x480_c4.ppm"),
}

# sim -> (executables that must be on PATH, TCP port). Distinct ports so a
# leftover socket from one sim can't be mistaken for the next.
SIMS = {
    "dpi":   (["verilator"],       5030),
    "vhpi":  (["ghdl"],            5031),
    "vpi":   (["iverilog", "vvp"], 5032),
    "nvc":   (["nvc"],             5033),
    "mcode": (["ghdl"],            5034),
}

IS_WINDOWS = os.name == "nt" or "MSYSTEM" in os.environ


def which(name: str):
    """shutil.which, but also finds extensionless wrapper scripts on Windows.
    MSYS2 ships tools like `verilator` as a bare Perl/shell wrapper (the real
    binary is `verilator_bin.exe`); shutil.which misses it on Windows because it
    only matches names carrying a PATHEXT extension."""
    found = shutil.which(name)
    if found or not IS_WINDOWS:
        return found
    for d in os.environ.get("PATH", "").split(os.pathsep):
        cand = Path(d) / name
        if cand.is_file():
            return str(cand)
    return None


def lib_name(kind: str) -> str:
    """Prebuilt shared-library filename for 'dpi' or 'vhpi' on this OS."""
    if platform.system() == "Darwin":
        return f"libvga_monitor_{kind}.dylib"
    if IS_WINDOWS:
        return f"vga_monitor_{kind}.dll"
    return f"libvga_monitor_{kind}.so"


# In --dist mode, the prebuilt file(s) each sim consumes from the dist dir.
def dist_files(sim: str) -> list:
    return {
        "dpi":   [lib_name("dpi")],
        "vhpi":  [lib_name("vhpi")],
        "nvc":   [lib_name("vhpi")],
        "vpi":   ["vga_monitor.vpi"],
        # mcode loads the VHPI library named in the generated wrapper.
        "mcode": [lib_name("vhpi"), "vga_monitor_pkg_mcode.vhdl"],
    }[sim]


def ghdl_backend():
    """Which GHDL code generator is installed: 'mcode', 'llvm', 'gcc', or None.
    vhpi (link at elaboration) needs llvm/gcc; mcode (load at run time) needs
    mcode -- the two are mutually exclusive in one `ghdl` install."""
    g = which("ghdl")
    if not g:
        return None
    out = subprocess.run([g, "--version"], capture_output=True, text=True).stdout.lower()
    for be in ("mcode", "llvm", "gcc"):
        if be in out:
            return be
    return "unknown"


def port_listening(port: int) -> bool:
    # Prefer ss (Linux); fall back to netstat (macOS/Windows). Listening
    # endpoints print the port after ':' (ss/Windows) or '.' (BSD/macOS).
    for cmd, needs_listen in (["ss", "-ltn"], False), (["netstat", "-an"], True):
        if not which(cmd[0]):
            continue
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        for line in out.splitlines():
            if needs_listen and "LISTEN" not in line.upper():
                continue
            for tok in line.replace("\t", " ").split():
                if tok.endswith(f":{port}") or tok.endswith(f".{port}"):
                    return True
        return False  # the tool ran but the port is not listening (yet)
    return False


def loader_env(env: dict, dist: str) -> None:
    """Let the prebuilt lib be found at runtime, per OS, in-place."""
    d = str(Path(dist).resolve())
    if platform.system() == "Darwin":
        var = "DYLD_LIBRARY_PATH"      # belt-and-suspenders; targets also set -rpath
    elif IS_WINDOWS:
        var = "PATH"
    else:
        var = "LD_LIBRARY_PATH"
    env[var] = d + os.pathsep + env.get(var, "")


def popen_group(cmd, env, log):
    """Start the sim in its own process group/session so we can kill the whole
    tree (make -> shell -> simulator) afterwards. Output goes to a file, not a
    pipe, so a chatty sim can never fill a pipe buffer and stall."""
    if os.name == "nt":
        return subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log,
                                stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    return subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log,
                            stderr=subprocess.STDOUT, start_new_session=True)


def kill_group(p):
    """Tear down the sim's whole process tree (it serves forever with no frame
    limit, so it won't exit on its own). On Windows the simulator is a grandchild
    behind MSYS2's make/sh, and taskkill /T can't always walk that tree, so we
    also kill whatever still holds the port (the actual server) -- see kill_port."""
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)],
                           capture_output=True)
        else:
            os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except Exception:
        p.kill()
    try:
        p.wait(timeout=10)
    except Exception:
        p.kill()


def kill_port(port):
    """Kill any process still LISTENING on `port` (Windows). A reliable backstop
    for the sim when the make/sh process tree can't be walked from the parent PID;
    a no-op on POSIX, where killpg already reaped the whole session."""
    if os.name != "nt":
        return
    out = subprocess.run(["netstat", "-ano"], capture_output=True, text=True).stdout
    pids = set()
    for line in out.splitlines():
        if "LISTENING" not in line.upper():
            continue
        toks = line.split()
        if len(toks) >= 2 and toks[1].endswith(f":{port}") and toks[-1].isdigit():
            pids.add(toks[-1])
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", pid], capture_output=True)


def run_one(sim, dist, variant="", fmt="raw"):
    port = SIMS[sim][1]
    suffix, golden = VARIANTS[variant]
    target = f"stream-{sim}-dist" if dist else f"stream-{sim}{suffix}"
    tag = f"{sim}{suffix}{'-ppm' if fmt == 'ppm' else ''}"
    tmp = Path("/tmp") if not IS_WINDOWS else Path(os.environ.get("TEMP", "."))
    grab = tmp / f"stream_e2e_{tag}.ppm"
    simlog = tmp / f"stream_e2e_{tag}.log"
    if grab.exists():
        grab.unlink()

    def fail(why, log):
        print(f"  {sim}: FAIL ({why})")
        if log.exists():
            print(log.read_text(errors="replace")[-6000:])
        return False

    # Guard against a stale sim left listening on this port by a botched earlier
    # run: we'd otherwise connect to it (wrong format/pattern) and mis-report.
    if port_listening(port):
        print(f"  {sim}: FAIL (port {port} already in use -- stale sim from a previous run?)")
        return False

    # 1. build + run the streaming SERVER in the background. No VGA_MONITOR_FRAMES,
    #    so it serves continuously until kill_group below. The wire format goes in
    #    as the FORMAT make var (the recipe turns it into VGA_MONITOR_FORMAT for the
    #    sim); clear any inherited env copy so it can't shadow that.
    env = dict(os.environ)
    env.pop("VGA_MONITOR_FRAMES", None)
    env.pop("VGA_MONITOR_FORMAT", None)
    cmd = ["make", target, f"STREAM=127.0.0.1:{port}", f"FORMAT={fmt}"]
    if dist:
        loader_env(env, dist)
        cmd.append(f"DIST={dist}")

    with open(simlog, "w") as log:
        sim_proc = popen_group(cmd, env, log)
        try:
            # 2. wait for the sim to bind+listen (this also covers the build time).
            t0 = time.time()
            while not port_listening(port):
                if sim_proc.poll() is not None:
                    return fail("sim exited before it listened (build/run error?)", simlog)
                if time.time() - t0 > 240:       # generous: includes a verilator build
                    return fail("sim never started listening", simlog)
                time.sleep(0.1)

            # 3. ffmpeg: the standard viewer, connecting to the sim and grabbing one
            #    frame off the socket. In ppm mode the stream is self-describing
            #    (per-frame P6 headers carry the geometry), so image2pipe/ppm reads
            #    it with NO -video_size; raw rgb24 must be told the size out of band.
            if fmt == "ppm":
                ff_input = ["-f", "image2pipe", "-vcodec", "ppm"]
            else:
                ff_input = ["-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", f"{W}x{H}"]
            try:
                subprocess.run(
                    ["ffmpeg", "-hide_banner", "-loglevel", "error", *ff_input,
                     "-i", f"tcp://127.0.0.1:{port}", "-frames:v", "1", "-y", str(grab)],
                    timeout=120, capture_output=True,
                )
            except subprocess.TimeoutExpired:
                return fail("ffmpeg did not capture a frame", simlog)
        finally:
            kill_group(sim_proc)
            kill_port(port)

    if not grab.exists() or grab.stat().st_size == 0:
        return fail("no frame grabbed", simlog)

    ok = filecmp.cmp(grab, golden, shallow=False)
    sz = grab.stat().st_size
    label = f"{sim} (ppm)" if fmt == "ppm" else sim
    print(f"  {label}: {'PASS' if ok else 'FAIL'} "
          f"(ffmpeg grab {sz} B {'==' if ok else '!='} golden {golden.stat().st_size} B)")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default="all",
                    help="comma-separated subset of: " + ", ".join(SIMS) + " (default all)")
    ap.add_argument("--dist", default=None, metavar="DIR",
                    help="test the prebuilt artifacts in DIR instead of building from source")
    ap.add_argument("--variant", default="", choices=sorted(VARIANTS),
                    help="pattern variant: '' (8-bit gradient) or 'c4' (4-bit color width)")
    ap.add_argument("--format", default="raw", choices=("raw", "ppm"),
                    help="stream wire format: 'raw' (rgb24, ffmpeg -video_size) or "
                         "'ppm' (self-describing P6, ffmpeg image2pipe)")
    args = ap.parse_args()

    if args.variant and args.dist:
        print("error: --variant cannot be combined with --dist")
        return 2

    if not which("ffmpeg"):
        print("error: 'ffmpeg' not found (needed for the end-to-end test)")
        return 2
    if not which("ss") and not which("netstat"):
        print("error: neither 'ss' nor 'netstat' found (needed to detect the viewer)")
        return 2

    requested = list(SIMS) if args.sim == "all" else args.sim.split(",")
    for s in requested:
        if s not in SIMS:
            print(f"error: unknown sim '{s}' (choose from {', '.join(SIMS)})")
            return 2

    # Select what we can actually run here; report (don't silently drop) the rest.
    selected, skipped = [], []
    for s in requested:
        tools = SIMS[s][0]
        missing = [t for t in tools if not which(t)]
        absent = [f for f in dist_files(s) if not (Path(args.dist) / f).exists()] \
            if args.dist else []
        # vhpi and mcode share the `ghdl` binary but need OPPOSITE backends:
        # vhpi links the library at elaboration (gcc/llvm), mcode loads it (mcode).
        be = ghdl_backend() if s in ("vhpi", "mcode") else None
        if missing:
            skipped.append((s, "missing " + ", ".join(missing)))
        elif s == "mcode" and not args.dist:
            skipped.append((s, "mcode is dist-only (loads the prebuilt library; use --dist)"))
        elif s == "mcode" and be != "mcode":
            skipped.append((s, f"ghdl backend is {be}, not mcode"))
        elif s == "vhpi" and be == "mcode":
            skipped.append((s, "ghdl backend is mcode, not gcc/llvm (use --sim mcode)"))
        elif absent:
            skipped.append((s, f"no {', '.join(absent)} in {args.dist}"))
        else:
            selected.append(s)

    mode = f"prebuilt artifacts in {args.dist}" if args.dist else "built from source"
    wire = "ppm (P6, self-describing)" if args.format == "ppm" else "raw rgb24"
    print(f"end-to-end {wire} stream test ({mode})")
    for s, why in skipped:
        print(f"  {s}: SKIP ({why})")
    if not selected:
        print("RESULT: nothing to test (no matching simulator available)")
        return 2

    results = {s: run_one(s, args.dist, args.variant, args.format) for s in selected}

    ok = all(results.values())
    print("RESULT:", "all passed" if ok else "FAILURES: " +
          ", ".join(s for s, r in results.items() if not r))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
