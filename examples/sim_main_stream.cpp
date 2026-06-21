// sim_main_stream.cpp -- clock driver for the single-monitor raw-rgb24 stream
// demo (stream-dpi). Same --timing-free clock as the other demos.

#include "Vtb_stream.h"
#include "sim_clock_main.h"

int main(int argc, char** argv) {
    return run_clock_main<Vtb_stream>(argc, argv);
}
