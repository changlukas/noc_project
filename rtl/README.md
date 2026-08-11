# rtl — synthesisable RTL

Empty until the RTL is written. One directory per block, matching the c_model's
sub-namespaces: `nmu/`, `nsu/`, `router/`, plus `common/` for anything shared.

## The port contract

One testbench serves both DUTs. `sim/tb/` drives the C++ behavioural model today
through the DPI wrappers in `ref_model/top/`, and will drive the RTL through the
same ports, so **a block's RTL takes the port list its wrapper already has**:

| block | the wrapper that fixes the ports |
|---|---|
| NMU | `ref_model/top/nmu_wrap.sv` |
| NSU | `ref_model/top/nsu_wrap.sv` |
| router | `ref_model/top/router_wrap.sv` |

The wrappers carry a `longint unsigned ctx` handle that the RTL will not have,
and that is the one difference to resolve when the first block lands — either
the RTL ignores it or the tb selects between two instantiations. Everything
else, the AXI faces and the NoC link faces, is the contract.

## Why it matters

Same stimulus, same seed, both DUTs: `sim/tools/gen_test_patterns.py` produces
one set of files and either DUT replays it. That is what makes the C++ model a
*reference* model rather than a separate implementation — RTL output can be
compared against it beat for beat instead of only against a scoreboard.
