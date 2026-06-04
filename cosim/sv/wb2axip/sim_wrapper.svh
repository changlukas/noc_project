// Macro shim allowing ZipCPU wb2axip formal properties to run as
// simulation-runtime observers. Source files faxi_master.v / faxi_slave.v
// are byte-identical to upstream; this file is the ONLY adaptation layer.
//
// Three changes vs formal:
//   1. SLAVE_ASSUME / SLAVE_ASSERT both map to `assert` (formal `assume` has
//      no simulation semantic; treat the constraint as a directly-checked
//      invariant on our DUT).
//   2. `f_past_valid` requires explicit init to 0 (Verilator default-X
//      would make $past() reads UB on first cycle).
//   3. Macro guard so this file can be safely included multiple times.

`ifndef WB2AXIP_SIM_WRAPPER_SVH
`define WB2AXIP_SIM_WRAPPER_SVH

`define SLAVE_ASSUME assert
`define SLAVE_ASSERT assert

// To map standalone `assume(...)` inside wb2axip into `assert(...)`, see
// the Verilator build flag in cosim/verilator/Makefile:
//     -Dassume=assert

`endif // WB2AXIP_SIM_WRAPPER_SVH
