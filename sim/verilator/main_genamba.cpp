// Verilated main for tb_genamba (--timing mode, two-phase build).
// Mirrors the main Verilator --binary would auto-generate; kept separate
// so 'make genamba' can use --cc --exe --timing + explicit $(MAKE) -C,
// avoiding the Windows/MSYS2 nested-make subprocess issue in --binary mode.

#include "verilated.h"
// GENAMBA_TOP selects the verilated top class. Default: Vtb_genamba.
// The tester build (run-genamba-tester) passes -DGENAMBA_TESTER_TOP so
// this one main serves both tops.
#ifdef GENAMBA_TESTER_TOP
#include "Vtb_genamba_tester.h"
#define GENAMBA_TOP Vtb_genamba_tester
#else
#include "Vtb_genamba.h"
#define GENAMBA_TOP Vtb_genamba
#endif

// Legacy SystemC timestamp stub required by verilated.cpp when compiled
// without -DVL_TIME_CONTEXT (which --cc mode does not set by default).
double sc_time_stamp() {
    return 0.0;
}

int main(int argc, char** argv, char**) {
    Verilated::debug(0);
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->commandArgs(argc, argv);

    const std::unique_ptr<GENAMBA_TOP> topp{new GENAMBA_TOP{contextp.get(), ""}};

    // Simulate until $finish
    while (!contextp->gotFinish()) {
        topp->eval();
        if (!topp->eventsPending()) break;
        contextp->time(topp->nextTimeSlot());
    }

    if (!contextp->gotFinish()) {
        VL_DEBUG_IF(VL_PRINTF("+ Exiting without $finish; no events left\n"););
    }

    topp->final();
    contextp->statsPrintSummary();
    return 0;
}
