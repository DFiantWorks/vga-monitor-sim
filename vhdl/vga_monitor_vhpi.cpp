// vga_monitor_vhpi.cpp
//
// Thin VHPIDIRECT shim that lets GHDL drive the shared, simulator-agnostic
// monitor backend in ../backend/vga_monitor.cpp (the same core the SystemVerilog
// DPI flow uses). VHPIDIRECT marshals VHDL integers, not pointers, so each
// monitor instance is referred to by an integer id into a small registry here;
// `out integer` handles arrive as int*.

#include <cstdint>
#include <vector>

// Export the foreign symbols from a DLL on Windows (MSVC needs this explicitly;
// GCC/MinGW exports by default). A no-op decoration elsewhere.
#ifdef _WIN32
  #define VHPI_EXPORT extern "C" __declspec(dllexport)
#else
  #define VHPI_EXPORT extern "C"
#endif

// Core API (defined in ../backend/vga_monitor.cpp).
extern "C" {
void* vga_monitor_open(const char* name);
void  vga_monitor_event(void* handle, double t, uint8_t r, uint8_t g, uint8_t b,
                        int hsync, int vsync);
}

namespace {
std::vector<void*> g_handles;   // id -> core handle
}

VHPI_EXPORT void vhpi_monitor_open(int* handle) {
    void* h = vga_monitor_open("VGA Monitor (VHDL)");
    g_handles.push_back(h);
    *handle = static_cast<int>(g_handles.size()) - 1;
}

VHPI_EXPORT void vhpi_monitor_event(int handle, double t, int r, int g, int b,
                                    int hsync, int vsync) {
    vga_monitor_event(g_handles[handle], t,
                      static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                      static_cast<uint8_t>(b), hsync, vsync);
}
