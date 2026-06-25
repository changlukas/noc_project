// Verilator --timing entry for tb_top_spike.
// sc_time_stamp stub required when verilated.cpp is linked without SystemC.
#include "Vtb_top_spike.h"
#include "verilated.h"
#include <memory>

double sc_time_stamp() { return 0.0; }

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);
    contextp->threads(1);
    auto top = std::make_unique<Vtb_top_spike>(contextp.get());
    while (!contextp->gotFinish()) {
        top->eval();
        if (!top->eventsPending()) break;
        contextp->time(top->nextTimeSlot());
    }
    top->final();
    return contextp->gotError() ? 1 : 0;
}
