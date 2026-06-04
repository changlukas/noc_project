# Nmu / Nsu top-level assembly — design

**Status**: design (Codex round-1 NEEDS-REVISION fixes folded; ready for spec review + writing-plans)
**Date**: 2026-06-04
**Branch**: `stage3/packetize-depacketize` (commits accumulate)
**Prior rounds shipped on this branch**: ROB, addr_trans, AxiSlavePort, AxiMasterPort, Packetize (NMU+NSU multi-output), Depacketize (NMU+NSU), MetaBuffer, VcArbiter (NMU+NSU, Mode A/B with NUM_VC parameterized), WormholeArbiter (template, NMU 3-input pairing + NSU 2-input no-pairing), header.last FlooNoC alignment, .clang-format Google-base + 4-space indent

## 1. Motivation

Stage 3 main plan §3 (rows 168 + 173) requires `nmu/nmu.hpp` + `nsu/nsu.hpp` top-level files that "組裝" sub-modules. Currently integration testbench (`test_request_response_loopback.cpp`) manually wires 6-7 sub-modules per side + arranges tick order in a 5-line per-cycle loop. After this round, that becomes 1 `nmu::Nmu` + N `nsu::Nsu` instances each exposing single `tick()`.

This closes Stage 3 NI architecture; next round (NUM_VC>1 e2e stress, per user direction) then session pivots to Stage 5 axi_checker.sv co-sim.

## 2. Scope

**In scope**:
- `c_model/include/nmu/nmu.hpp` (new) — `nmu::Nmu` class + `NmuConfig` struct
- `c_model/include/nsu/nsu.hpp` (new) — `nsu::Nsu` class + `NsuConfig` struct
- `c_model/tests/integration/test_request_response_loopback.cpp` — refactor to use Nmu/Nsu

**Out of scope** (deferred):
- Pin-struct conversion (Stage 5 axi_checker round)
- New Nmu/Nsu dedicated unit tests (integration testbench = e2e validation)
- Integration testbench NUM_VC>1 e2e stress (next follow-up round per user direction; unit tests already cover NUM_VC ∈ {1,2,4,8})

## 3. Anchored decisions

| Decision | Choice | Source |
|---|---|---|
| Ctor style | Single `NmuConfig` / `NsuConfig` struct + external pin refs | User Q1 |
| Lifetime | Stack members; delete move/copy (WormholeArbiter is non-movable) | Established pattern |
| Tick orchestration | Single `tick()` entry per class, internal sub-module call order | Design proposal |
| Pin interface | C++ ref-based (defer pin-struct to Stage 5) | Scope decision |
| AddressTrans | NOT a stack member — it's `nmu::addr_trans::xy_route()` namespace helper (Codex B1) | Codex fix |
| AXI ctor binding | NOT `axi::AxiMaster&` (type collision with `AxiMasterT<AxiSlavePort>` testbench instantiation); expose facade via getter `axi_slave_port()` returning `AxiSlavePort&` (Codex B2) | Codex fix |

## 4. Architecture

### 4.1 NMU pipeline (already in production; just encapsulated)

```
AxiMaster (testbench-side, holds AxiSlavePort& via template)
   │
   ▼ AXI 5-channel (AW/W/AR push + B/R pop)
┌────────────────────────────────────────┐
│ Nmu                                    │
│ ┌──────────────────┐                   │
│ │ AxiSlavePort     │── B/R from depkt  │
│ │  (port_params)   │── AW/AR/W to Rob  │
│ └──────────────────┘                   │
│         │                              │
│         ▼ Rob (read_mode + write_mode) │
│   ┌──────────┐                         │
│   │  Rob     │── push to Packetize     │
│   └──────────┘                         │
│         │                              │
│         ▼ Packetize (src_id, w_meta_fifo)
│   ┌──────────┐                         │
│   │ Packetize│── aw_out / w_out / ar_out
│   └──────────┘                         │
│         │                              │
│         ▼ WormholeArbiter<NocReqOut>   │
│   ┌──────────────────┐ (3 in, {{0,1}}) │
│   │ WormholeArbiter  │                 │
│   └──────────────────┘                 │
│         │                              │
│         ▼ VcArbiter (Mode A/B, NUM_VC) │
│   ┌──────────┐                         │
│   │ VcArbiter│── single NocReqOut output
│   └──────────┘                         │
│         │                              │
│         │                              │
│   Depacketize (rsp side) ── pop NocRspIn
│         │                              │
│         ▼ B/R back to AxiSlavePort     │
└────────────────────────────────────────┘
   │
   ▼ NocReqOut / NocRspIn external refs (LoopbackNoc adapter or DPI bridge)
```

### 4.2 NSU pipeline (mirror, asymmetric)

```
LoopbackNoc / NoC
   │
   ▼ NocReqIn / NocRspOut external refs
┌────────────────────────────────────────┐
│ Nsu                                    │
│         │                              │
│   Depacketize (req side, depkt depths) │
│   ┌──────────┐ uses MetaBuffer for     │
│   │ Depacketize  meta snapshot         │
│   └──────────┘                         │
│         │                              │
│         ▼ AxiMasterPort                │
│   ┌──────────┐                         │
│   │ AxiMaster─── AW/W/AR drive externally
│   │  Port    │   B/R from external slave
│   └──────────┘                         │
│         │ (response path)              │
│         ▼ Packetize (b_out / r_out)    │
│   ┌──────────┐                         │
│   │ Packetize│                         │
│   └──────────┘                         │
│         │                              │
│         ▼ WormholeArbiter<NocRspOut>   │
│   ┌──────────────────┐ (2 in, no pair) │
│   │ WormholeArbiter  │                 │
│   └──────────────────┘                 │
│         │                              │
│         ▼ VcArbiter                    │
│   ┌──────────┐                         │
│   │ VcArbiter│── single NocRspOut output
│   └──────────┘                         │
└────────────────────────────────────────┘
```

Key asymmetries vs NMU:
- No Rob (response side doesn't reorder; ROB is NMU's responsibility for matching IDs)
- No `addr_trans` (NSU uses `dst_id` from incoming flit header, not address-derived)
- Depacketize sits at front (receiving req flits); Packetize at back (sending rsp flits)
- WormholeArbiter has 2 inputs no pairing (B/R each single-flit per FlooNoC pattern)
- MetaBuffer between Depacketize and Packetize for rob_idx/src_id snapshot

## 5. Class shape

### 5.1 `nmu::Nmu`

```cpp
namespace ni::cmodel::nmu {

struct NmuConfig {
    uint8_t src_id;
    RobMode read_rob_mode = RobMode::Disabled;
    RobMode write_rob_mode = RobMode::Disabled;
    PortParams port_params{};                    // AxiSlavePort config (loaded from yaml elsewhere)
    std::size_t depkt_aw_q_depth = 16;           // Depacketize per-channel depths
    std::size_t depkt_w_q_depth = 16;
    std::size_t depkt_ar_q_depth = 16;
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_vc = 0;
    uint8_t read_vc = 0;
    // Mode B candidate arrays; ignored when vc_mode == ReadWriteSplit
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
};

class Nmu {
  public:
    Nmu(NmuConfig cfg,
        noc::NocReqOut& downstream_req,
        noc::NocRspIn&  downstream_rsp);

    Nmu(const Nmu&) = delete;
    Nmu(Nmu&&) = delete;

    // AXI facade — testbench's AxiMaster<AxiSlavePort> binds to this getter
    AxiSlavePort& axi_slave_port() { return axi_slave_port_; }

    // Per-cycle tick — orchestrates all sub-modules in correct order
    void tick();

    // Test introspection (optional; expose only when needed)
    const Rob& rob() const { return rob_; }
    const VcArbiter& vc_arbiter() const { return vc_arbiter_; }

  private:
    // Declaration order matters for ctor sequence:
    //   downstream refs first → VcArbiter → WormholeArbiter (wraps VcArbiter)
    //   → Depacketize (rsp side; takes downstream_rsp_) → Packetize (req side; takes wormhole inputs)
    //   → Rob (takes Packetize + Depacketize) → AxiSlavePort (takes Rob)
    NmuConfig                                cfg_;
    noc::NocReqOut&                          downstream_req_;
    noc::NocRspIn&                           downstream_rsp_;
    VcArbiter                                vc_arbiter_;       // wraps downstream_req_
    noc::WormholeArbiter<noc::NocReqOut>     wormhole_arbiter_; // wraps vc_arbiter_
    Depacketize                              depacketize_;      // wraps downstream_rsp_
    Packetize                                packetize_;        // wraps wormhole_arbiter_.input(0/1/2)
    Rob                                      rob_;              // wraps packetize_ + depacketize_
    AxiSlavePort                             axi_slave_port_;   // wraps rob_ (as Packetizer+Depacketizer)
};

}  // namespace
```

### 5.2 `nsu::Nsu`

```cpp
namespace ni::cmodel::nsu {

struct NsuConfig {
    uint8_t src_id;
    PortParams port_params{};                    // AxiMasterPort config
    std::size_t meta_buffer_per_id_depth = 16;
    std::size_t depkt_aw_q_depth = 16;
    std::size_t depkt_w_q_depth = 16;
    std::size_t depkt_ar_q_depth = 16;
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_rsp_vc = 0;   // B → write_rsp_vc
    uint8_t read_rsp_vc = 0;    // R → read_rsp_vc
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
};

class Nsu {
  public:
    Nsu(NsuConfig cfg,
        noc::NocReqIn&  upstream_req,
        noc::NocRspOut& downstream_rsp);

    Nsu(const Nsu&) = delete;
    Nsu(Nsu&&) = delete;

    // AXI facade — testbench's AxiSlave-side wires bind through this
    AxiMasterPort& axi_master_port() { return axi_master_port_; }

    void tick();

  private:
    // Declaration order:
    //   downstream_rsp first → VcArbiter → WormholeArbiter (wraps VcArbiter)
    //   → MetaBuffer (no upstream dep) → Packetize (takes wormhole inputs + meta)
    //   → Depacketize (takes upstream_req + meta) → AxiMasterPort (takes Depacketize + Packetize)
    NsuConfig                                cfg_;
    noc::NocReqIn&                           upstream_req_;
    noc::NocRspOut&                          downstream_rsp_;
    VcArbiter                                vc_arbiter_;
    noc::WormholeArbiter<noc::NocRspOut>     wormhole_arbiter_;
    MetaBuffer                               meta_buffer_;
    Packetize                                packetize_;
    Depacketize                              depacketize_;
    AxiMasterPort                            axi_master_port_;
};

}  // namespace
```

## 6. Tick orchestration

Per-cycle order (matches current testbench loop at `test_request_response_loopback.cpp:311-418`):

```cpp
// nmu::Nmu::tick():
void Nmu::tick() {
    depacketize_.tick();        // pop NocRspIn flits → emit B/R beats to AxiSlavePort
    axi_slave_port_.tick();     // handshake state machine
    wormhole_arbiter_.tick();   // drain pending → vc_arbiter input
    vc_arbiter_.tick();         // drain pending → downstream_req_
}

// nsu::Nsu::tick():
void Nsu::tick() {
    depacketize_.tick();        // pop NocReqIn flits → emit AW/W/AR to AxiMasterPort
    axi_master_port_.tick();    // handshake state machine
    wormhole_arbiter_.tick();
    vc_arbiter_.tick();
}
```

Upstream-first convention from vc_arb/wormhole_arbiter rounds preserved. Testbench loop simplifies to:

```cpp
while (!master.done()) {
    master.tick();
    nmu.tick();
    for (auto& nsu : nsus) nsu->tick();
    slave.tick(); mem.tick();   // axi endpoint side
    // shuttle slave ↔ ports (existing logic in testbench)
    loopback.tick();
}
```

## 7. Integration testbench refactor

`test_request_response_loopback.cpp`: ~400 lines wiring → ~80 lines.

**Cannot be absorbed into Nmu/Nsu** (stays in testbench per Codex I5):
- LoopbackNoc `set_dst_route(dst_id, nsu_idx)` multi-NSU routing
- Per-NSU latency config `set_nsu_latency(...)`
- Per-NSU shuttle logic (slave ↔ AxiMasterPort: routing B back to owning NSU, R order tracking)

These belong in testbench fabric setup, not NI internals.

**Absorbed into Nmu/Nsu**:
- All 7 NMU sub-module construction + cross-wiring (declaration order in 5.1)
- All 6 NSU sub-module construction + cross-wiring (declaration order in 5.2)
- Per-cycle tick orchestration of NI sub-modules

## 8. Commit boundary (3 commits)

| # | Subject | ctest |
|---|---|---|
| 1 | `feat(nmu): add Nmu top-level class encapsulating sub-modules` | 336/336 unchanged (smoke test only; existing integration not yet wired) |
| 2 | `feat(nsu): add Nsu top-level class (NSU mirror, no Rob)` | 336/336 unchanged |
| 3 | `refactor(tests/integration): use Nmu/Nsu in PacketizeLoopback testbench` | 336/336 unchanged (e2e validation: integration tests still pass) |

Commit 1+2 are pure additions (new header files + minimal smoke test ensuring class compiles + ticks without crash). Commit 3 is the testbench refactor — same e2e behavior, ~320 fewer lines.

## 9. Out of scope (deferred follow-ups)

- **Next follow-up round** (per user direction): Integration testbench NUM_VC>1 e2e stress. unit tests already cover NUM_VC ∈ {1,2,4,8} via TEST_P; this would extend integration testbench to run multi-NSU + multi-VC traffic through wormhole+vc pipeline.
- **Stage 5 (new session)**: axi_checker.sv co-sim to verify c_model NMU/NSU produce AXI4-conformant traffic at the boundary.
- Pin-struct conversion (Stage 5 prereq depending on axi_checker interface)
- Nmu/Nsu dedicated unit tests (e2e via integration is sufficient at this encapsulation scope)

## 10. Naming convention

- Style: `.clang-format` enforced (Google base + 4-space indent + 4-space continuation + 100col)
- Types: PascalCase (`Nmu`, `Nsu`, `NmuConfig`, `NsuConfig`)
- Methods + fields: snake_case (`axi_slave_port_`, `wormhole_arbiter_`, `tick`)
- Constants: `kCamelCase` for project conventions established in prior rounds (e.g., `kDefaultPendingDepth`)
- File names: snake_case (`nmu.hpp`, `nsu.hpp`)
