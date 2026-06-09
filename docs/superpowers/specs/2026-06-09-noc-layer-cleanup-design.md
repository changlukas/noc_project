# NoC layer cleanup — rename + unify intf

**Date:** 2026-06-09

Two coupled cleanups on the c_model NoC layer:

1. Rename `LoopbackNoc` → `ChannelModel`. Not actually a loopback (master sends r/w op; slave generates response). Behavioural NoC channel model between NMU and NSU.
2. Merge `noc_req_intf` + `noc_rsp_intf` → ONE `noc_intf` with `mosi`/`miso` modports. NMU/NSU module port lists go from 1 AXI + 2 NoC → 1 AXI + 1 NoC (symmetric with `axi4_intf`).

## Ground truth

- `cosim/sv/nmu_wrap.sv:45-47` — current asymmetric port list
- `specgen/generated/sv/ni_signals_pkg.sv:69-160` — current 3 interface blocks
- `specgen/source/interface_handshake.json:16` — SV interface definition source (NOT ni_signals.json)
- `cosim/c/cmodel_dpi.cpp:46-52` — 5-singleton invariant (out of scope; separate session)

## Design

### New SV interface

```sv
interface noc_intf #(
  parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
  parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
  parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
);
  logic                  req_valid;
  logic [FLIT_WIDTH-1:0] req_flit;
  logic [NUM_VC-1:0]     req_credit_return;
  logic                  rsp_valid;
  logic [FLIT_WIDTH-1:0] rsp_flit;
  logic [NUM_VC-1:0]     rsp_credit_return;

  modport mosi (output req_valid, req_flit, rsp_credit_return,
                input  req_credit_return, rsp_valid, rsp_flit);
  modport miso (input  req_valid, req_flit, rsp_credit_return,
                output req_credit_return, rsp_valid, rsp_flit);
endinterface : noc_intf
```

### Module port list

| Module | Before | After |
|---|---|---|
| `nmu_wrap` | `axi4_intf.slave axi_i, noc_req_intf.master noc_req_o, noc_rsp_intf.slave noc_rsp_i` | `axi4_intf.slave axi_i, noc_intf.mosi noc_o` |
| `nsu_wrap` | `axi4_intf.master axi_o, noc_req_intf.slave noc_req_i, noc_rsp_intf.master noc_rsp_o` | `axi4_intf.master axi_o, noc_intf.miso noc_o` |
| `channel_model_wrap` (was `loopback_noc_wrap`) | 4 NoC ports | `noc_intf.miso noc_nmu_side, noc_intf.mosi noc_nsu_side` |

`axi4_intf` keeps `master`/`slave` modports; only `noc_intf` uses `mosi`/`miso`.

### Renames (C1)

`git mv`:

| Before | After |
|---|---|
| `cosim/sv/loopback_noc_wrap.sv` | `channel_model_wrap.sv` |
| `c_model/include/cosim/loopback_noc_shell_adapter.hpp` | `channel_model_shell_adapter.hpp` |
| `c_model/include/cosim/loopback_noc_shell_io.hpp` | `channel_model_shell_io.hpp` |
| `c_model/tests/common/loopback_noc.hpp` | `channel_model.hpp` |
| `c_model/tests/cosim/test_loopback_noc_shell_adapter.cpp` | `test_channel_model_shell_adapter.cpp` |
| `c_model/tests/common/test_loopback_noc_per_vc_credit.cpp` | `test_channel_model_per_vc_credit.cpp` |

Identifiers:

| Before | After |
|---|---|
| `LoopbackNoc`, `LoopbackNocShellAdapter`, `LoopbackNocInputs`, `LoopbackNocOutputs` | `ChannelModel`, `ChannelModelShellAdapter`, `ChannelModelInputs`, `ChannelModelOutputs` |
| SV module `loopback_noc_wrap` | `channel_model_wrap` |
| DPI `cmodel_loopback_noc_{tick,set_inputs,get_outputs}` | `cmodel_channel_model_*` |
| `loopback_noc_req_depth`, `loopback_noc_rsp_depth` (port_params.hpp) + `loopback_noc:` YAML key (port_params.yaml:37) | `channel_model_*` |

Content updates (no rename): `cmodel_dpi.{cpp,h}`, `cosim/sv/{nmu,nsu,tb_top}_wrap.sv`, `cosim/verilator/Makefile`, `c_model/include/{nmu/nmu.hpp, cosim/{flit_byte_conv,poc_defaults,port_params}.hpp, noc/noc_{req,rsp}_{in,out}.hpp comments}`, 11 c_model tests (test_packetize, test_depacketize, test_nmu, test_vc_arbiter, test_rob, test_nsu_depacketize, test_nsu_vc_arbiter, test_nsu_packetize, test_nsu, test_loopback_latency, test_request_response_loopback), 2 CMakeLists.txt, scenario AX4-STR-003 yaml, `docs/architecture.md`, `CLAUDE.md`, this spec.

Files preserving "loopback" in name (refers to topology / latency feature, not the class): `test_loopback_latency.cpp`, `test_request_response_loopback.cpp` — only `#include` + class references update.

`docs/_archive/*` + `docs/superpowers/{specs,plans}/2026-06-0[2-8]-*` untouched.

## Commit chain

| # | Type | Scope |
|---|---|---|
| C1 | `refactor: rename LoopbackNoc -> ChannelModel` | 6 `git mv` + all identifier + config-key renames + ~20 content refs. Pure rename. `make check` clean. |
| C2 | `refactor(specgen): merge ni_signals.json NoC interfaces` | NMU `NOC_REQ_OUT+NOC_RSP_IN` → `NOC_INTF_MOSI`; NSU symmetric; ChannelModel symmetric. Regen `ni_signals.h`. Refresh golden. Update `test_signals_schema.py`, `test_signals_resolver.py`, `test_pins_smoke.cpp`. |
| C3 | `refactor(specgen): merge interface_handshake.json + mosi/miso modports` | Merge 2 NoC entries → 1 in `interface_handshake.json`; `sv_signals.py` drives modport names from JSON (axi4 → master/slave, noc → mosi/miso). Regen `ni_signals_pkg.sv`. Refresh golden. Update `test_handshake_schema.py`, `test_codegen_sv.py`. |
| C4 | `refactor(cosim/sv): wraps use noc_intf.mosi/miso` | nmu/nsu/channel_model/tb_top: port list + signal references (`noc_req_o.valid` → `noc_o.req_valid`). DPI signatures unchanged. |

C1 first so C2-C4 use new names. C5 (c_model shell_io merge) was speculation — structs are already flat, codegen handles it.

## Out of scope

- Multi-instance NMU/NSU (5-singleton invariant in `cmodel_dpi.cpp:46-52`) — separate session.
- Credit-based flow-control protocol changes (still single-VC PoC).
- `direction: "in/out"` field semantics in ni_signals.json.

## Success criteria

- `codegen.py --check` exit 0
- `pytest specgen/tests/` all PASS
- NMU/NSU each declare 1 NoC port; ChannelModel declares 2 NoC ports
- `make check` clean (modulo pre-existing GCC ICE)
- No surviving `LoopbackNoc` / `loopback_noc` / `cmodel_loopback_noc` / `noc_req_intf` / `noc_rsp_intf` outside `docs/_archive/` and `docs/superpowers/specs|plans/2026-06-0[2-8]-*`

## Risks

- Config-key + DPI symbol renames must be atomic per commit (YAML key + loader; .h decl + .cpp defn + SV import).
- Goldens refresh atomically with the JSON change in the same commit (C2 + C3).
- `test_codegen_sv.py` may assert old interface name substrings — update expectation.
- Stage 5b `NUM_VC=1` elaboration guards preserved in renamed SV files.
- `cosim/verilator/Makefile` references `loopback_noc_wrap.sv` by path — must update in C1.
