// sim_clock_main.h
//
// Shared clock driver for the --timing-free Verilator demos. Toggles clk and
// advances simulation time so $realtime (which every monitor event is stamped
// with) keeps increasing -- the fast path, no #-delays in the RTL. Each thin
// main just picks the top module: run_clock_main<Vtb_xxx>(argc, argv).

#ifndef SIM_CLOCK_MAIN_H
#define SIM_CLOCK_MAIN_H

#include "verilated.h"

template <class TOP>
static int run_clock_main(int argc, char** argv) {
    VerilatedContext* ctx = new VerilatedContext;
    ctx->commandArgs(argc, argv);

    TOP* top = new TOP{ctx};

    top->reset = 1;
    top->clk   = 0;
    for (int i = 0; i < 8; ++i) {        // a few cycles held in reset
        top->clk = !top->clk;
        ctx->timeInc(5);                 // half a clock period
        top->eval();
    }
    top->reset = 0;

    while (!ctx->gotFinish()) {           // free-run; backend ends the process
        top->clk = !top->clk;
        ctx->timeInc(5);
        top->eval();
    }

    top->final();
    delete top;
    delete ctx;
    return 0;
}

#endif  // SIM_CLOCK_MAIN_H
