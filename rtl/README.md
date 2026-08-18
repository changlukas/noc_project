# RTL - synthesizable RTL

Empty until the RTL is written. One directory per block, matching the c_model's
sub-namespaces: `nmu/`, `nsu/`, `router/`, plus `common/` for anything shared.

## The port contract

One verification environment serves model, RTL, and hybrid compositions. `sim/tb/`
drives the C++ behavioural model today through the DPI wrappers in `ref_model/top/`,
and will drive RTL through the same ports, so **a block's RTL takes the functional
port list its wrapper already has**:

| block | wrapper that fixes the functional ports |
|---|---|
| NMU | `ref_model/top/nmu_wrap.sv` |
| NSU | `ref_model/top/nsu_wrap.sv` |
| Router | `ref_model/top/router_wrap.sv` |

The wrappers carry a `longint unsigned ctx` handle that RTL does not have. The
testbench selects model or RTL independently for NMU, NSU, and Router; only the
model branch consumes the handle. Production RTL must not expose or ignore a dummy
DPI handle. The AXI faces and NoC link faces are the shared contract.

## Why it matters

Same stimulus, same seed, mixed implementations: `sim/tools/gen_test_patterns.py`
produces one set of files. Before a full RTL mesh, block signoff uses RTL NMU with
reference NSU, reference NMU with RTL NSU, and a reference-driven differential
Router harness. The NMU/NSU pair uses a zero-hop test link. REQ and RSP connect
directly; a verification-only DAT adapter translates flow control where the
current model's credit port differs from the target ready/valid contract. The
adapter does not transform packets or implement Router behavior. Full-model and
full-RTL runs remain available for end-to-end comparison and milestone regression.
