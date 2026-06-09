// Verilator harness top. Drives clock + reset from C++, runs until $finish.
// Clock: rising edge every 10 time units. Reset: low for first 4 cycles.
// Timeout: 100 000 cycles.

#include "Vtb_top.h"
#include "verilated.h"
#include <cstdio>
#include <memory>

extern "C" void cmodel_finalize(void);

// Legacy SystemC timestamp stub.
// IMPORTANT: must NOT call VerilatedContext::time() — doing so triggers
//   vl_time_stamp64() → sc_time_stamp() infinite mutual recursion that
//   exhausts the stack (STATUS_STACK_OVERFLOW).
// Actual simulation time is tracked via contextp->timeInc(); this stub is
// only consulted by Verilator's pre-4.0 legacy path when m_s.m_time == 0.
double sc_time_stamp() {
    return 0.0;
}

static constexpr uint64_t RESET_CYCLES = 4;
static constexpr uint64_t TIMEOUT_CYCLES = 100000;

int main(int argc, char** argv) {
    auto contextp = std::make_unique<VerilatedContext>();
    contextp->commandArgs(argc, argv);  // forwards +scenario=... to $value$plusargs
    contextp->threads(1);

    auto top = std::make_unique<Vtb_top>(contextp.get());

    top->clk_i = 0;
    top->rst_ni = 0;
    top->eval();

    uint64_t cycle = 0;
    while (!contextp->gotFinish()) {
        contextp->timeInc(5);
        top->clk_i = 1;
        top->rst_ni = (cycle >= RESET_CYCLES) ? 1 : 0;
        top->eval();

        if (cycle >= TIMEOUT_CYCLES) {
            std::puts("FAIL: timeout (100 000 cycles)");
            cmodel_finalize();
            top->final();
            return 1;
        }

        // Check finish after rising edge — skip falling edge if done.
        if (contextp->gotFinish()) break;

        contextp->timeInc(5);
        top->clk_i = 0;
        top->eval();
        ++cycle;
    }

    cmodel_finalize();
    top->final();
    return contextp->gotError() ? 1 : 0;
}
