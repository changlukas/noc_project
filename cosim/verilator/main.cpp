// Verilator harness top. Verilator generates Vtb_axi_conformity.{h,cpp};
// this main creates one instance and drives eval() until $finish.

#include "Vtb_axi_conformity.h"
#include "verilated.h"
#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);  // forwards +scenario=... to $value$plusargs

    auto top = std::make_unique<Vtb_axi_conformity>(contextp.get());

    while (!contextp->gotFinish()) {
        top->eval();
        contextp->timeInc(1);
    }

    return contextp->gotError() ? 1 : 0;
}
