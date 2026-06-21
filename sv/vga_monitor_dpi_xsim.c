/* vga_monitor_dpi_xsim.c -- DPI trampoline for Vivado XSim (Windows).
 *
 * XSim links DPI through its own bundled GCC and rejects a foreign-built
 * library (the MSVC DLL, and a MinGW .a of the real backend fails on
 * libstdc++/UCRT/winsock ABI). This shim avoids all of that: it uses ONLY
 * kernel32 (LoadLibrary/GetProcAddress + GetEnvironmentVariable/lstrcpy), so a
 * prebuilt MinGW vga_monitor_dpi.a links cleanly under XSim's gcc. On first DPI
 * call it loads the self-contained vga_monitor_dpi.dll and forwards.
 *
 * The DLL name defaults to "vga_monitor_dpi.dll"; override with the
 * VGA_MONITOR_DLL environment variable (e.g. a versioned release filename).
 *
 * Build: gcc -O2 -c vga_monitor_dpi_xsim.c && ar rcs vga_monitor_dpi.a *.o
 * Use:   xelab my_tb -sv_root <dir> -sv_lib vga_monitor_dpi   (DLL on PATH)
 */
#include <windows.h>

typedef void* (*open_fn)(const char*);
typedef void  (*event_fn)(void*, double, unsigned char, unsigned char,
                          unsigned char, int, int);
typedef void  (*close_fn)(void*);

static open_fn  p_open;
static event_fn p_event;
static close_fn p_close;
static int      tried;

static void ensure(void) {
    if (tried) return;
    tried = 1;
    char dll[MAX_PATH];
    if (GetEnvironmentVariableA("VGA_MONITOR_DLL", dll, sizeof dll) == 0)
        lstrcpyA(dll, "vga_monitor_dpi.dll");
    HMODULE h = LoadLibraryA(dll);
    if (!h) return;
    p_open  = (open_fn)  (void*)GetProcAddress(h, "vga_monitor_open");
    p_event = (event_fn) (void*)GetProcAddress(h, "vga_monitor_event");
    p_close = (close_fn) (void*)GetProcAddress(h, "vga_monitor_close");
}

void* vga_monitor_open(const char* name) { ensure(); return p_open ? p_open(name) : 0; }
void  vga_monitor_event(void* h, double t, unsigned char r, unsigned char g,
                        unsigned char b, int hsync, int vsync) {
    ensure(); if (p_event) p_event(h, t, r, g, b, hsync, vsync);
}
void  vga_monitor_close(void* h) { ensure(); if (p_close) p_close(h); }
