// Macro shim allowing ZipCPU wb2axip formal properties to run as
// simulation-runtime observers. Source files faxi_master.v / faxi_slave.v
// are byte-identical to upstream; this file is the ONLY adaptation layer.
//
// faxi_master.v and faxi_slave.v each define SLAVE_ASSUME and SLAVE_ASSERT
// internally (with inverse semantics per file role). We do NOT redefine
// them here to avoid conflicting with those per-file definitions.
//
// Standalone `assume(...)` calls inside wb2axip are mapped to `assert` via
// the Verilator build flag in cosim/verilator/Makefile:
//     +define+assume=assert

`ifndef WB2AXIP_SIM_WRAPPER_SVH
`define WB2AXIP_SIM_WRAPPER_SVH

`endif // WB2AXIP_SIM_WRAPPER_SVH
