// sim_main.cpp -- clock driver for the two-monitor stream demo (stream-two).
// See sim_clock_main.h for why this exists (built WITHOUT Verilator --timing).

#include "Vtb_two_monitors.h"
#include "sim_clock_main.h"

int main(int argc, char** argv) {
    return run_clock_main<Vtb_two_monitors>(argc, argv);
}
