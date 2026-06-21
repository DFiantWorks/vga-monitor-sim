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
// memory, and streams each finished frame as raw rgb24 over TCP to a standard
// viewer (e.g. ffplay). No windowing toolkit is linked into the simulator.
//
// Each instance maps to one Monitor (returned as an opaque handle), so multiple
// monitors can run at once, each with its own framebuffer and stream.
//
// Environment variables:
//   VGA_MONITOR_FRAMES=<N>    exit(0) after N complete frames (per process)
//   VGA_MONITOR_STREAM=host:port
//                             once geometry is locked, connect (TCP) and write
//                             each finished frame as raw rgb24 (W*H*3 bytes). A
//                             standard viewer reads it directly, e.g.:
//                               ffplay -f rawvideo -pixel_format rgb24 \
//                                      -video_size 640x480 -i 'tcp://0.0.0.0:5000?listen=1'
//                             With multiple monitors, instance i (by open order)
//                             connects to port+i, so each gets its own viewer.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

// --- Cross-platform TCP socket layer (POSIX + Windows/Winsock) --------------
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  static const sock_t kBadSock = INVALID_SOCKET;
  static int  sock_close(sock_t s) { return ::closesocket(s); }
  static int  sock_send(sock_t s, const char* b, size_t n) { return ::send(s, b, (int)n, 0); }
  static void sock_startup() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
  #define VGA_EXPORT extern "C" __declspec(dllexport)
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netdb.h>
  using sock_t = int;
  static const sock_t kBadSock = -1;
  static int     sock_close(sock_t s) { return ::close(s); }
  static ssize_t sock_send(sock_t s, const char* b, size_t n) { return ::write(s, b, n); }
  static void    sock_startup() {}
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

    // --- Raw-rgb24 TCP stream (VGA_MONITOR_STREAM) ---
    int    index = 0;             // open order; instance i streams to port+i
    sock_t stream_fd = kBadSock;
    bool   stream_tried = false;
};

long g_frame_limit = -1;          // -1 => unlimited
bool g_inited = false;
int  g_open_count = 0;            // monitors opened so far (for per-instance port)
std::string g_stream_target;      // "host:port" or empty (no streaming)

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
}

// Connect (TCP) to the viewer named by VGA_MONITOR_STREAM. Called once, after
// geometry is known, so the raw stream has a fixed, agreed-on frame size.
void stream_connect(Monitor* m) {
    m->stream_tried = true;
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
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "[vga_monitor] '%s' stream: cannot resolve %s:%s\n",
                     m->name.c_str(), host.c_str(), port.c_str());
        return;
    }
    sock_t fd = kBadSock;
    for (auto p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kBadSock) continue;
        if (connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        sock_close(fd); fd = kBadSock;
    }
    freeaddrinfo(res);
    if (fd == kBadSock) {
        std::fprintf(stderr, "[vga_monitor] '%s' stream: connect to %s:%s failed\n",
                     m->name.c_str(), host.c_str(), port.c_str());
        return;
    }
    m->stream_fd = fd;
    std::fprintf(stderr,
        "[vga_monitor] '%s' streaming raw rgb24 %dx%d to %s:%s\n",
        m->name.c_str(), m->h_active, m->v_active, host.c_str(), port.c_str());
}

// Write one finished frame as raw rgb24 (W*H*3 bytes, no header). The Pixel
// vector is already tightly packed r,g,b -- exactly the rawvideo layout.
void stream_send_frame(Monitor* m) {
    if (m->stream_fd == kBadSock) return;
    const char* p = reinterpret_cast<const char*>(m->framebuffer.data());
    size_t n = m->framebuffer.size() * sizeof(Pixel);
    while (n) {
        auto k = sock_send(m->stream_fd, p, n);
        if (k <= 0) {
#ifndef _WIN32
            if (k < 0 && errno == EINTR) continue;
#endif
            std::fprintf(stderr, "[vga_monitor] '%s' stream: viewer gone, stopping\n",
                         m->name.c_str());
            sock_close(m->stream_fd); m->stream_fd = kBadSock;
            return;
        }
        p += k; n -= static_cast<size_t>(k);
    }
}

// Called once per complete frame (on the leading edge of the vsync pulse, after
// the active region has been fully drawn).
void on_frame_complete(Monitor* m) {
    m->frames_done++;

    if (!g_stream_target.empty()) {
        if (m->stream_fd < 0 && !m->stream_tried) stream_connect(m);
        stream_send_frame(m);
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
    delete m;
}
