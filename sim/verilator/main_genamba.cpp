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

// VCD tracing — compiled in only when verilated with --trace (TRACE=1 in
// sim/verilator/Makefile defines VM_TRACE). Path comes from +vcd=<abs-path>;
// the run recipe supplies output/genamba_<scenario>/tb_genamba.vcd.
#if VM_TRACE
#include "verilated_vcd_c.h"
#include <string>
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

#if VM_TRACE
    contextp->traceEverOn(true);
    const std::unique_ptr<VerilatedVcdC> tfp{new VerilatedVcdC};
    topp->trace(tfp.get(), 99);  // 99 = full hierarchy depth
    std::string vcd_path = "tb_genamba.vcd";
    {
        const std::string m = contextp->commandArgsPlusMatch("vcd=");
        if (m.rfind("+vcd=", 0) == 0) vcd_path = m.substr(5);
    }
    tfp->open(vcd_path.c_str());
#endif

    // Simulate until $finish
    while (!contextp->gotFinish()) {
        topp->eval();
#if VM_TRACE
        tfp->dump(contextp->time());
#endif
        if (!topp->eventsPending()) break;
        contextp->time(topp->nextTimeSlot());
    }

    if (!contextp->gotFinish()) {
        VL_DEBUG_IF(VL_PRINTF("+ Exiting without $finish; no events left\n"););
    }

#if VM_TRACE
    tfp->close();
#endif
    topp->final();
    contextp->statsPrintSummary();
    return 0;
}
