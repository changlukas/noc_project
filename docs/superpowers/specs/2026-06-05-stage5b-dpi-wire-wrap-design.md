# Stage 5b — DPI wire-wrap co-sim (registered-wire β model) — design

**Status**: design (brainstorm output, ready for spec review + writing-plans)
**Date**: 2026-06-05
**Branch**: TBD `stage5b/dpi-wire-wrap` (to be created off Stage 5a tip `0a8849c`)
**Predecessor**: Stage 5a `stage5/axi-checker-cosim` (23 commits, ctest 398/398, KNOWN_LIMITATIONS §2 unresolved)

## 1. Motivation

Stage 5a shipped a working wb2axip-on-c_model PoC where the SV side observes c_model AXI boundary via per-cycle pin snapshots emitted through DPI. The snapshot model captures only the front of c_model's internal queues per tick — so when `Nmu::tick()` drains an entire multi-beat W burst in one C++ tick, beats W[1..N-1] never reach the SV checker. KNOWN_LIMITATIONS §2 documents this scope ceiling.

Stage 5b removes the snapshot abstraction by wrapping each c_model component in its own SV shell module. Components communicate via real, registered SV wires (β tick discipline: 1-cycle latency per hop, same semantic as fully-flopped AXI). Multi-beat / multi-outstanding traffic becomes wire-visible; wb2axip properties using `$past` / `$stable` get a real signal history to inspect.

The architectural prize is also future-facing: once each c_model component sits behind an SV shell with a wire-level external contract, swapping that component for real RTL (`nmu.sv`, `nsu.sv`, etc.) becomes a one-line SV instance swap — no DPI / proxy / harness rewrite. Stage 5b is the infrastructure layer that unblocks `main plan §5.2` ComponentHandle vision without committing to the full virtual-class abstraction.

## 2. Scope

**In scope**:
- 5 SV wrap modules: `axi_master_wrap`, `nmu_wrap`, `loopback_noc_wrap`, `nsu_wrap`, `axi_slave_wrap`
- 5 C++ ShellAdapter classes (one per component), enforcing hermetic singleton invariant
- 3 SV interface definitions: `axi_intf` (with full AXI handshake), `noc_req_intf`, `noc_rsp_intf` (both parameterized over `NUM_VC`)
- DPI surface: 3-step per shell (`cmodel_<comp>_set_inputs` → `cmodel_<comp>_tick` → `cmodel_<comp>_get_outputs`) plus lifecycle + error propagation
- Top-level `tb_top.sv` instantiating 5 shells, 6 wire bundles, 2 wb2axip checkers
- Reuse Stage 5a's vendored `wb2axip/` + Verilator MSYS2 build pattern
- Additive c_model header changes: standalone ctor overloads, `can_accept_*()` const queries, plusarg-gated `inject_violation()` API
- 5 ctest scenarios (simplified per `feedback-test-meaningfulness-over-count`)
- rtl-forge `rtl-style` skill + karpathy-guidelines + `rtl-reviewer` agent integration

**Out of scope** (deferred to follow-up):
- `main plan §5.2` ComponentHandle SV virtual class abstraction
- RTL replacement of any c_model component (next stage builds on top of 5b)
- Stage 4 NoC fabric (router still doesn't exist as c_model or RTL)
- VCS DPI-RTL backend (Verilator first)
- Async clock domain crossing (assume `aclk == noc_clk`)
- Performance counter comparison
- Layer 3 cross-comparison tool ((a) vs (b) beat sequence diff)
- specgen handshake field upstream (Phase 2)
- 256-beat / 4KB-cross / multi-VC stress scenarios
- `cosim2/` → `cosim/` rename + delete (a) artifacts (separate clean commit after 5b stable)

## 3. Anchored decisions

| Decision | Choice | Source |
|---|---|---|
| Tick discipline | β: registered output, 1-cycle latency per hop | Section 2 Q2 |
| Architecture | Approach 1: 5 hard-coded SV shells + manual wire interconnect in `tb_top` | Section 4 Approach selection |
| DPI shape | Batched per shell: `set_inputs` + `tick` + `get_outputs` (3 calls per shell per cycle) | Codex Section 3 fix |
| Hermetic singleton invariant | C++ components never directly method-call each other; only path = SV wire | Section 3 Codex review pushback |
| ctor compatibility | Overloaded ctors: existing `Nmu(NmuConfig, NocReqOut&, NocRspIn&)` retained, new `Nmu(NmuConfig)` standalone added | Codex Section 6+7 patch #1 |
| AW handshake closes when | Master's `*valid_q[t]` AND slave's `*ready_q[t]` both high in same SV cycle | Codex Section 4 High #1 |
| Backpressure rule | `tick()` sequence: sample valid → query `can_accept_*()` → push iff both true → ALWAYS set `out.ready = can_accept_*_next_cycle()` | Codex Section 4 High #2 |
| `can_accept_*()` semantic | Returns tick-end capacity (post-tick state); ShellAdapter calls at tick end only | Codex Section 6+7 Medium #3 |
| NoC interface parameterization | `parameter int NUM_VC`; `credit_return [NUM_VC-1:0]` | Codex Section 4 High #3 |
| Error propagation | C++ try/catch sets error code; SV shell tick end inline check + `$fatal(1)` on error | Codex Section 3 Medium #4 + Section 4 (b) |
| Fault injection | c_model `AxiMaster` runtime plusarg `+inject=<mode>=<cycle>`; allowlist + `$fatal` on unknown | Codex Section 5 (b) + Section 6+7 Medium #2 |
| wb2axip `F_AXI_MAXSTALL` | Parametric formula `[UNVERIFIED]`; implementation gated by sanity-check of wb2axip source | Codex Section 4 Medium + Section 6+7 Low #4 |
| Reset | Synchronous, single global `rst_ni` driven by `main.cpp` (carry from 5a) | Codex Section 3 (b) |
| Verilator | 5.036 on Windows MSYS2 (proven in 5a Task 8) | 5a result |
| Branch | `stage5b/dpi-wire-wrap` off Stage 5a tip `0a8849c` | Section 6 |
| (a) artifact deletion | Single dedicated commit early in 5b; carries wb2axip vendor to `cosim2/sv/wb2axip/` | Codex (c) |
| Interface naming | No `cosim` prefix on SV / CMake / library names (drop redundancy with dir name) | User Section 7 feedback |
| Module / signal naming | `_i / _o / _q / _d / _ni` per `rtl-style` skill; sync reset default | rtl-forge SKILL.md |
| Coding discipline | `rtl-style` + `karpathy-guidelines` skills authoritative during writing; `rtl-reviewer` agent + Codex during review | User Section 6+7 directive |
| Specgen handshake | Phase 1 cosim2-local; Phase 2 upstream into specgen `ni_signals_pkg.sv` (separate spec) | User Section 4 directive + Codex (b) |

## 4. Architecture

### 4.1 Topology

```
                C++ side (5 hermetic singletons, each DPI-callable)
   ┌────────────────────────────────────────────────────────────────────┐
   │ C++ AxiMaster   C++ Nmu   C++ LoopbackNoc   C++ Nsu   C++ AxiSlave │
   │ (+ Scoreboard       │            │              │      (+ Memory   │
   │  callback)          │            │              │       helper)    │
   └───────│─────────────│────────────│──────────────│──────────│───────┘
           │ DPI 3-step  │ DPI        │ DPI          │ DPI      │ DPI
           ▼             ▼            ▼              ▼          ▼
   ┌────────────────────────────────────────────────────────────────────┐
   │ axi_master   nmu_wrap    loopback_noc   nsu_wrap    axi_slave      │
   │ _wrap                    _wrap                      _wrap          │
   │     │             │              │             │            │     │
   │     │ axi_intf    │ noc_req_intf │ noc_req_intf │ axi_intf  │     │
   │     └────────────►├─────────────►├─────────────►├──────────►│     │
   │                   │ noc_rsp_intf │ noc_rsp_intf │           │     │
   │                   │◄─────────────┤◄─────────────┤           │     │
   │                                                                   │
   │  wb2axip faxi_slave  bind on master→nmu axi_intf                  │
   │  wb2axip faxi_master bind on nsu→slave axi_intf                   │
   │                                                                   │
   │  tb_top: clk_i, rst_ni, +scenario plusarg, [+inject= optional]    │
   └────────────────────────────────────────────────────────────────────┘
```

### 4.2 Per-cycle behavior (each SV wrap)

```systemverilog
always_ff @(posedge clk_i) begin
    if (!rst_ni) begin
        // drive all output wires to reset values (ni_signals_pkg constants)
    end else begin
        // 3-step DPI: read prev-cycle wires, advance c_model, register new outputs
        cmodel_nmu_set_inputs(/* axi, noc_req credit, noc_rsp */);
        cmodel_nmu_tick();
        cmodel_nmu_get_outputs(/* axi, noc_req, noc_rsp credit */);
        // register outputs (nonblocking <= ) — visible next cycle to downstream shells

        // inline error check (per Codex Section 4 (b))
        string msg;
        int err = cmodel_check_error(msg);
        if (err != 0) begin
            $display("[nmu_wrap] DPI fatal: %s", msg);
            cmodel_finalize();
            $fatal(1);
        end
    end
end
```

### 4.3 Hermetic singleton invariant (mechanical enforcement)

Each ShellAdapter owns ONE c_model component. Cross-component data ALWAYS via SV wire. Enforcement:

- Each `*_shell_adapter.hpp` in its own header; no `#include` of another shell adapter
- Each `cosim2/c/<comp>_dpi.cpp` only references its own adapter's singleton
- Build-system policy: `cosim2/c/` translation units do not link against each other
- CI lint script `tools/check_cosim2_hermetic.sh`: grep for forbidden cross-shell access patterns
- c_model standalone ctor: ShellAdapter constructs `Nmu(NmuConfig)` without other-component refs

## 5. Shell pattern + DPI surface

### 5.1 C++ ShellAdapter (template per component)

```cpp
namespace ni::cmodel::cosim2 {

class NmuShellAdapter {
  public:
    void init(const nmu::NmuConfig& cfg);
    void finalize() noexcept;
    bool initialized() const noexcept;

    void set_inputs(const NmuInputs& in);          // SV calls per cycle with prev-cycle wire values
    void tick();                                    // advance c_model one cycle
    void get_outputs(NmuOutputs& out) const;        // SV calls to read new output state

  private:
    std::unique_ptr<nmu::Nmu> nmu_;                 // hermetic — no refs to other components
    NmuInputs in_;                                  // per-cycle input latch
    NmuOutputs out_;                                // per-cycle output latch
    bool initialized_ = false;
};

}  // namespace
```

`NmuInputs` / `NmuOutputs` are POD structs grouping all AXI + NoC pins for the boundary. Defined in `c_model/include/cosim2/nmu_shell_io.hpp` for symmetry.

### 5.2 DPI signatures (per shell, batched)

```c
// cosim2/c/cmodel_dpi.h
extern "C" {
    // Lifecycle (singleton management for 5 adapters)
    void cmodel_init(const char* scenario_yaml_path);
    void cmodel_finalize(void);
    int  cmodel_check_error(const char** msg);          // 0 = OK, !=0 = fatal pending

    // 5 sets of {set, tick, get} — one set per component
    void cmodel_nmu_set_inputs(/* svBit/svBitVecVal args matching NmuInputs */);
    void cmodel_nmu_tick(void);
    void cmodel_nmu_get_outputs(/* svBit/svBitVecVal out args matching NmuOutputs */);
    // ... 4 more components same shape
}
```

Each DPI entrypoint wraps work in try/catch via `DPI_BOUNDARY_BEGIN(fn)` / `DPI_BOUNDARY_END` macro that sets `g_dpi_error_{code,msg}` on exception. NO `vl_fatal` mid-cycle — propagation via return code, SV shell raises `$fatal` at safe sync point.

### 5.3 Idempotent init (strong exception guarantee)

```cpp
extern "C" void cmodel_init(const char* path) {
    DPI_BOUNDARY_BEGIN(cmodel_init) {
        // Step 1: tear down全部舊 singleton
        g_master_adapter.reset(); g_nmu_adapter.reset();
        g_loopback_adapter.reset(); g_nsu_adapter.reset();
        g_slave_adapter.reset();
        g_dpi_error_code.store(0);
        g_dpi_error_msg.clear();

        // Step 2: parse scenario
        auto scenario = axi::parse_scenario(path);

        // Step 3: construct fresh (local unique_ptr → all-or-nothing)
        auto m = std::make_unique<MasterShellAdapter>(); m->init(scenario);
        auto n = std::make_unique<NmuShellAdapter>(); n->init(make_nmu_config());
        auto l = std::make_unique<LoopbackNocShellAdapter>(); l->init();
        auto s = std::make_unique<NsuShellAdapter>(); s->init(make_nsu_config());
        auto v = std::make_unique<SlaveShellAdapter>(); v->init(make_memory_config());

        // Step 4: commit
        g_master_adapter = std::move(m); g_nmu_adapter = std::move(n);
        g_loopback_adapter = std::move(l); g_nsu_adapter = std::move(s);
        g_slave_adapter = std::move(v);
    } DPI_BOUNDARY_END;
}
```

c_model ctors MUST be throw-safe (audit before implementation; spec-level invariant).

## 6. Wire interface contracts

### 6.1 axi_intf (cosim2-local; Phase 2 upstream → specgen)

```systemverilog
interface axi_intf #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (input logic clk_i, input logic rst_ni);
    // AW channel (master drives, slave consumes)
    logic                    awvalid, awready;
    logic [ID_WIDTH-1:0]     awid;
    logic [ADDR_WIDTH-1:0]   awaddr;
    logic [7:0]              awlen;
    logic [2:0]              awsize;
    logic [1:0]              awburst;
    logic                    awlock;
    logic [3:0]              awcache;
    logic [2:0]              awprot;
    logic [3:0]              awqos;
    // W
    logic                    wvalid, wready, wlast;
    logic [DATA_WIDTH-1:0]   wdata;
    logic [DATA_WIDTH/8-1:0] wstrb;
    // B
    logic                    bvalid, bready;
    logic [ID_WIDTH-1:0]     bid;
    logic [1:0]              bresp;
    // AR
    logic                    arvalid, arready;
    logic [ID_WIDTH-1:0]     arid;
    logic [ADDR_WIDTH-1:0]   araddr;
    logic [7:0]              arlen;
    logic [2:0]              arsize;
    logic [1:0]              arburst;
    logic                    arlock;
    logic [3:0]              arcache;
    logic [2:0]              arprot;
    logic [3:0]              arqos;
    // R
    logic                    rvalid, rready, rlast;
    logic [ID_WIDTH-1:0]     rid;
    logic [DATA_WIDTH-1:0]   rdata;
    logic [1:0]              rresp;

    modport master ( /* AW,W,AR + bready,rready out; B,R + ready in */ );
    modport slave  ( /* AW,W,AR + bready,rready in; B,R + ready out */ );
endinterface
```

`region` / `user` fields intentionally omitted from PoC (not checked by wb2axip; KNOWN_LIMITATIONS).

### 6.2 noc_req_intf / noc_rsp_intf (parameterized)

```systemverilog
interface noc_req_intf #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 256
) (input logic clk_i, input logic rst_ni);
    logic                   valid;
    logic [FLIT_W-1:0]      flit;
    logic [NUM_VC-1:0]      credit_return;
    modport producer ( output valid, flit; input credit_return );
    modport consumer ( input valid, flit; output credit_return );
endinterface
```

`noc_rsp_intf` symmetric (rsp direction). `NUM_VC` flows from top-level `localparam` to all interface instances + shell parameter ports.

### 6.3 AW handshake timing (fully-registered β)

```
Cycle:      T-1   T    T+1   T+2
master:
  out.awvalid  1   1    1    0       (drives at T-2, reg visible from T-1)
nmu_wrap:
  in.awvalid   .   1    1    1       (reads prev-cycle reg)
  cap OK?      y   y    .    .       (queried at tick end)
  consume?     .   .    Y    .       (closes when valid && my_prev_ready)
  out.awready  1   1    0    0       (cap available T-1; full now)
master:
  in.awready   1   1    1    0       (reads nmu's prev-cycle reg)
  handshake closed at T+1            (master sees both valid && ready high)
```

**Invariant**: handshake closes iff master's `*valid_q` AND slave's `*ready_q` are both 1 in the same SV cycle (both are registered values). Reset cycle cannot close (both start 0). Back-to-back beats consume ≥ 2 cycles each.

### 6.4 ShellAdapter backpressure rule (mandatory ordering)

```cpp
void NmuShellAdapter::tick() {
    // 1. Sample inputs (already latched by set_inputs() this tick)
    bool aw_valid_now = in_.aw_valid;
    bool can_take = nmu_->axi_slave_port().can_accept_aw();   // tick-end capacity query

    // 2. Push iff valid + capacity (NEVER push without checking capacity first)
    if (aw_valid_now && can_take) {
        nmu_->axi_slave_port().push_aw(make_beat(in_));
    }

    // ... same pattern for W / AR

    // 3. Advance c_model
    nmu_->tick();

    // 4. ALWAYS update output latches at tick end
    out_.aw_ready = nmu_->axi_slave_port().can_accept_aw();   // next-cycle capacity
    out_.b_valid  = nmu_->axi_slave_port().has_b_response();
    // ...
}
```

Forbidden: push before checking capacity, push then drop on overflow, query capacity mid-tick more than once.

## 7. Verification chain

### 7.1 Three layers

| Layer | Mechanism | Independent source |
|---|---|---|
| 1 — protocol legality | wb2axip SVA bound on AXI bundles | ZipCPU reading of IHI 0022 |
| 2 — data integrity | C++ Scoreboard via AxiMaster callbacks | Own oracle |
| 3 — cross-validation | (a) vs (b) beat sequence diff | (a) and (b) share testbench heritage but differ in architecture |

Layers 1+2 ship gate. Layer 3 marked follow-up.

### 7.2 wb2axip bind + parametric override

```systemverilog
localparam int BURST_LEN_MAX  = 256;
localparam int NUM_HOPS_FWD   = 4;
localparam int NUM_HOPS_BACK  = 4;
localparam int MEM_LATENCY_MAX = 16;
// [UNVERIFIED] — implementer MUST verify wb2axip property semantic
// before pinning. See cosim2/sv/wb2axip/{faxi_master,faxi_slave}.v sources.
localparam int F_AXI_MAXSTALL_VAL  = NUM_HOPS_FWD * 2;
localparam int F_AXI_MAXRSTALL_VAL = NUM_HOPS_BACK * 2;
localparam int F_AXI_MAXDELAY_VAL  = BURST_LEN_MAX + NUM_HOPS_FWD + NUM_HOPS_BACK + MEM_LATENCY_MAX;

faxi_slave #(.F_AXI_MAXSTALL(F_AXI_MAXSTALL_VAL), .F_AXI_MAXDELAY(F_AXI_MAXDELAY_VAL), ...)
    u_nmu_check (...);
```

### 7.3 Scoreboard placement + timing caveat

Scoreboard hooked on `AxiMaster::on_write_completed` / `on_read_observed` callbacks, fires inside C++ tick. Reports c-model-completion semantic — **1+ cycle earlier** than SV-wire-observable BREADY/BVALID closing under β. Documented; no SV-side wire tracker needed for PoC.

### 7.4 Fault injection (plusarg contract)

```
Production: ./Vtb_top +scenario=fixtures/conformity_write_read.yaml
Injection:  ./Vtb_top +scenario=fixtures/injection_aw_unstable.yaml +inject=aw_unstable@cycle=10
```

Plusarg parser at init:
- No `+inject=` → no overhead, no injection
- `+inject=<mode>@cycle=<N>` → parse, validate `<mode>` against allowlist (`aw_unstable`, future modes)
- Unknown mode → `$fatal(1, "unknown +inject mode: %s", mode)`
- Active mode echoed to `$display` for traceability

Implementation: `c_model/include/axi/axi_master.hpp` adds plusarg read + `if (inject_mode == AW_UNSTABLE && cycle == inject_cycle) force_awvalid_low()`. Production binary has same code; not gated by `#ifdef`. Zero-overhead when plusarg absent.

### 7.5 DPI error propagation

```cpp
extern "C" int cmodel_check_error(const char** msg) {
    DPI_BOUNDARY_BEGIN(cmodel_check_error) {
        *msg = g_dpi_error_msg.c_str();
        return g_dpi_error_code.load();
    } DPI_BOUNDARY_END;
    return 1;  // boundary-throw fallback
}
```

Each SV shell `always_ff` calls `cmodel_check_error` inline at end of cycle; if non-zero → `cmodel_finalize()` + `$fatal(1)`. Localizes error to specific shell for debug.

## 8. File layout

```
cosim2/                                  # Stage 5b root (rename → cosim/ after stable)
├── README.md, KNOWN_LIMITATIONS.md, CODING_DISCIPLINE.md
├── sv/
│   ├── axi_intf.sv                      # AXI + handshake + master/slave modports
│   ├── noc_req_intf.sv                  # parameter NUM_VC; [NUM_VC-1:0] credit
│   ├── noc_rsp_intf.sv                  # symmetric
│   ├── axi_master_wrap.sv               # 5 shells, rtl-style conformant
│   ├── nmu_wrap.sv
│   ├── loopback_noc_wrap.sv
│   ├── nsu_wrap.sv
│   ├── axi_slave_wrap.sv
│   ├── tb_top.sv                        # top: 5 instances + 6 bundles + wb2axip bind
│   └── wb2axip/                         # carried from cosim/sv/wb2axip/
│       ├── faxi_master.v, faxi_slave.v, faxi_wstrb.v, sim_wrapper.svh
│       └── ATTRIBUTION.md (updated path)
├── c/
│   ├── cmodel_dpi.h, cmodel_dpi.cpp     # DPI lifecycle + 5×(set/tick/get)
│   └── dpi_boundary_macros.h            # DPI_BOUNDARY_BEGIN/END for try/catch dedup
├── verilator/
│   ├── Makefile                         # adapted from cosim/verilator/
│   └── main.cpp                         # drives clk_i, rst_ni, eval loop
└── tests/
    ├── CMakeLists.txt
    ├── fixtures/                        # 5 YAML scenarios
    └── test_cosim_wire_smoke.cpp + test_checker_fires_on_violation.cpp

c_model/include/cosim2/
├── master_shell_adapter.hpp, master_shell_io.hpp
├── nmu_shell_adapter.hpp, nmu_shell_io.hpp
├── loopback_noc_shell_adapter.hpp, loopback_noc_shell_io.hpp
├── nsu_shell_adapter.hpp, nsu_shell_io.hpp
└── slave_shell_adapter.hpp, slave_shell_io.hpp

c_model/include/{nmu,nsu,axi,common}/    # additive changes only
├── nmu/axi_slave_port.hpp               # + can_accept_aw/w/ar() const
├── nsu/axi_master_port.hpp              # + can_accept_b/r() const
├── axi/axi_master.hpp                   # + standalone ctor + plusarg inject mode
├── axi/axi_slave.hpp                    # + standalone ctor
├── nmu/nmu.hpp                          # + standalone ctor (NmuConfig only)
├── nsu/nsu.hpp                          # + standalone ctor
└── common/loopback_noc.hpp              # + standalone ctor

# CODING_DISCIPLINE.md content
- All cosim2/sv/*.sv MUST conform to rtl-style skill
- Subagents writing/modifying SV invoke rtl-style skill first
- Writers invoke karpathy-guidelines skill for code quality discipline
- Reviewers dispatch rtl-reviewer agent + Codex review
- Hermetic singleton invariant enforced via build/CI/grep
- Shells contain ONLY wire↔method conversion + handshake; no c_model logic
```

(a) artifacts to **delete in single dedicated commit** (top of branch):
`cosim/sv/{nmu_cmodel_proxy,nsu_cmodel_proxy,tb_axi_conformity}.sv`,
`cosim/c/{axi_dpi.cpp,axi_dpi.h}`,
`c_model/include/cosim/{axi_dpi_adapter.hpp,pin_snapshot.hpp}`,
`c_model/tests/cosim/`.

Carry to `cosim2/sv/wb2axip/`: vendored files + ATTRIBUTION + sim_wrapper + faxi_wstrb (unchanged).

## 9. Test plan

### 9.1 Smoke set (5 scenarios, 1 binary, simplified per `feedback-test-meaningfulness-over-count`)

| # | Scenario | Distinct property |
|---|---|---|
| 1 | `conformity_write_read.yaml` | Basic plumbing (single beat, single outstanding) |
| 2 | `multibeat_incr_8beat.yaml` | Multi W beat visibility — KNOWN_LIMITATIONS §2 unlock |
| 3 | `multioutstanding_aw_stress.yaml` | Multi-outstanding + ROB enabled mode wire-level |
| 4 | `conformity_backpressure.yaml` | Stall/backpressure handshake |
| 5 | `injection_aw_unstable.yaml` | wb2axip checker liveness regression |

### 9.2 Acceptance criteria

- 4 conformity / multibeat scenarios PASS (production mode, no plusarg)
- 1 injection scenario produces non-zero exit code (binary fail = test pass), `$display` confirms wb2axip property name
- Existing Stage 5a `test_axi_dpi_adapter` (2 cases) unchanged (parallel infra)
- 4 drift gates clean (specgen pytest 163, codegen --check, gen_inventory --check, c_model ctest)
- `KNOWN_LIMITATIONS.md` §2 marked RESOLVED with evidence (multibeat passing log)
- `rtl-reviewer` agent reports 0 CRITICAL/HIGH findings on shells

**ctest expected**: 395 (a baseline) + 5 (new) = **400/400**

### 9.3 Implementation prerequisites (must resolve before writing tests)

1. wb2axip `F_AXI_MAXSTALL` semantic verified — implementer dispatches sanity-check subagent that reads `cosim2/sv/wb2axip/faxi_*.v` and confirms per-response vs per-channel stall semantic. Updates parametric formula if needed. Documented in spec amendment before `conformity_backpressure.yaml` task starts.
2. c_model component ctors audited throw-safe. List of any non-throw-safe ctors documented; either refactored to RAII or excluded from strong-init guarantee.
3. `can_accept_*()` query API added to ports (small additive change; verified by re-running Stage 5a `test_axi_dpi_adapter`).

### 9.4 Performance budget

- β tick adds ~1 cycle per hop → estimated 5x cycle count vs Stage 5a
- `conformity_*.yaml` ≈ 150 cycles each, <1 sec wall time
- `multibeat_incr_8beat.yaml` ≈ 250 cycles
- `multioutstanding_aw_stress.yaml` ≈ 600 cycles
- 5 scenarios total < 30 sec ctest wall time

### 9.5 CI / drift gates

GH Actions doesn't install Verilator (same as Stage 5a). cosim2 ctest local-only. CI runs specgen pytest + codegen / inventory checks + c_model ctest (excluding cosim2 entries).

## 10. Karpathy 4-lens

**Overcomplication?** 25 new files lookbig but 5 shells are cookie-cutter (rtl-style template instantiated 5×). Alternatives rejected: extending (a) snapshot (treats symptom not cause); ComponentHandle SV virtual class (over-engineer for single backend; Verilator SV OOP support uncertain). Risk: shells creeping non-conversion logic. Mitigation: CODING_DISCIPLINE.md explicit rule + per-task review.

**Surgical?** All c_model changes additive (overloaded ctors, `can_accept_*()` const queries, plusarg-driven inject — no signature changes). (a) infrastructure untouched. specgen unmodified Phase 1. New files all in cosim2/. ctest 395 baseline preserved. Risk: implementer drift into existing c_model internal logic. Mitigation: per-task scope clause in implementation plan + spec-compliance reviewer enforces `git diff` boundary.

**Surface assumptions** (with verification + fallback):

| Assumption | Verify | Fallback |
|---|---|---|
| c_model components standalone-constructible (hermetic) | Per-component unit test after ctor overload added | Bind-via-`bind_downstream(...)` post-ctor if WormholeArbiter requires |
| 75 DPI calls/cycle Verilator perf OK on Windows | Smoke benchmark in implementation Task 0 | Reduce to fewer batched DPI per shell |
| Multi-beat W truly wire-visible under β | `multibeat_incr_8beat.yaml` PASS | Bug in ShellAdapter handshake detect — rewrite |
| wb2axip MAXSTALL parametric formula | Implementer sanity-check (gated prerequisite) | Update formula per discovered semantic |
| rtl-style + karpathy-guidelines produce Verilator-clean SV | rtl-reviewer + Codex review per task; Verilator compile | Rule conflict → user adjudicates, rtl-style authoritative |
| Plusarg injection doesn't contaminate production | Production scenarios don't include `+inject=`; parser fails on unknown | grep production fixtures for plusarg in CI |

**Verifiable success**:

Hard gate (ship): 4 conformity/multibeat PASS + 1 fault injection PASS (binary fails as expected) + (a) ctest 395 unchanged + 4 drift gates clean + KNOWN_LIMITATIONS §2 marked RESOLVED.

Soft signal: rtl-reviewer 0 CRITICAL/HIGH; Codex per-task reviews approved; implementation commits all <500 lines net new (excluding vendored).

## 11. Open items & Phase 2 follow-ups

- Layer 3 cross-comparison tool ((a) vs (b) beat sequence diff)
- specgen handshake field upstream + regenerate `ni_signals_pkg.sv`; cosim2 switches from local interfaces to regenerated specgen output
- Stress scenarios: 256-beat, 4KB cross, multi-VC (NUM_VC > 1)
- `cosim2/` → `cosim/` rename + delete (a) artifacts (after 5b stable)
- VCS DPI-RTL backend port (use same SV side; replace Verilator main.cpp with DPI bridge)
- ComponentHandle SV virtual class abstraction (only if multiple backends per component become needed)
- Performance counter wire visibility

## 12. References

- Stage 5a spec: `docs/superpowers/specs/2026-06-04-stage5-axi-checker-cosim-design.md`
- Stage 5a plan: `docs/superpowers/plans/2026-06-04-stage5-axi-checker-cosim.md`
- Main plan: `docs/noc_cmodel_rtl_plan.md` §5 (mixed co-sim vision)
- Stage 5a KNOWN_LIMITATIONS: `cosim/KNOWN_LIMITATIONS.md` (carried + updated to `cosim2/`)
- wb2axip vendor: https://github.com/ZipCPU/wb2axip @ commit `2e8d3bc2` (Apache 2.0)
- rtl-forge skill: https://github.com/changlukas/rtl-forge (rtl-style + rtl-reviewer)
- karpathy-guidelines skill (installed via plugin)
- AXI4 IHI 0022 (rule reference for wb2axip property IDs)
