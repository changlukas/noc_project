# addr_trans + ROB Disabled + AXI4 Conformity — Design Spec

**Status**: Draft, brainstormed 2026-06-02 (Codex review rounds 1-8 all PROCEED), awaiting user review
**Stage**: Stage 3 (post NMU/NSU packetize+depacketize; before ROB Enabled mode + vc_arb + vc_mapping)
**Anchor docs**: `docs/noc_cmodel_rtl_plan.md` §3 + §3.1, NMU image `docs/image/nmu.jpg`, NSU image `docs/image/nsu.jpg`
**Prior spec (this branch)**: `docs/superpowers/specs/2026-06-02-nmu-nsu-packetize-depacketize-design.md`

---

## 1. Goal

Three coupled deliverables this round:

1. **Stage 2 AxiMaster AXI4 conformity fix** — relax the conservative same-AXI-ID concurrent admission block (which violated AXI4 IHI 0022 §A5.3) and switch outstanding-op tracking to per-id FIFO. After fix, master can issue multiple outstanding same-ID transactions per spec; ordering becomes the NMU's job (ROB).

2. **`nmu/addr_trans`** — pure helper for XYRouting (awaddr/araddr → dst_id + local_addr). Replaces the TestPacketize sticky-setter hack from the prior round; Packetize self-computes routing internally via this helper.

3. **`nmu/rob`** — per-AXI-ID Disabled mode (transaction ordering filter): block same-id different-dst until in-flight to current dst completes. Forward-compat to Enabled mode (per-id reorder buffer, next round). Sits as in-line layer between AxiSlavePort and Packetize/Depacketize.

**Success criterion**:
- All Stage 2 baseline tests still pass after AxiMaster relaxation (impact-localized updates allowed)
- New unit tests (addr_trans, rob, refactored packetize, protocol helpers): all pass
- Integration loopback test: existing 6 fixtures no-regression + 1 new `multi_dst_stress.yaml` (now viable post AxiMaster fix) hits ROB Disabled stall path end-to-end, scoreboard zero mismatch
- All drift gates clean

---

## 2. Scope

### In-scope

| Concern | Files |
|---|---|
| Stage 2 AxiMaster AXI4 conformity fix | `c_model/include/axi/axi_master.hpp` (modify), `c_model/include/axi/protocol_rules.hpp` (modify, +2 template helpers) |
| addr_trans helper namespace | `c_model/include/nmu/addr_trans.hpp` (new) |
| ROB Disabled mode + forward-compat for Enabled | `c_model/include/nmu/rob.hpp` (new) |
| Packetize refactor (drop sticky setter, +push_*_with_meta) | `c_model/include/nmu/packetize.hpp` (modify) |
| TestPacketize retire | `c_model/tests/common/test_packetize_adapter.hpp` (delete) |
| Integration test wire ROB into pipeline + add multi-dst fixture | `c_model/tests/integration/test_request_response_loopback.cpp` (modify), `c_model/tests/axi/fixtures/multi_dst_stress.yaml` (new) |
| Unit tests | `c_model/tests/nmu/test_addr_trans.cpp` (new), `c_model/tests/nmu/test_rob.cpp` (new), `c_model/tests/nmu/test_packetize.cpp` (modify), `c_model/tests/axi/test_protocol_rules.cpp` (modify), `c_model/tests/axi/test_axi_master.cpp` (modify) |

### Out-of-scope (deferred)

| Concern | Owner / Future round |
|---|---|
| ROB Enabled mode (per-id reorder buffer + write metadata FIFO + rob_idx allocator) | Next round (per plan §3.1) |
| `nmu::VcMapping` / `nmu::VcArb` / `nsu::VcArb` (NoC pacing + per-VC credit) | Future round |
| `addr_trans` SAM table (IDRouting), SourceRouting | Future task |
| `addr_trans` address remap (local_addr ≠ addr) | Future task |
| `route_par` / `flit_ecc` algorithm helpers | Orthogonal, anytime |
| `nmu.hpp` / `nsu.hpp` top-level assembly | After all sub-components done |
| LoopbackNoc per-dst latency variation (needed for Enabled mode out-of-order generation) | Next round (paired with ROB Enabled) |

---

## 3. Architecture

### 3.1 Pipeline overview (post this round)

```
AXI request out (NMU side):
  AxiMaster → AxiSlavePort → Rob (as Packetizer) → nmu::Packetize → NoC
                                                                 ↓ addr_trans (internal helper call)

AXI response in (NMU side):
  AxiMaster ← AxiSlavePort ← Rob (as Depacketizer) ← nmu::Depacketize ← NoC
```

Rob inherits both `Packetizer` and `Depacketizer` (multi-inheritance; no same-name virtual collision since `Packetizer` has push_* and `Depacketizer` has pop_*). Same Rob instance passed to AxiSlavePort's frozen ctor `AxiSlavePort(Packetizer&, Depacketizer&, PortParams)` for both slots.

### 3.2 Responsibility map

| Module | Responsibility | Explicit non-responsibilities |
|---|---|---|
| `axi::AxiMasterT` (relaxed) | Submit AXI transactions; same-id multi-outstanding allowed per AXI4 spec; per-id FIFO completion ordering | Per-id reorder, dst routing |
| `axi::protocol_rules` (extended) | Static template helpers `check_b_front_can_accept_response`, `check_r_front_can_accept_beat` validating per-id FIFO invariants at AxiMaster B/R receipt | Modify AxiMaster internals |
| `nmu::addr_trans::xy_route(addr)` | Stateless pure function returning `{dst_id, local_addr}` from address | SAM lookup, remap (deferred); stateful cache |
| `nmu::Packetize` (refactored) | Internally call addr_trans for dst; bit-pack flit; expose both frozen `Packetizer` interface (with default meta) AND `push_*_with_meta` (called by Rob with full metadata including future rob_idx) | Sticky setter API (removed); ordering enforcement |
| `nmu::Rob` | Per-id ordering filter (Disabled mode): stall same-id different-dst; track per-id outstanding via deque; observe B/R completions; W burst credit gates W beats so they don't sneak past stalled AW | Reorder responses (Enabled mode, deferred); flit bit-packing |
| `TestPacketize` | Removed | — |

### 3.3 Why this composition

- **Image alignment**: NMU image shows Address Map as a sub-routine of Packetizing (same column) — addr_trans as helper called inside Packetize matches the image semantics.
- **plan §3.1 alignment**: rob in request path "after addr_trans" — the Rob class invokes addr_trans (via the same helper Packetize uses) to know dst for gating.
- **AXI4 conformity**: master being permissive on same-id concurrent matches IHI 0022 §A5.3; ordering responsibility moves to NMU (which is where the NoC reorder potential exists). FlooNoC convention matches this.
- **Forward-compat to Enabled mode**: per-id deque entry struct extends with `rob_idx` field; same multi-inheritance layout; only push_aw/push_ar/pop_b/pop_r body changes.

---

## 4. Module specifications

### 4.1 `axi::AxiMasterT` relaxation (Stage 2 modify)

**Removed restrictions**:
- Drop `if (active_write_ops_.count(txn.id)) break;` and `if (active_read_ops_.count(txn.id)) break;` from admission code

**Type changes**:
- `std::map<uint8_t, OperationContext> active_write_ops_` → `std::map<uint8_t, std::deque<OperationContext>> active_write_ops_`
- Same for `active_read_ops_`
- Add new counters: `std::size_t active_write_count_ = 0;` and `std::size_t active_read_count_ = 0;` (`map.size()` no longer counts total ops because of per-id grouping)

**Admission code** (post change):
```cpp
while (next_txn_idx_ < sc_.transactions.size()) {
    const auto& txn = sc_.transactions[next_txn_idx_];
    if (txn.op == ScenarioTransaction::Op::Write) {
        if (active_write_count_ >= max_out_w_) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        op.data = load_write_data_(txn.data_file, ...);
        op.strb_per_beat = load_strb_file_(txn.strb_file, ...);
        active_write_ops_[txn.id].push_back(std::move(op));
        active_write_count_++;
    } else {
        if (active_read_count_ >= max_out_r_) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        active_read_ops_[txn.id].push_back(std::move(op));
        active_read_count_++;
    }
    ++next_txn_idx_;
}
```

**AW/W push order preservation**: must walk per-id FIFO in submission order; break on first op whose write-request phase isn't complete (else op2's AW could race ahead of op1's W backpressure, violating AXI4 W stream ordering). `push_writes_` changes signature:
```cpp
// Returns true if op's write request phase (AW + all W beats) fully pushed downstream.
bool push_writes_(uint8_t id, OperationContext& op);

// Outer loop:
for (auto& [id, deq] : active_write_ops_) {
    for (auto& op : deq) {
        if (!push_writes_(id, op)) break;  // same-id FIFO: don't skip ahead
    }
}
```

Same pattern for `push_reads_`.

**B response handling** (per-id FIFO front routing):
```cpp
while (auto b = slave_.pop_b()) {
    AXI_PROTOCOL_ASSERT(rules::check_b_front_can_accept_response(b->id, active_write_ops_),
        "B_FRONT_CAN_ACCEPT: id deque empty or front op already fully responded");
    auto& deq = active_write_ops_[b->id];
    auto& op = deq.front();
    op.b_count_++;
    if (static_cast<uint8_t>(b->resp) > static_cast<uint8_t>(op.worst_resp_))
        op.worst_resp_ = b->resp;
    if (op.b_count_ == op.sub_bursts.size()) {
        if (wcb_) wcb_(WriteResult{...});
        deq.pop_front();
        active_write_count_--;
        if (deq.empty()) active_write_ops_.erase(b->id);  // optional cleanup
    }
}
```

R handling similar with multi-beat handling: `check_r_front_can_accept_beat(rid, rlast, active_read_ops_)`; intermediate beats don't advance sub-burst index; only rlast does.

### 4.2 `axi::protocol_rules` template helpers (Stage 2 modify)

Two new templated free functions in `c_model/include/axi/protocol_rules.hpp`:

```cpp
// Templated so protocol_rules.hpp doesn't need to know AxiMasterT::OperationContext
// (which is a private nested type defined after this header is included).
//
// OpType is structurally typed: requires .b_count_, .sub_bursts (size()),
// .cur_r_sub_idx_, .r_beats_in_cur_, .sub_bursts[idx].len.
template<typename OpType>
inline bool check_b_front_can_accept_response(
    uint8_t bid,
    const std::map<uint8_t, std::deque<OpType>>& active_write_ops) {
    auto it = active_write_ops.find(bid);
    if (it == active_write_ops.end() || it->second.empty()) return false;
    const auto& op = it->second.front();
    return op.b_count_ < op.sub_bursts.size();
}

template<typename OpType>
inline bool check_r_front_can_accept_beat(
    uint8_t rid, bool rlast,
    const std::map<uint8_t, std::deque<OpType>>& active_read_ops) {
    auto it = active_read_ops.find(rid);
    if (it == active_read_ops.end() || it->second.empty()) return false;
    const auto& op = it->second.front();
    if (op.cur_r_sub_idx_ >= op.sub_bursts.size()) return false;
    const auto& sub = op.sub_bursts[op.cur_r_sub_idx_];
    if (op.r_beats_in_cur_ > sub.len) return false;
    // rlast must occur iff this is the final beat of the current sub-burst
    return rlast == (op.r_beats_in_cur_ == sub.len);
}
```

Used by AxiMaster's B/R receipt path via `AXI_PROTOCOL_ASSERT(...)` macro (debug build fail-loud, release build no-op per existing project pattern).

### 4.3 `nmu::addr_trans` (new)

```cpp
// c_model/include/nmu/addr_trans.hpp
#pragma once
#include "ni_flit_constants.h"
#include <cstdint>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t  dst_id;     // X_WIDTH + Y_WIDTH = 8 bits (per ni_packet.json)
    uint64_t local_addr; // for c_model = addr (no remap)
};

// XYRouting bit allocation (c_model policy, will move when SAM/remap added):
//   addr[15:0]  = local address (64KB per dst)
//   addr[19:16] = x (low 4 bits of dst_id)
//   addr[23:20] = y (high 4 bits of dst_id)
//   addr[63:24] = unused (zero in current test fixtures)
//
// local_addr is unmodified — XYRouting only extracts dst_id; address space
// is global for c_model. Future SAM/remap may set local_addr = addr - base.
inline Translated xy_route(uint64_t addr) noexcept {
    constexpr uint64_t LOCAL_ADDR_BITS = 16;
    uint8_t dst = static_cast<uint8_t>((addr >> LOCAL_ADDR_BITS) & 0xFF);
    return { dst, /*local_addr=*/addr };
}

}  // namespace ni::cmodel::nmu::addr_trans
```

### 4.4 `nmu::Packetize` refactor (modify)

**Removed**: sticky-setter API (`set_aw_header_extras`, `set_ar_header_extras`), pending flags (`aw_extras_pending_`, `ar_extras_pending_`), latched state (`aw_dst_id_`, `aw_rob_*_`, `ar_*_`).

**Added**:
```cpp
struct AwHeaderMeta {
    uint8_t  dst_id;     // from addr_trans
    uint64_t local_addr; // from addr_trans (= awaddr for c_model)
    uint8_t  rob_req;    // 0 in Disabled mode, 1 in Enabled (next round)
    uint8_t  rob_idx;    // 0 in Disabled mode, allocated in Enabled
};

class Packetize : public Packetizer {
public:
    Packetize(noc::NocReqOut& req_out, uint8_t src_id);

    // Frozen Packetizer interface — used when no Rob in chain (test-mode bypass)
    bool push_aw(const axi::AwBeat& b) override {
        auto t = addr_trans::xy_route(b.addr);
        return push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
    }
    bool push_w(const axi::WBeat& b) override;  // unchanged — uses w_meta_fifo_
    bool push_ar(const axi::ArBeat& b) override {
        auto t = addr_trans::xy_route(b.addr);
        return push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
    }
    bool push_b(const axi::BBeat&) override { assert(false && "NMU pkt: B"); std::abort(); return false; }
    bool push_r(const axi::RBeat&) override { assert(false && "NMU pkt: R"); std::abort(); return false; }

    // Non-interface, called by Rob with full metadata (Enabled mode supplies rob_idx via this path)
    bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta);
    bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta);

private:
    noc::NocReqOut& req_out_;
    uint8_t src_id_;
    struct WMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };  // no local_addr (W has no addr per AXI4)
    std::deque<WMeta> w_meta_fifo_;
};
```

**push_aw_with_meta impl** (using `meta.local_addr`, NOT `b.addr`):
```cpp
inline bool Packetize::push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) {
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_AW);
    f.set_header_field("src_id",  src_id_);
    f.set_header_field("dst_id",  meta.dst_id);
    f.set_header_field("vc_id",   0);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", meta.rob_req);
    f.set_header_field("rob_idx", meta.rob_idx);
    f.set_payload_field("AW", "awid",   b.id);
    f.set_payload_field("AW", "awaddr", meta.local_addr);  // future remap-safe
    f.set_payload_field("AW", "awlen",  b.len);
    // ... other AW fields ...
    if (!req_out_.push_flit(f)) return false;  // backpressure: NO state change
    w_meta_fifo_.push_back({meta.dst_id, meta.rob_req, meta.rob_idx});
    return true;
}
```

### 4.5 `nmu::Rob` (new)

```cpp
// c_model/include/nmu/rob.hpp
#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/packetize.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nmu {

enum class RobMode { Disabled, Enabled };  // Enabled = next round

class Rob : public Packetizer, public Depacketizer {
public:
    Rob(Packetize& next_pkt,
        Depacketizer& next_depkt,
        RobMode mode_w,
        RobMode mode_r);

    // ===== Packetizer interface (request side; B/R assert) =====
    bool push_aw(const axi::AwBeat& b) override;
    bool push_w (const axi::WBeat&  b) override;
    bool push_ar(const axi::ArBeat& b) override;
    bool push_b(const axi::BBeat&) override { assert(false && "Rob: push_b"); std::abort(); return false; }
    bool push_r(const axi::RBeat&) override { assert(false && "Rob: push_r"); std::abort(); return false; }

    // ===== Depacketizer interface (response side; AW/W/AR assert) =====
    std::optional<axi::BBeat> pop_b() override;
    std::optional<axi::RBeat> pop_r() override;
    std::optional<axi::AwBeat> pop_aw() override { assert(false && "Rob: pop_aw"); std::abort(); return std::nullopt; }
    std::optional<axi::WBeat>  pop_w()  override { assert(false && "Rob: pop_w");  std::abort(); return std::nullopt; }
    std::optional<axi::ArBeat> pop_ar() override { assert(false && "Rob: pop_ar"); std::abort(); return std::nullopt; }

private:
    Packetize&     next_pkt_;      // concrete type (to call push_aw_with_meta)
    Depacketizer&  next_depkt_;
    RobMode mode_w_, mode_r_;

    // Per-AXI-ID FIFO of outstanding entries. Disabled mode invariant:
    // for any non-empty deque, all entries share the same dst_id (gate enforces).
    // Forward-compat: add rob_idx field for Enabled mode without restructuring.
    struct OutstandingEntry {
        uint8_t dst_id;
        // future Enabled mode: uint8_t rob_idx;
    };
    struct WriteState { std::deque<OutstandingEntry> outstanding; };
    struct ReadState  { std::deque<OutstandingEntry> outstanding; };
    std::array<WriteState, 256> write_;
    std::array<ReadState,  256> read_;

    // W burst credit gate: prevents W beats from reaching Packetize before
    // their corresponding AW has been ROB-accepted. Single counter (not per-id)
    // because AXI4 W beats follow AW issue order strictly (no WID).
    uint32_t w_burst_credit_ = 0;
};

}  // namespace ni::cmodel::nmu
```

**push_aw state machine (Disabled mode)**:
```cpp
inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_aw not yet implemented");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = write_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;  // stall: same id, different dst
    }
    if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;  // downstream backpressure: NO state change
    }
    s.outstanding.push_back({t.dst_id});
    w_burst_credit_++;
    return true;
}
```

**push_w state machine** (credit gate; pass-through to packetize on success):
```cpp
inline bool Rob::push_w(const axi::WBeat& b) {
    if (w_burst_credit_ == 0) return false;  // W cannot proceed before its AW
    if (!next_pkt_.push_w(b)) return false;  // downstream backpressure: NO credit change
    if (b.last) w_burst_credit_--;
    return true;
}
```

**push_ar state machine** (Disabled mode mirror, no W credit):
```cpp
inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_ar not yet implemented");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = read_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;
    }
    if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    s.outstanding.push_back({t.dst_id});
    return true;
}
```

**pop_b** (Disabled mode: pop FIFO front to free dst; Enabled mode stubbed for next round):
```cpp
inline std::optional<axi::BBeat> Rob::pop_b() {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_b not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_b();
    if (!opt) return std::nullopt;
    // Disabled mode
    auto& s = write_[opt->id];
    assert(!s.outstanding.empty() && "B for id with no outstanding write");
    s.outstanding.pop_front();
    return opt;
}
```

**pop_r** (multi-beat; pop FIFO only on `rlast=1`; Enabled mode stubbed for next round):
```cpp
inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_r not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_r();
    if (!opt) return std::nullopt;
    // Disabled mode
    if (opt->last) {
        auto& s = read_[opt->id];
        assert(!s.outstanding.empty() && "R(last) for id with no outstanding read");
        s.outstanding.pop_front();
    }
    return opt;
}
```

### 4.6 Tick-order + state-sharing semantics

- Single-threaded tick model: Rob's Packetizer-side mutations (push_aw → outstanding.push_back + credit++) and Depacketizer-side mutations (pop_b → outstanding.pop_front) happen in same thread, no synchronization needed.
- AxiSlavePort tick order (per Stage 3 port-pair task): drain B/R from depkt BEFORE forwarding AW/W/AR to pkt. So a response in same cycle CAN free an id and allow next-cycle issue of new dst.
- ID free is delayed whenever `Rob::pop_b/pop_r` is not reached or returns nullopt (B/R port queue full, NoC latency, HoL blocking, etc.). This is by design — no fairness violation since AxiMaster can still issue other ids.

### 4.7 ROB responsibility boundary (precise)

ROB Disabled mode ONLY handles **same-id DIFFERENT-dst** stall. Same-id SAME-dst response ordering is guaranteed by:
- AxiMaster per-id FIFO submission order (post AXI4 conformity fix)
- Downstream NoC's per-pair deterministic routing (XYRouting satisfies this; plan §3.1 line 70)

→ ROB Disabled does NOT add per-id-same-dst ordering — that's an implicit invariant from the AxiMaster + NoC routing combination.

---

## 5. Test plan (simplified per "化繁為簡" principle)

### 5.1 `test_addr_trans.cpp` — 3 tests

| # | Test | Purpose |
|---|---|---|
| 1 | `XyRoute_LowBitsAreLocalAddr` | addr=0x1234 → dst=0, local_addr=0x1234 — verifies low-bit pass-through |
| 2 | `XyRoute_HighBitsDecodeXY` | addr=0x00FF0000 → dst=0xFF (x=0xF, y=0xF) — exercises full bit-extraction |
| 3 | `XyRoute_LocalAddrPassesThroughFullWidth` | addr=0xABCDEF12345678 → local_addr=0xABCDEF12345678 — confirms 64-bit unmasked |

Dropped (subset of others): `AddrZeroDstZero`, `Bit16TriggersDst1`.

### 5.2 `test_rob.cpp` — 10 tests

**ROB-specific core behavior (4)**:
| # | Test | Purpose |
|---|---|---|
| 1 | `Disabled_StallSameIdDiffDst` | core hazard: id=5 outstanding, new push id=5 different dst → return false |
| 2 | `Disabled_StallReleaseOnBComplete` | pop_b → outstanding.empty() → next stall-trigger passes |
| 3 | `Disabled_StallReleaseOnRlast` | R(last=1) → read outstanding decrements; intermediate beat does NOT |
| 4 | `Disabled_WCreditBlocksWBeforeAw` | push_w with credit=0 returns false (no AW yet) |

**ROB invariants (3)**:
| # | Test | Purpose |
|---|---|---|
| 5 | `Disabled_BackpressureAtomicityPushAw` | downstream full → state (outstanding/credit) unchanged; retry succeeds |
| 6 | `Disabled_MultiOutstandingSameIdSameDst_NoFalseStall` | happy path: 2 same-id same-dst → both pass; outstanding.size()=2 |
| 7 | `Disabled_WCreditMultiOutstandingCorrectDecrement` | 2 AW → credit=2 → 2 wlast → credit decrements in order |

**ROB edge cases (2)**:
| # | Test | Purpose |
|---|---|---|
| 8 | `Disabled_WBackpressureDoesNotConsumeCredit` | push_w succeeds at Rob but downstream rejects → credit unchanged |
| 9 | `Disabled_DifferentIdsIndependentNoInterference` | id=5 stalled does not block id=6 |

**Defensive (1)**:
| # | Test | Purpose |
|---|---|---|
| 10 | `Disabled_AbortPaths` | 5 EXPECT_DEATH consolidated: push_b/push_r + pop_aw/pop_w/pop_ar all abort |

Dropped (per Codex round-8 + simplification): `BackpressureAtomicityPushAr` (mirror of #5), `ReadPathMirrorOfWritePath` (read semantics already in #3 + #9).

### 5.3 `test_packetize.cpp` — modify (net −2)

- DELETE 4 sticky-setter death tests: `StickySetterAssertMissingSet`, `StickySetterAssertDoubleSet`, `StickySetterArMissingSet`, `StickySetterArDoubleSet`
- KEEP 8 bit-perfect / W FIFO / NoC backpressure / disabled-fields-zero tests (with `pkt.push_aw(beat)` directly, no sticky setter)
- ADD 2:
  - `PushAwWithMeta_OverrideDefault` — call `push_aw_with_meta(beat, custom_meta)` with custom dst/rob → flit reflects custom meta
  - `AddrTransIntegratedDstIdInHeader` — `push_aw(beat)` (frozen iface) → flit dst_id == addr_trans::xy_route(beat.addr).dst_id

### 5.4 `test_protocol_rules.cpp` — +2 tests

| # | Test | Purpose |
|---|---|---|
| 1 | `CheckBFront_NoOutstandingOrFullyResponded_Rejects` | EXPECT_DEATH twice: empty deque + already-fully-responded op |
| 2 | `CheckRFront_BadBeatTimingOrRlast_Rejects` | EXPECT_DEATH multiple: empty deque + intermediate beat with rlast=1 (mismatch) + extra beat beyond sub.len |

### 5.5 `test_axi_master.cpp` — modify + 1 new

- Update 5-8 existing tests that assume same-id 1-outstanding implicit limit (most likely scenario_parser fixture-driven tests; concrete list emerges during impl)
- ADD 1 new: `SameIdConcurrentAdmissionWithFifoOrderedB` — issue 2 same-id writes concurrently (max_out_w ≥ 2), verify both admit, B responses route via per-id FIFO front in submission order, both writes complete in order at scoreboard

### 5.6 Integration test `test_request_response_loopback.cpp` — modify

- Replace TestPacketize wrapper with Rob in pipeline:
  ```cpp
  nmu::Rob rob(real_nmu_pkt, nmu_depkt, RobMode::Disabled, RobMode::Disabled);
  nmu::AxiSlavePort port(rob, rob, params);  // Rob is both Packetizer and Depacketizer
  ```
- DELETE `c_model/tests/common/test_packetize_adapter.hpp`
- Existing 6 fixtures unchanged (awaddrs ≤ 0xFFFF → all dst=0 → no stall) — verify no regression
- ADD 1 new fixture `multi_dst_stress.yaml`:
  - 2 same-id writes (id=0x05) at addr 0x100 (dst=0) + 0x10100 (dst=1)
  - **No YAML schema change**: the integration test rig overrides `max_outstanding_write` parameter to 2 specifically for this fixture (in the `INSTANTIATE_TEST_SUITE_P` tuple or via a small per-fixture override map in the rig — concrete decision deferred to implementation phase). Existing fixtures keep their YAML-config-driven defaults; only multi_dst_stress needs the override.
  - Memory size in test rig scaled to ≥ 0x11000 to cover both addrs
  - Expected: AxiMaster admits both concurrently → ROB stalls 2nd → B1 returns → outstanding empty → ROB releases 2nd → scoreboard zero mismatch

Final integration: 7 fixture variants (6 existing + 1 new).

---

## 6. Open follow-ups (deferred, documented)

| Item | Owner / round |
|---|---|
| ROB Enabled mode (per-id reorder buffer for R, FIFO metadata buffer for B, rob_idx allocator) | Next round, per plan §3.1 |
| LoopbackNoc per-dst latency variation extension (enables Enabled mode out-of-order test) | Paired with Enabled mode round |
| vc_mapping (NUM_VC=1 trivial) + vc_arb (round-robin + per-VC credit) | Later round |
| addr_trans SAM table (IDRouting) + SourceRouting | Future task |
| addr_trans address remap (local_addr ≠ addr) | Future task; current `local_addr = addr` is c_model policy |
| route_par / flit_ecc algorithm helpers | Orthogonal, anytime |
| nmu.hpp / nsu.hpp top-level assembly | After all sub-components done |
| FlooNoC reference for Enabled mode design: `floo_rob.sv` (full SRAM) + `floo_simple_rob.sv` (B metadata) + `tb_floo_rob.sv` (variable-latency slave test pattern) | Read at start of Enabled mode round |

---

## 7. Karpathy 4-lens self-check

- **Overcomplication**: addr_trans is a 6-line pure function; Rob is single multi-inheritance class with 2 small state arrays + 1 credit counter (~120 lines); Packetize refactor net-shrinks (removes sticky setter machinery). Enabled mode body stubbed with assert+abort — no premature implementation. AxiMaster relaxation is targeted (3-4 method changes + type change), not rewrite.
- **Surgical**:
  - Stage 2 axi/ is modified (per AXI4 conformity), NOT frozen this round — but changes are localized to AxiMaster + protocol_rules helpers; types/algos unchanged elsewhere.
  - Stage 3 port-pair (axi_slave_port, axi_master_port) and packetize+depacketize prior round unchanged.
  - Only NMU side touched for ROB; NSU side untouched.
- **Surface assumptions** (spec explicit):
  - XYRouting `LOCAL_ADDR_BITS=16` is c_model policy (will move with SAM/remap)
  - ROB Disabled assumes deterministic per-pair routing (plan §3.1 line 70; XYRouting satisfies)
  - Same-id same-dst ordering is implicit invariant from AxiMaster + NoC routing — not ROB's job
  - AxiSlavePort tick order (drain B/R first) enables same-cycle ID free in best case
  - ID free is delayed whenever pop_b/pop_r is unavailable (any cause)
- **Verifiable success**:
  - 3 addr_trans + 10 Rob + 2 protocol_rules + 1 axi_master same-id + ~10 modified packetize/axi_master tests
  - Integration: 7 fixtures (6 existing no-regression + 1 new multi_dst_stress with ROB stall path)
  - All drift gates clean
  - ctest count estimate: ~285 → ~300+ (depending on AxiMaster test impact scope)

---

## 8. Drift gates (every commit must pass)

```bash
cd specgen && py -3 -m pytest -q                  # 163 (no specgen changes this round)
py -3 tools/codegen.py --check                    # clean (no spec change)
py -3 tools/gen_inventory.py --check              # clean
cd ../c_model && cmake --build build && ctest --test-dir build -j 1  # ~300+, all pass
```
