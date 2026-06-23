// Verilator --timing entry for the self-clocked tb_top. Clock/reset/timeout/
// perf/finalize all live in tb_top.sv; this just advances the event loop.
#include "Vtb_top.h"
#include "verilated.h"
#include <memory>

// Legacy SystemC timestamp stub (must NOT call VerilatedContext::time()).
double sc_time_stamp() { return 0.0; }

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);  // forwards +scenario_node*/+perf_* to SV
    contextp->threads(1);
    auto top = std::make_unique<Vtb_top>(contextp.get());
    while (!contextp->gotFinish()) {
        top->eval();
        if (!top->eventsPending()) break;
        contextp->time(top->nextTimeSlot());
    }
    top->final();  // fires tb_top SV final (perf dump + cmodel_finalize)
    return contextp->gotError() ? 1 : 0;
}
