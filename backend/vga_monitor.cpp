// vga_monitor.cpp
//
// Simulator-agnostic backend for the auto-detecting VGA monitor. It is reached
// through three thin foreign-interface shims that all call the same functions:
//   - SystemVerilog + Verilator -> DPI-C       (vga_monitor.sv)
//   - VHDL + GHDL               -> VHPIDIRECT  (vga_monitor_vhpi.cpp)
//   - Verilog + Icarus          -> VPI         (vga_monitor_vpi.cpp)
//
// There is a single monitor variant: it takes NO clock and NO parameters.
// Driven by the signal transitions (r, g, b, hsync, vsync) tagged with the
// current simulation time, it recovers everything -- sync polarity, the pixel
// period, and the active resolution/offset -- reconstructs the framebuffer in
// memory, and serves each finished frame as raw rgb24 over TCP to a standard
// viewer (e.g. ffplay). No windowing toolkit is linked into the simulator.
//
// The monitor is the SERVER: it binds + listens, and a viewer connects to it.
// This makes the link insensitive to launch order -- the simulation and the
// viewer can start in either order, and a viewer can connect, disconnect, and
// reconnect on the fly. While no viewer is attached the simulation runs
// unaffected (frames are simply not sent); accept() is non-blocking and writes
// are time-bounded, so the viewer's lifecycle can never stall the simulation.
//
// Each instance maps to one Monitor (returned as an opaque handle), so multiple
// monitors can run at once, each with its own framebuffer and listening port.
//
// Environment variables:
//   VGA_MONITOR_FRAMES=<N>    exit(0) after N complete frames (per process)
//   VGA_MONITOR_STREAM=host:port
//                             bind + listen here; when a viewer connects, write
//                             each finished frame as raw rgb24 (W*H*3 bytes). Use
//                             127.0.0.1 for local viewers, 0.0.0.0 for remote. A
//                             standard viewer connects to it, e.g.:
//                               ffplay -f rawvideo -pixel_format rgb24 \
//                                      -video_size 640x480 -i 'tcp://127.0.0.1:5000'
//                             With multiple monitors, instance i (by open order)
//                             listens on port+i, so each gets its own viewer.
//   VGA_MONITOR_FORMAT=raw|ppm  stream wire format (default raw; ppm recommended
//                             for a live viewer). In ppm mode each frame is a
//                             self-describing P6 PPM (a ~15-byte ASCII
//                             "P6\n<W> <H>\n255\n" header carrying the geometry,
//                             then the same W*H*3 rgb24 bytes), so a viewer needs
//                             no out-of-band -video_size AND can resync on the P6
//                             magic when it joins or rejoins mid-stream:
//                               ffplay -f image2pipe -vcodec ppm -i tcp://127.0.0.1:5000
//                             The format choice is global across instances.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

// --- Cross-platform TCP socket layer (POSIX + Windows/Winsock) --------------
// The monitor is the SERVER: it binds + listens, and a viewer connects to it
// whenever it likes. So beyond send/close we need a non-blocking accept (to poll
// for a (re)connecting viewer without ever blocking the simulation), a send
// timeout (so a paused/wedged viewer drops frames instead of freezing the sim),
// and a "transient, try again later" predicate over the platform's errno set.
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  static const sock_t kBadSock = INVALID_SOCKET;
  static int  sock_close(sock_t s) { return ::closesocket(s); }
  static int  sock_send(sock_t s, const char* b, size_t n) { return ::send(s, b, (int)n, 0); }
  static void sock_set_nonblock(sock_t s) { u_long m = 1; ::ioctlsocket(s, FIONBIO, &m); }
  static void sock_set_send_timeout(sock_t s, int ms) {
      DWORD t = static_cast<DWORD>(ms);
      ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  }
  // Deliberately NOT SO_REUSEADDR on Windows: there it lets a second process bind
  // the SAME live port and silently steal connections (unlike POSIX, where it only
  // reclaims a TIME_WAIT port). The default already rejects a double bind, which is
  // what we want -- a stale server cleanly loses the port to "address in use".
  static void sock_reuse_addr(sock_t) {}
  // EWOULDBLOCK (non-blocking accept, no pending viewer) or ETIMEDOUT (a blocking
  // send hit SO_SNDTIMEO): both mean "no progress now", not "viewer gone".
  static bool sock_transient() { int e = ::WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT; }
  static void sock_startup() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
  #define VGA_EXPORT extern "C" __declspec(dllexport)
#else
  #include <csignal>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netdb.h>
  #include <unistd.h>
  using sock_t = int;
  static const sock_t kBadSock = -1;
  static int     sock_close(sock_t s) { return ::close(s); }
  static ssize_t sock_send(sock_t s, const char* b, size_t n) {
  #ifdef MSG_NOSIGNAL
      return ::send(s, b, n, MSG_NOSIGNAL);   // Linux: don't raise SIGPIPE on a dead peer
  #else
      return ::send(s, b, n, 0);              // macOS: covered by the SIGPIPE ignore below
  #endif
  }
  static void sock_set_nonblock(sock_t s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
  static void sock_set_send_timeout(sock_t s, int ms) {
      struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
      ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  // Allow an immediate rebind of a port still in TIME_WAIT from our own prior run.
  // On POSIX this does NOT permit a second live bind, so it can't steal a port.
  static void sock_reuse_addr(sock_t s) { int one = 1; ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); }
  static bool sock_transient() { return errno == EAGAIN || errno == EWOULDBLOCK; }
  // A viewer that vanishes mid-write would otherwise SIGPIPE the simulator dead;
  // ignore it process-wide (belt to MSG_NOSIGNAL's braces) so writes just error.
  static void sock_startup() { ::signal(SIGPIPE, SIG_IGN); }
  #define VGA_EXPORT extern "C"
#endif

namespace {

struct Pixel {
    uint8_t r, g, b;
};

// Standard VGA/VESA timing modes, keyed by the two quantities recoverable from
// HSYNC + VSYNC alone: the number of lines per frame (vertical total) and the
// hsync-pulse / line-period ratio. Matching one yields the full timing, so
// everything below -- the dot clock and the active window -- is sync-derived,
// with no dependence on RGB content.
struct VgaMode {
    int         v_total;     // lines per frame (incl. blanking) -- the match key
    int         h_total;     // pixel clocks per line (incl. blanking)
    int         h_active;    // visible pixels per line
    int         v_active;    // visible lines
    // Blanking before the first active sample, i.e. sync pulse + back porch. The
    // active window opens this far past each sync's leading edge; the exact
    // front/back porch split is irrelevant, only where blanking ends matters.
    int         h_lead2act;  // pixels from the hsync lead to the first active pixel
    int         v_lead2act;  // lines  from the vsync lead to the first active line
    double      hs_ratio;    // hsync pulse width / line period (sanity cross-check)
    const char* name;
};

const VgaMode kModes[] = {
    //  v_tot  h_tot  h_act  v_act  h_l2a  v_l2a  hs_ratio          name
    {   525,   800,   640,   480,   144,   35,    96.0 / 800.0,   "640x480@60"},
    {   628,  1056,   800,   600,   216,   27,    128.0 / 1056.0, "800x600@60"},
    {   806,  1344,  1024,   768,   296,   35,    136.0 / 1344.0, "1024x768@60"},
    {   994,  1712,  1280,   960,   352,   33,    136.0 / 1712.0, "1280x960@60"},
};

// Match a measured (lines/frame, hsync ratio) to a standard mode. The line count
// is exact (a transition count), so it keys the lookup; the ratio guards against
// a coincidental count match.
const VgaMode* match_mode(int v_total, double hs_ratio) {
    for (const VgaMode& m : kModes) {
        if (m.v_total == v_total && std::fabs(hs_ratio - m.hs_ratio) < 0.03)
            return &m;
    }
    return nullptr;
}

struct Monitor {
    std::string name;

    // Active geometry, recovered during lock (not given as parameters).
    int h_active = 0, v_active = 0;
    int hsync_pol = 0, vsync_pol = 0;  // level on the wire during the sync pulse

    // Reconstruction state
    std::vector<Pixel> framebuffer;    // current frame being drawn
    bool prev_in_hsync = false;
    bool prev_in_vsync = false;
    uint64_t frames_done = 0;

    // --- Sampling / line-count state ---
    // Time each color run against the line's hsync lead and count hsync pulses
    // for the line number -- exactly what a real monitor's sync separator + PLL
    // do. Once locked, pixels are point-sampled inside each run (see below).
    bool   has_prev_event = false;
    double e_prev_time = 0.0;           // timestamp of the previous event
    Pixel  e_prev_color = {0, 0, 0};    // color held since the previous event
    double t_line_lead = 0.0;           // time of the current line's hsync lead edge
    bool   have_line_lead = false;
    double t_pix = 0.0;                 // recovered pixel period (0 => not locked)
    int    line_count = 0;              // hsync lead edges since the vsync lead edge

    // --- Auto-detect (no clock, no parameters) state ---
    // Everything is recovered from the sync signals alone, like a real monitor's
    // PLL, in three steps:
    //   A. lock sync polarity (shorter-held level = pulse);
    //   B. lock the line period from HSYNC and match a standard VGA mode;
    //   C. the matched mode gives the dot clock (t_pix = line_period / h_total)
    //      and the active window (it opens h_lead2act pixels past the hsync lead
    //      and v_lead2act lines past the vsync lead). Pixels are then point-
    //      sampled a quarter period in, clear of the edge transitions. RGB never
    //      influences the timing, so any image -- flat, coarse, or underscanned
    //      -- locks identically.

    // Phase A -- sync polarity by time held at each level. Locked once both
    // signals show >=2 runs of each level. hs_maxdur[hsync_pol] is also the hsync
    // pulse width (a constant run), used as a sanity cross-check on the mode.
    bool   pol_locked = false;
    int    hs_lvl = -1; double hs_t0 = 0.0; double hs_maxdur[2] = {0, 0}; int hs_cnt[2] = {0, 0};
    int    vs_lvl = -1; double vs_t0 = 0.0; double vs_maxdur[2] = {0, 0}; int vs_cnt[2] = {0, 0};

    // Phase B -- line period from HSYNC and lines/frame, measured over whole
    // frames; locked when two consecutive frames agree on the line count.
    bool   locked = false;              // t_pix and geometry fully recovered
    bool   have_last_hlead = false;
    double last_hlead_t = 0.0;          // time of the previous hsync leading edge
    double t_line = 0.0;                // measured line period (between hsync leads)
    int    hlead_warm = 0;             // skip the first leads (startup settling)
    int    lines_this_frame = 0;        // hsync leads since the last vsync lead
    int    prev_v_total = -2;           // last frame's line count, to confirm stability

    // Phase C -- locked-in recovered parameters (all from sync + the matched mode).
    double act_offset = 0.0;            // active-window start, relative to hsync lead
    int    det_yoff = 0;                // line number of the first active line

    // --- TCP video stream (VGA_MONITOR_STREAM) ---
    // The monitor is the server: it listens on listen_fd and serves whatever
    // viewer is connected (stream_fd), one at a time. Both are independent of the
    // viewer's lifecycle -- a viewer may attach, drop, and reattach at will; the
    // simulation runs unaffected when none is connected.
    int    index = 0;             // open order; instance i listens on port+i
    sock_t listen_fd = kBadSock;  // bound + listening (idle until a viewer arrives)
    sock_t stream_fd = kBadSock;  // the connected viewer, or kBadSock if none
};

long g_frame_limit = -1;          // -1 => unlimited
bool g_inited = false;
int  g_open_count = 0;            // monitors opened so far (for per-instance port)
std::string g_stream_target;      // bind "host:port" or empty (no streaming)
bool g_stream_ppm = false;        // VGA_MONITOR_FORMAT=ppm -> per-frame P6 headers

void lazy_init_globals() {
    if (g_inited) return;
    g_inited = true;
    sock_startup();               // no-op on POSIX; WSAStartup on Windows
    if (const char* f = std::getenv("VGA_MONITOR_FRAMES")) {
        g_frame_limit = std::atol(f);
    }
    if (const char* s = std::getenv("VGA_MONITOR_STREAM")) {
        if (s[0]) g_stream_target = s;
    }
    if (const char* f = std::getenv("VGA_MONITOR_FORMAT")) {
        g_stream_ppm = (std::string(f) == "ppm");
    }
}

// Bind + listen on the address named by VGA_MONITOR_STREAM. Done once, up front
// (geometry need not be known: the listen socket carries no frame size), so a
// viewer may already be waiting -- or may connect, drop, and reconnect at any
// later point -- without the simulation ever caring about the order.
void stream_listen(Monitor* m) {
    const std::string& tgt = g_stream_target;
    size_t colon = tgt.find_last_of(':');
    std::string host = (colon == std::string::npos) ? "127.0.0.1" : tgt.substr(0, colon);
    std::string port = (colon == std::string::npos) ? tgt : tgt.substr(colon + 1);
    if (host.empty()) host = "127.0.0.1";
    if (m->index != 0)            // multiple monitors -> consecutive ports
        port = std::to_string(std::atol(port.c_str()) + m->index);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;  // bind, not connect
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "[vga_monitor] '%s' stream: cannot resolve %s:%s\n",
                     m->name.c_str(), host.c_str(), port.c_str());
        return;
    }
    sock_t fd = kBadSock;
    for (auto p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kBadSock) continue;
        sock_reuse_addr(fd);    // reclaim our own TIME_WAIT port (POSIX); never steal a live one
        if (bind(fd, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0 && listen(fd, 1) == 0) break;
        sock_close(fd); fd = kBadSock;
    }
    freeaddrinfo(res);
    if (fd == kBadSock) {
        std::fprintf(stderr, "[vga_monitor] '%s' stream: cannot listen on %s:%s\n",
                     m->name.c_str(), host.c_str(), port.c_str());
        return;
    }
    sock_set_nonblock(fd);      // accept must never block the simulation
    m->listen_fd = fd;
    std::fprintf(stderr,
        "[vga_monitor] '%s' serving %s on %s:%s (connect a viewer any time)\n",
        m->name.c_str(), g_stream_ppm ? "ppm (P6)" : "raw rgb24", host.c_str(), port.c_str());
}

// Poll (non-blocking) for a viewer connecting/reconnecting. Called between frames
// while no viewer is attached, so a newly accepted viewer always starts at a
// frame boundary. Returns immediately if none is pending.
void stream_accept(Monitor* m) {
    if (m->listen_fd == kBadSock || m->stream_fd != kBadSock) return;
    sock_t fd = accept(m->listen_fd, nullptr, nullptr);
    if (fd == kBadSock) {
        if (!sock_transient())  // a real accept error is worth a line; EWOULDBLOCK isn't
            std::fprintf(stderr, "[vga_monitor] '%s' stream: accept failed\n", m->name.c_str());
        return;
    }
    // Bound write time so a paused/slow viewer drops frames rather than stalling
    // the simulation; on timeout the half-written frame is abandoned and a PPM
    // viewer resyncs on the next frame's P6 magic.
    sock_set_send_timeout(fd, 1000);
    m->stream_fd = fd;
    std::fprintf(stderr, "[vga_monitor] '%s' viewer connected (%dx%d)\n",
                 m->name.c_str(), m->h_active, m->v_active);
}

// Send exactly n bytes, retrying short writes. Distinguishes two failures: a
// transient one (SO_SNDTIMEO fired -- the viewer can't keep up) drops the rest of
// this frame but keeps the link; a hard error (viewer gone) closes the socket so
// the next frame goes back to accepting. Either way it returns false.
bool stream_write_all(Monitor* m, const char* p, size_t n) {
    while (n) {
        auto k = sock_send(m->stream_fd, p, n);
        if (k > 0) { p += k; n -= static_cast<size_t>(k); continue; }
#ifndef _WIN32
        if (k < 0 && errno == EINTR) continue;
#endif
        if (sock_transient()) return false;   // viewer too slow: drop frame, keep link
        std::fprintf(stderr, "[vga_monitor] '%s' stream: viewer gone, will re-accept\n",
                     m->name.c_str());
        sock_close(m->stream_fd); m->stream_fd = kBadSock;
        return false;
    }
    return true;
}

// Write one finished frame. The Pixel vector is already tightly packed r,g,b --
// exactly the rawvideo layout -- so raw mode sends it verbatim (W*H*3 bytes, no
// header). In PPM mode (VGA_MONITOR_FORMAT=ppm) each frame is prefixed with its
// own P6 header ("P6\n<W> <H>\n255\n"), making the stream self-describing: the
// geometry rides in-band, so image2pipe/ppm viewers need no -video_size.
void stream_send_frame(Monitor* m) {
    if (m->stream_fd == kBadSock) return;
    if (g_stream_ppm) {
        char hdr[64];
        int len = std::snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n",
                                m->h_active, m->v_active);
        if (!stream_write_all(m, hdr, static_cast<size_t>(len))) return;
    }
    const char* p = reinterpret_cast<const char*>(m->framebuffer.data());
    size_t n = m->framebuffer.size() * sizeof(Pixel);
    stream_write_all(m, p, n);
}

// Called once per complete frame (on the leading edge of the vsync pulse, after
// the active region has been fully drawn).
void on_frame_complete(Monitor* m) {
    m->frames_done++;

    if (!g_stream_target.empty()) {
        if (m->stream_fd == kBadSock) stream_accept(m);  // pick up a (re)connecting viewer
        stream_send_frame(m);                            // no-op while none is connected
    }

    if (g_frame_limit >= 0 && static_cast<long>(m->frames_done) >= g_frame_limit) {
        std::exit(0);
    }
}

}  // namespace


// Auto-detect path: NO clock and NO parameters. Driven by signal *transitions*
// tagged with the current simulation time, it recovers EVERYTHING, the way a
// real monitor locks:
//   - sync polarity   : the shorter-held level of each sync is the pulse;
//   - the line period : from HSYNC -- the time between hsync leading edges;
//   - the resolution  : the lines/frame and hsync ratio are matched to a standard
//                       VGA mode, which gives the EXPECTED active-pixel count;
//   - the pixel clock : t_pix = (measured active video duration) / (expected
//                       active pixels). Calibrating to the real active window,
//                       not an assumed porch split, makes the dot clock exact;
//   - rendering       : each pixel is point-sampled a quarter period into its
//                       cell, so the sample lands in the data, not on a
//                       transition edge.
VGA_EXPORT void* vga_monitor_open(const char* name) {
    lazy_init_globals();
    Monitor* m = new Monitor();
    m->name = name ? name : "VGA Monitor (auto)";
    m->index = g_open_count++;
    std::fprintf(stderr, "[vga_monitor] opened '%s' auto-detect\n", m->name.c_str());
    if (!g_stream_target.empty()) stream_listen(m);  // listen now; a viewer may attach any time
    return m;
}

VGA_EXPORT void vga_monitor_event(void* handle, double t,
                                       uint8_t r, uint8_t g, uint8_t b,
                                       int hsync, int vsync) {
    Monitor* m = static_cast<Monitor*>(handle);
    if (!m) return;

    const int hs = (hsync != 0) ? 1 : 0;
    const int vs = (vsync != 0) ? 1 : 0;

    // --- Phase A: recover sync polarity by the time held at each level. ---
    if (!m->pol_locked) {
        if (m->hs_lvl < 0) {                       // first event
            m->hs_lvl = hs; m->hs_t0 = t;
            m->vs_lvl = vs; m->vs_t0 = t;
        }
        if (hs != m->hs_lvl) {
            const double d = t - m->hs_t0;
            if (d > m->hs_maxdur[m->hs_lvl]) m->hs_maxdur[m->hs_lvl] = d;
            m->hs_cnt[m->hs_lvl]++; m->hs_lvl = hs; m->hs_t0 = t;
        }
        if (vs != m->vs_lvl) {
            const double d = t - m->vs_t0;
            if (d > m->vs_maxdur[m->vs_lvl]) m->vs_maxdur[m->vs_lvl] = d;
            m->vs_cnt[m->vs_lvl]++; m->vs_lvl = vs; m->vs_t0 = t;
        }

        if (m->hs_cnt[0] >= 2 && m->hs_cnt[1] >= 2 &&
            m->vs_cnt[0] >= 2 && m->vs_cnt[1] >= 2) {
            m->hsync_pol = (m->hs_maxdur[0] < m->hs_maxdur[1]) ? 0 : 1;
            m->vsync_pol = (m->vs_maxdur[0] < m->vs_maxdur[1]) ? 0 : 1;
            m->pol_locked = true;
            // Begin line-period recovery from the next event.
            m->prev_in_hsync = (hs == m->hsync_pol);
            m->prev_in_vsync = (vs == m->vsync_pol);
            m->has_prev_event = false;
            m->have_last_hlead = false;
            m->have_line_lead = false;
            m->hlead_warm = 0;
            m->t_line = 0.0;
            m->lines_this_frame = 0;
            m->line_count = 0;
            m->prev_v_total = -2;
        }
        return;
    }

    const bool in_hsync = (hs == m->hsync_pol);
    const bool in_vsync = (vs == m->vsync_pol);
    const bool hlead = in_hsync && !m->prev_in_hsync;
    const bool vlead = in_vsync && !m->prev_in_vsync;

    // --- Phase B (pre-lock): measure the line period and lines/frame from the
    //     sync signals only. When two consecutive frames agree on the line count
    //     and it matches a standard mode, derive the dot clock and the active
    //     window from that mode and lock. No RGB is consulted. ---
    if (!m->locked) {
        if (hlead) {                              // line period, between hsync leads
            if (m->have_last_hlead) {
                const double dt = t - m->last_hlead_t;
                if (dt > 0.0) {
                    if (m->hlead_warm < 2)
                        m->hlead_warm++;          // skip startup settling
                    else if (m->t_line <= 0.0 || dt < m->t_line)
                        m->t_line = dt;           // a constant run -> the line period
                }
            }
            m->last_hlead_t = t;
            m->have_last_hlead = true;
            m->lines_this_frame++;
        }

        if (vlead) {
            const int v_total = m->lines_this_frame;
            m->lines_this_frame = 0;
            // Require two consecutive frames to agree (the first frame after
            // polarity lock is partial).
            if (v_total == m->prev_v_total && m->t_line > 0.0) {
                const double hs_ratio = m->hs_maxdur[m->hsync_pol] / m->t_line;
                if (const VgaMode* mode = match_mode(v_total, hs_ratio)) {
                    m->h_active   = mode->h_active;
                    m->v_active   = mode->v_active;
                    m->t_pix      = m->t_line / mode->h_total;
                    // Active opens once blanking (sync + back porch) is done.
                    m->act_offset = mode->h_lead2act * m->t_pix;
                    m->det_yoff   = mode->v_lead2act;
                    m->framebuffer.assign(
                        static_cast<size_t>(m->h_active) * m->v_active, Pixel{0, 0, 0});
                    m->locked = true;
                    // Fresh start for the render phase.
                    m->prev_in_hsync = in_hsync;
                    m->prev_in_vsync = in_vsync;
                    m->has_prev_event = false;
                    m->have_line_lead = false;
                    m->line_count = 0;
                    std::fprintf(stderr,
                        "[vga_monitor] '%s' locked %dx%d (%s, %s hsync, %s vsync, "
                        "%.4g ns/pixel, %.4g ns/line)\n",
                        m->name.c_str(), m->h_active, m->v_active, mode->name,
                        m->hsync_pol ? "positive" : "negative",
                        m->vsync_pol ? "positive" : "negative", m->t_pix, m->t_line);
                    return;
                }
            }
            m->prev_v_total = v_total;
        }

        m->prev_in_hsync = in_hsync;
        m->prev_in_vsync = in_vsync;
        return;
    }

    // --- Locked: point-sample each pixel. Pixel i on the current line is sampled
    //     at  t_line_lead + act_offset + (i + 0.25)*t_pix  -- a quarter period in,
    //     so the sample lands inside the pixel, clear of its edge transitions. The
    //     color held over the run [e_prev_time, t) paints every pixel whose sample
    //     time falls in that interval. ---
    if (m->has_prev_event && m->have_line_lead) {
        const int ay = m->line_count - m->det_yoff;
        if (ay >= 0 && ay < m->v_active) {
            const double base = m->t_line_lead + m->act_offset + 0.25 * m->t_pix;
            int i0 = static_cast<int>(std::ceil((m->e_prev_time - base) / m->t_pix - 1e-9));
            int i1 = static_cast<int>(std::ceil((t              - base) / m->t_pix - 1e-9));
            if (i0 < 0)           i0 = 0;
            if (i1 > m->h_active) i1 = m->h_active;
            for (int i = i0; i < i1; ++i)
                m->framebuffer[static_cast<size_t>(ay) * m->h_active + i] = m->e_prev_color;
        }
    }

    if (hlead) {                                  // hsync leading edge: retime line
        m->t_line_lead = t;
        m->have_line_lead = true;
        m->line_count++;
    }
    if (vlead) {                                  // vsync leading edge: frame done
        on_frame_complete(m);
        m->line_count = 0;
        m->have_line_lead = false;
    }

    m->prev_in_hsync = in_hsync;
    m->prev_in_vsync = in_vsync;
    m->has_prev_event = true;
    m->e_prev_time = t;
    m->e_prev_color = Pixel{r, g, b};
}

VGA_EXPORT void vga_monitor_close(void* handle) {
    Monitor* m = static_cast<Monitor*>(handle);
    if (!m) return;
    if (m->stream_fd != kBadSock) { sock_close(m->stream_fd); m->stream_fd = kBadSock; }
    if (m->listen_fd != kBadSock) { sock_close(m->listen_fd); m->listen_fd = kBadSock; }
    delete m;
}
