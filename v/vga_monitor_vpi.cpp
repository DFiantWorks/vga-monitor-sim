// vga_monitor_vpi.cpp
//
// VPI shim that lets a Verilog simulator (e.g. Icarus, which has no DPI support)
// drive the shared, simulator-agnostic monitor backend in
// ../backend/vga_monitor.cpp -- the same core used by the Verilator/DPI and
// GHDL/VHPI flows.
//
// VPI calls the backend through registered system tasks/functions instead of
// DPI imports. As with the VHDL/VHPI shim, each monitor instance is an integer
// id into a registry (no pointer types cross the boundary):
//
//   handle = $vga_monitor_open();                                 // function
//   $vga_monitor_event(handle, $realtime, r, g, b, hsync, vsync); // task
//
// Build into a loadable VPI module and run with:
//   vvp -M<dir> -m vga_monitor sim.vvp

#include <cstdint>
#include <cstring>
#include <vector>
#include <vpi_user.h>

// Core API (defined in ../backend/vga_monitor.cpp).
extern "C" {
void* vga_monitor_open(const char* name);
void  vga_monitor_event(void* handle, double t, uint8_t r, uint8_t g, uint8_t b,
                        int hsync, int vsync);
}

namespace {

std::vector<void*> g_handles;   // id -> core handle

// Collect all arguments of the current system-task/function call. Scanning to
// the end auto-frees the iterator (no leak across the many per-event calls).
int collect_args(vpiHandle systf, vpiHandle* out, int maxn) {
    vpiHandle it = vpi_iterate(vpiArgument, systf);
    if (!it) return 0;
    int n = 0;
    vpiHandle a;
    while ((a = vpi_scan(it)) != nullptr) {
        if (n < maxn) out[n] = a;
        ++n;
    }
    return n;
}

int as_int(vpiHandle a) {
    s_vpi_value v;
    v.format = vpiIntVal;
    vpi_get_value(a, &v);
    return v.value.integer;
}

double as_real(vpiHandle a) {
    s_vpi_value v;
    v.format = vpiRealVal;
    vpi_get_value(a, &v);
    return v.value.real;
}

void put_int_return(vpiHandle systf, int value) {
    s_vpi_value rv;
    rv.format = vpiIntVal;
    rv.value.integer = value;
    vpi_put_value(systf, &rv, nullptr, vpiNoDelay);
}

PLI_INT32 open_calltf(PLI_BYTE8*) {
    vpiHandle systf = vpi_handle(vpiSysTfCall, nullptr);
    void* h = vga_monitor_open("VGA Monitor (Verilog)");
    g_handles.push_back(h);
    put_int_return(systf, static_cast<int>(g_handles.size()) - 1);
    return 0;
}

PLI_INT32 event_calltf(PLI_BYTE8*) {
    vpiHandle systf = vpi_handle(vpiSysTfCall, nullptr);
    vpiHandle a[7];
    if (collect_args(systf, a, 7) < 7) return 0;
    vga_monitor_event(g_handles[as_int(a[0])], as_real(a[1]),
                      static_cast<uint8_t>(as_int(a[2])),
                      static_cast<uint8_t>(as_int(a[3])),
                      static_cast<uint8_t>(as_int(a[4])),
                      as_int(a[5]), as_int(a[6]));
    return 0;
}

void reg_func(const char* name, PLI_INT32 (*calltf)(PLI_BYTE8*)) {
    s_vpi_systf_data d;
    std::memset(&d, 0, sizeof(d));
    d.type = vpiSysFunc;
    d.sysfunctype = vpiIntFunc;
    d.tfname = const_cast<PLI_BYTE8*>(name);
    d.calltf = calltf;
    vpi_register_systf(&d);
}

void reg_task(const char* name, PLI_INT32 (*calltf)(PLI_BYTE8*)) {
    s_vpi_systf_data d;
    std::memset(&d, 0, sizeof(d));
    d.type = vpiSysTask;
    d.tfname = const_cast<PLI_BYTE8*>(name);
    d.calltf = calltf;
    vpi_register_systf(&d);
}

void register_monitor_tf() {
    reg_func("$vga_monitor_open", open_calltf);
    reg_task("$vga_monitor_event", event_calltf);
}

}  // namespace

extern "C" {
void (*vlog_startup_routines[])() = {
    register_monitor_tf,
    nullptr
};
}
