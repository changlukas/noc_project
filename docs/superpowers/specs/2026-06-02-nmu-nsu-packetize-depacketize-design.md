# NMU/NSU Packetize + Depacketize — Design Spec

**Status**: Draft, brainstormed 2026-06-02, awaiting user review
**Stage**: Stage 3 (post port-pair, before vc_arb / ROB / addr_trans)
**Anchor docs**: `docs/noc_cmodel_rtl_plan.md` §3 + §8 (next-minimum-action), `NEXT_STEPS.md`

---

## 1. Goal

Add the four NoC packet encoders/decoders that sit between the Stage 3 port pair and the (future) NoC routers:

- `nmu::Packetize`     — AXI AW/W/AR beats → request flit
- `nmu::Depacketize`   — response flit → AXI B/R beats
- `nsu::Depacketize`   — request flit → AXI AW/W/AR beats
- `nsu::Packetize`     — AXI B/R beats → response flit

Plus the supporting `nsu::MetaBuffer` (per `plan §3` module list), the NoC-side abstract interfaces, a complete `Flit` class, a `LoopbackNoc` test bridge, codegen extension to emit per-payload-field bit positions, and an end-to-end loopback integration test reusing the Stage 2 `AxiMaster`/`AxiSlave`/`Memory`/`Scoreboard` rig.

**Success criterion**: integration test replays ≥5 Stage 2 YAML fixtures through `AxiMaster → AxiSlavePort → nmu::Packetize → LoopbackNoc → nsu::Depacketize → AxiMasterPort → AxiSlave + Memory` (and the response path back through `nsu::Packetize → LoopbackNoc → nmu::Depacketize`) with **zero Scoreboard mismatches**, plus per-module unit tests for bit-perfect pack/unpack and backpressure.

---

## 2. Scope

### In-scope

| Module | New file |
|---|---|
| `nmu::Packetize` | `c_model/include/nmu/packetize.hpp` |
| `nmu::Depacketize` | `c_model/include/nmu/depacketize.hpp` |
| `nsu::Depacketize` | `c_model/include/nsu/depacketize.hpp` |
| `nsu::Packetize` | `c_model/include/nsu/packetize.hpp` |
| `nsu::MetaBuffer` | `c_model/include/nsu/meta_buffer.hpp` |
| `noc::NocReqOut` (abstract) | `c_model/include/noc/noc_req_out.hpp` |
| `noc::NocReqIn` (abstract) | `c_model/include/noc/noc_req_in.hpp` |
| `noc::NocRspOut` (abstract) | `c_model/include/noc/noc_rsp_out.hpp` |
| `noc::NocRspIn` (abstract) | `c_model/include/noc/noc_rsp_in.hpp` |
| `Flit` (functional-complete) | `c_model/include/ni/flit.hpp` (replaces `c_model/include/flit.hpp`) |
| `LoopbackNoc` test fixture | `c_model/tests/common/loopback_noc.hpp` |
| Per-module unit tests | `c_model/tests/{nmu,nsu}/test_{packetize,depacketize}.cpp` (×4) |
| Integration loopback test | `c_model/tests/integration/test_request_response_loopback.cpp` |
| Codegen extension (payload field LSB/MSB emission) | `specgen/tools/elaborate/cpp_packet.py`, `sv_packet.py` |
| Spec lock for `axi_ch` encoding | `specgen/generated/json/ni_packet.json` (add `axi_ch.encoding`) |

### Out-of-scope (deferred to named separate tasks)

| Concern | Owner module (per `plan §3`) | Why deferred |
|---|---|---|
| MUX 3→1 (AW/W/AR → single NoC req flit/cycle) | `nmu::VcArb` + `nsu::VcArb` | Independent task; needs credit logic + round-robin arbitration; LoopbackNoc here does not rate-limit, so data-fidelity tests work without MUX |
| VC mapping (fixed VC=0 for NUM_VC=1) | `nmu::VcMapping` | Trivial today (hardcode vc_id=0); separates cleanly when NUM_VC>1 |
| `route_par` field value computation (XOR over coverage fields) | Future helper | Field is enabled in spec but algorithm is orthogonal; this spec 0-fills with TODO |
| `flit_ecc` field value computation (SECDED Hamming over FLIT_DATA_WIDTH=398 bits) | Future ECC module | Standalone algorithm; ~150 lines + unit tests; this spec 0-fills with TODO |
| Address translation (awaddr/araddr → dst_id) | `nmu::AddrTrans` | Caller supplies dst_id via sticky setter; addr_trans task replaces caller with translator |
| Reorder buffer (per-AXI-ID response reordering) | `nmu::Rob` | rob_req=0, rob_idx=0 defaults in this spec; ROB task replaces with proper allocator |
| Atomic-ID tagging on meta_buffer | Future enhancement to `nsu::MetaBuffer` | Minimal per-ID FIFO suffices for tested fixtures; atomic tagging is a future safety net |

---

## 3. Architecture

### 3.1 Module responsibility table

| Module | Responsibility | Explicit non-responsibilities |
|---|---|---|
| `nmu::Packetize` | bit-pack `AwBeat`/`WBeat`/`ArBeat` → `Flit`; call `NocReqOut.push_flit()`; maintain small **write-metadata FIFO** so W beats inherit AW's dst_id/rob_* | MUX across channels, route_par/ECC value computation, credit |
| `nmu::Depacketize` | tick: pull flit from `NocRspIn`, demux by `axi_ch` into B queue / R queue; serve upstream port's `pop_b/pop_r` | Cross-ID reorder (ROB), ECC validation |
| `nsu::Depacketize` | tick: pull flit from `NocReqIn`, demux by `axi_ch` into AW/W/AR queues; serve upstream port's `pop_aw/pop_w/pop_ar`; **on AW/AR demux, snapshot metadata into `MetaBuffer`** | Cross-ID reorder, ECC validation |
| `nsu::Packetize` | bit-pack `BBeat`/`RBeat` → `Flit`; **look up dst_id / rob_* from MetaBuffer by bid/rid**; call `NocRspOut.push_flit()` | MUX, route_par/ECC, credit |
| `nsu::MetaBuffer` | snapshot per-incoming-AW/AR: `{src_id, awid|arid, rob_req, rob_idx}`; lookup on outgoing B (by bid) and R (by rid+rlast); pop on consume | Atomic ID assignment, cross-ID ordering |
| `LoopbackNoc` | implements all 4 NoC abstracts; bounded deque per direction (req_q_, rsp_q_); accepts multiple flits per tick | **Does NOT model 1-flit/cycle physical NoC pacing** — that's vc_arb's job |
| Specgen extension | emit `ni::payload::<ch>::<FIELD>_LSB/MSB` constants; per-channel `static_assert` that sum-of-widths == channel `WIDTH` | Change spec structure or field layout |

### 3.2 Data flow

**Forward (request):**
```
AxiMaster              (Stage 2)
  └─push_aw/w/ar──▶ AxiSlavePort        (Stage 3 port-pair)
                      └─push_aw/w/ar──▶ nmu::Packetize       ── NEW
                                          └─push_flit──▶ NocReqOut
                                                          └─(LoopbackNoc.req_q_)
                                                              └─pop_flit──▶ NocReqIn
                                                                              └─(nsu::Depacketize.tick demux)
                                                                                  └─pop_aw/w/ar──▶ AxiMasterPort  (Stage 3)
                                                                                                      └─push_aw/w/ar──▶ AxiSlave + Memory  (Stage 2)
```

**Reverse (response):**
```
AxiSlave + Memory      (Stage 2)
  └─pop_b/r──▶ AxiMasterPort   (Stage 3)
                  └─pop_b/r──▶ nsu::Packetize          ── NEW
                                  ├─lookup────▶ nsu::MetaBuffer  ── NEW
                                  └─push_flit──▶ NocRspOut
                                                  └─(LoopbackNoc.rsp_q_)
                                                      └─pop_flit──▶ NocRspIn
                                                                      └─(nmu::Depacketize.tick demux)
                                                                          └─pop_b/r──▶ AxiSlavePort  (Stage 3)
                                                                                          └─pop_b/r──▶ AxiMaster  (Stage 2)
                                                                                                          └─Scoreboard observation
```

Scoreboard from Stage 2 observes the full round-trip. Zero mismatch = pass.

---

## 4. Module specifications

### 4.1 `noc::NocReqOut` / `noc::NocReqIn` / `noc::NocRspOut` / `noc::NocRspIn`

Four single-method abstract bases. Names match `ni_signals.json` pin struct conventions (`NocReqOutPins`, etc.).

```cpp
// c_model/include/noc/noc_req_out.hpp
namespace ni::cmodel::noc {
class NocReqOut {
public:
  virtual ~NocReqOut() = default;
  // Returns false when downstream cannot accept (= upstream sees backpressure)
  virtual bool push_flit(const Flit&) = 0;
};
}

// c_model/include/noc/noc_req_in.hpp
namespace ni::cmodel::noc {
class NocReqIn {
public:
  virtual ~NocReqIn() = default;
  // Returns nullopt when empty
  virtual std::optional<Flit> pop_flit() = 0;
};
}

// NocRspOut / NocRspIn mirror with identical shape
```

No I prefix (matches existing `Packetizer`/`Depacketizer` convention from port-pair). No `noexcept` (matches Stage 2 axi/ grain).

### 4.2 `ni::Flit` (`c_model/include/ni/flit.hpp`, replaces `c_model/include/flit.hpp`)

```cpp
namespace ni::cmodel {
class Flit {
public:
  static constexpr int WIDTH_BITS  = ni::FLIT_WIDTH;          // 408
  static constexpr int WIDTH_BYTES = (WIDTH_BITS + 7) / 8;    // 51

  Flit() = default;
  explicit Flit(const std::array<uint8_t, WIDTH_BYTES>& raw);

  // Header field access — all 13 fields auto-dispatched via codegen LSB/MSB
  void     set_header_field(std::string_view name, uint64_t value);
  uint64_t get_header_field(std::string_view name) const;

  // Per-channel payload field access via codegen-emitted ni::payload::<ch>::<FIELD>_LSB/MSB
  void     set_payload_field(std::string_view channel, std::string_view field, uint64_t value);
  uint64_t get_payload_field(std::string_view channel, std::string_view field) const;

  // Bulk payload bytes for wide fields (wdata, rdata 256-bit)
  void set_payload_bytes(std::string_view channel, std::string_view field,
                         const uint8_t* src, std::size_t bit_width);
  void get_payload_bytes(std::string_view channel, std::string_view field,
                         uint8_t* dst, std::size_t bit_width) const;

  const std::array<uint8_t, WIDTH_BYTES>& raw() const noexcept { return raw_; }
  bool check_padding_is_zero() const;  // rsvd field, route_par/flit_ecc if disabled, etc.

private:
  std::array<uint8_t, WIDTH_BYTES> raw_{};
};
}  // namespace ni::cmodel
```

Internal dispatch tables live in `detail::` namespace, populated from codegen constants — **no hand-rolled `if-else` chains**. Adding/removing fields in the spec automatically reflects in dispatch (drift impossible).

**Existing `c_model/include/flit.hpp` and `c_model/tests/test_flit.cpp`**: same-commit migration. The old header is deleted (no deprecated wrapper). `test_flit.cpp` updates its `#include` path. No external consumer exists (grep-verified before edit).

### 4.3 `nmu::Packetize`

```cpp
namespace ni::cmodel::nmu {
class Packetize : public Packetizer {  // Packetizer from port-pair task (frozen)
public:
  Packetize(noc::NocReqOut& req_out, uint8_t src_id);

  // ---- Packetizer interface (called by AxiSlavePort) ----
  bool push_aw(const axi::AwBeat& b) override;
  bool push_w (const axi::WBeat&  b) override;
  bool push_ar(const axi::ArBeat& b) override;

  // ---- Per-txn header setter (sticky, fail-loud) ----
  // Caller MUST call set_aw_header_extras() before each push_aw(); double-set without consume asserts.
  // No setter for W: dst_id/rob_* for W beats are read from the internal write-metadata FIFO (populated by push_aw).
  void set_aw_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0);
  void set_ar_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0);

private:
  noc::NocReqOut& req_out_;
  uint8_t src_id_;

  // Sticky setter state (fail-loud per Codex review)
  bool aw_extras_pending_ = false;
  bool ar_extras_pending_ = false;
  uint8_t aw_dst_id_, aw_rob_req_, aw_rob_idx_;
  uint8_t ar_dst_id_, ar_rob_req_, ar_rob_idx_;

  // Write-metadata FIFO: populated on push_aw, consumed on W beat with wlast=1
  struct WriteMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };
  std::deque<WriteMeta> w_meta_fifo_;
};
}
```

**State machine (per push_aw)**:
1. Assert `aw_extras_pending_` else fatal.
2. Build flit from beat + (src_id_, dst_id, vc_id=0, axi_ch=AW, last=1, rob_req, rob_idx, commtype=0, multicast=0, noc_qos=0, route_par=0, flit_ecc=0).
3. `req_out_.push_flit(f)`. If false: return false (do NOT clear pending flag, do NOT push to w_meta_fifo_; caller retries).
4. On success: push `{dst_id, rob_req, rob_idx}` to `w_meta_fifo_`, clear `aw_extras_pending_`, return true.

**State machine (per push_w)**:
1. Assert `!w_meta_fifo_.empty()` else fatal ("W beat before any AW issued").
2. Read front of `w_meta_fifo_` (peek, don't pop yet) for dst_id/rob_*. *(Note: rob_* on W flit is the AW's rob_* per AXI4 ID semantics.)*
3. Build flit with axi_ch=W, beat fields, dst_id, rob_*.
4. `req_out_.push_flit(f)`. If false: return false (no state change).
5. On success: if `b.last == 1`, pop front of `w_meta_fifo_`; return true.

**State machine (per push_ar)**: mirror of push_aw, no W-side FIFO involvement.

### 4.4 `nmu::Depacketize`

```cpp
namespace ni::cmodel::nmu {
class Depacketize : public Depacketizer {  // Depacketizer from port-pair task (frozen)
public:
  Depacketize(noc::NocRspIn& rsp_in,
              std::size_t b_q_depth, std::size_t r_q_depth);

  // Pull flits from rsp_in, demux into per-channel queues
  void tick();

  // Depacketizer interface (called by AxiSlavePort)
  std::optional<axi::BBeat> pop_b() override;
  std::optional<axi::RBeat> pop_r() override;

private:
  noc::NocRspIn& rsp_in_;
  std::deque<axi::BBeat> b_q_;
  std::deque<axi::RBeat> r_q_;
  std::size_t b_q_depth_, r_q_depth_;
  std::optional<Flit> pending_;  // held when target channel queue full (head-of-line blocking)
};
}
```

**tick() loop**:
```cpp
void tick() {
  while (true) {
    // Resolve pending flit first (head-of-line constraint)
    Flit f = pending_ ? *pending_ : Flit{};
    if (!pending_) {
      auto opt = rsp_in_.pop_flit();
      if (!opt) return;
      f = *opt;
    }
    uint64_t ch = f.get_header_field("axi_ch");
    // ni::AXI_CH_AW / W / AR / B / R are codegen-emitted from ni_packet.json axi_ch.encoding
    switch (ch) {
      case ni::AXI_CH_B:
        if (b_q_.size() >= b_q_depth_) { pending_ = f; return; }
        b_q_.push_back(decode_b_flit(f));
        break;
      case ni::AXI_CH_R:
        if (r_q_.size() >= r_q_depth_) { pending_ = f; return; }
        r_q_.push_back(decode_r_flit(f));
        break;
      default:
        assert(false && "NocRspIn delivered non-B/non-R flit");
    }
    pending_.reset();
  }
}
```

**Pending-flit stash semantics**: when `pending_` holds a flit whose target channel queue is full, subsequent ticks re-attempt the same flit. **Other flits in `NocRspIn` are blocked behind it** (head-of-line blocking). This is a deliberate single-FIFO ingress design — if cross-channel ingress independence is needed later, the network design itself (multi-VC) would provide it, not depacketize.

### 4.5 `nsu::Depacketize`

Same shape as `nmu::Depacketize` but with three channel queues (`aw_q_`, `w_q_`, `ar_q_`) and an additional side-effect: when demuxing an AW or AR flit, snapshot `{src_id, awid|arid, rob_req, rob_idx}` into the `MetaBuffer` referenced by ctor arg.

```cpp
class Depacketize : public Depacketizer {
public:
  Depacketize(noc::NocReqIn& req_in, MetaBuffer& meta,
              std::size_t aw_q_depth, std::size_t w_q_depth, std::size_t ar_q_depth);
  void tick();
  std::optional<axi::AwBeat> pop_aw() override;
  std::optional<axi::WBeat>  pop_w()  override;
  std::optional<axi::ArBeat> pop_ar() override;
private:
  noc::NocReqIn& req_in_;
  MetaBuffer& meta_;
  std::deque<axi::AwBeat> aw_q_;
  std::deque<axi::WBeat>  w_q_;
  std::deque<axi::ArBeat> ar_q_;
  std::size_t aw_q_depth_, w_q_depth_, ar_q_depth_;
  std::optional<Flit> pending_;
};
```

On AW flit: `meta_.snapshot_write(awid, {src_id, rob_req, rob_idx})`.
On AR flit: `meta_.snapshot_read (arid, {src_id, rob_req, rob_idx})`.
On W: no metadata side effect (W's address/id resolution lives in the AXI Master Port side, not in our flit header).

### 4.6 `nsu::Packetize`

```cpp
namespace ni::cmodel::nsu {
class Packetize : public Packetizer {  // implements 5-method Packetizer; push_aw/w/ar assert false
public:
  // src_id = NSU's own tile coord; becomes src_id in response flits (NoC routes back to NMU on this).
  Packetize(noc::NocRspOut& rsp_out, MetaBuffer& meta, uint8_t src_id);

  // Packetizer interface — NSU side only emits responses
  bool push_b(const axi::BBeat& b) override;
  bool push_r(const axi::RBeat& b) override;
  // Request-side methods assert false (NSU never emits requests)
  bool push_aw(const axi::AwBeat&) override { assert(false && "NSU packetize: AW not applicable"); return false; }
  bool push_w (const axi::WBeat&)  override { assert(false && "NSU packetize: W  not applicable"); return false; }
  bool push_ar(const axi::ArBeat&) override { assert(false && "NSU packetize: AR not applicable"); return false; }

private:
  noc::NocRspOut& rsp_out_;
  MetaBuffer& meta_;
  uint8_t src_id_;
};
}
```

**Interface mis-fit note**: `Packetizer` from the port-pair task carries both request (push_aw/w/ar) and response (push_b/r) halves so a single LoopbackPacketizer could serve both ports. For NSU's response-only role, push_aw/w/ar assert false — `AxiMasterPort` never calls them. If this assert-false pattern accumulates pain across more NSU-only consumers, split `Packetizer` into `RequestPacketizer` + `ResponsePacketizer` sub-interfaces in a follow-up.

**push_b state machine**:
1. Look up metadata: `auto meta_opt = meta_.consume_write(b.id);`. Assert `meta_opt` not nullopt ("B with no matching outstanding AW").
2. Build flit with axi_ch=B, dst_id=meta.src_id, src_id=`src_id_`, bid=b.id, bresp=b.resp, buser=b.user, rob_req/rob_idx=meta.rob_*, last=1.
3. `rsp_out_.push_flit(f)`. If false: return false. **Caveat**: `consume_write` already popped the metadata; on failed push we lose it. Mitigation: peek-only path — `peek_write(id)` to read without pop, push first, then `consume_write` on success. Spec implementation uses **peek+consume** pattern (see also push_r below).
4. On success: `meta_.consume_write(b.id)`; return true.

**push_r state machine**: similar — for multi-beat R bursts, `peek_read(r.id)` to fetch metadata for every R flit; `commit_read(r.id)` only when `r.last == 1`.

Both methods follow the same **peek-before-push, commit-on-success** pattern, so a failed `NocRspOut.push_flit()` never desynchronizes MetaBuffer.

### 4.7 `nsu::MetaBuffer`

```cpp
namespace ni::cmodel::nsu {
struct MetaEntry { uint8_t src_id; uint8_t rob_req; uint8_t rob_idx; };

class MetaBuffer {
public:
  MetaBuffer(std::size_t per_id_depth = 4);

  // On AW flit: store entry for awid.
  void snapshot_write(uint8_t awid, MetaEntry e);
  // On AR flit: store entry for arid.
  void snapshot_read (uint8_t arid, MetaEntry e);

  // On B with bid: pop front entry for bid (per AXI4 in-order-per-id semantics).
  // Returns nullopt if no matching entry (caller asserts).
  std::optional<MetaEntry> consume_write(uint8_t bid);
  // On R with rid: peek front entry; pop only when rlast=1 (call peek_read + commit_read).
  std::optional<MetaEntry> peek_read (uint8_t rid) const;
  void                     commit_read(uint8_t rid);

private:
  std::array<std::deque<MetaEntry>, 256> write_;  // per awid FIFO
  std::array<std::deque<MetaEntry>, 256> read_;   // per arid FIFO
  std::size_t per_id_depth_;
};
}
```

Per-ID FIFO. No global atomic-ID tagging (deferred). Multi-outstanding same-id: FIFO front is oldest entry per AXI4 ordering. Different-id: independent FIFOs.

### 4.8 `LoopbackNoc` test fixture

```cpp
namespace ni::cmodel::testing {
class LoopbackNoc {
public:
  LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth);

  // Test rig grabs 4 separate refs:
  noc::NocReqOut& req_out() { return req_out_adapter_; }
  noc::NocReqIn&  req_in()  { return req_in_adapter_;  }
  noc::NocRspOut& rsp_out() { return rsp_out_adapter_; }
  noc::NocRspIn&  rsp_in()  { return rsp_in_adapter_;  }

  // Optional: forced-latency variant (mirrors port-pair DelayedLoopback pattern)
  void set_req_delay(unsigned cycles) { req_delay_ = cycles; }
  void set_rsp_delay(unsigned cycles) { rsp_delay_ = cycles; }
  void tick();  // age delayed flits, move to visible queue

private:
  // Inner adapters avoid same-name virtual ambiguity from inheriting 4 abstract bases
  struct ReqOutAdapter : noc::NocReqOut { LoopbackNoc* p; bool push_flit(const Flit& f) override; };
  struct ReqInAdapter  : noc::NocReqIn  { LoopbackNoc* p; std::optional<Flit> pop_flit() override; };
  struct RspOutAdapter : noc::NocRspOut { LoopbackNoc* p; bool push_flit(const Flit& f) override; };
  struct RspInAdapter  : noc::NocRspIn  { LoopbackNoc* p; std::optional<Flit> pop_flit() override; };

  ReqOutAdapter req_out_adapter_;
  ReqInAdapter  req_in_adapter_;
  RspOutAdapter rsp_out_adapter_;
  RspInAdapter  rsp_in_adapter_;
  std::deque<Flit> req_q_, rsp_q_;
  // delayed pipes (only used when delay > 0)
  std::deque<std::pair<Flit, unsigned /*cycles_left*/>> req_pipe_, rsp_pipe_;
  unsigned req_delay_ = 0, rsp_delay_ = 0;
  std::size_t req_depth_, rsp_depth_;
};
}
```

**Pacing**: `push_flit` accepts as many flits per tick as queue space allows. **Does NOT model 1-flit/cycle physical NoC pacing**. Any latency/throughput metric derived from tests using LoopbackNoc is **test-time only, non-physical** — actual NoC pacing is `vc_arb`'s responsibility (future task).

---

## 5. Codegen extension

`specgen/tools/elaborate/cpp_packet.py` adds emission block:

```cpp
// In emitted ni_flit_constants.h, after existing namespace payload {...}:
namespace payload {
namespace aw {
  constexpr int AWID_LSB     = 0;    constexpr int AWID_MSB     = 7;
  constexpr int AWADDR_LSB   = 8;    constexpr int AWADDR_MSB   = 71;
  constexpr int AWLEN_LSB    = 72;   constexpr int AWLEN_MSB    = 79;
  // ... 11 fields per AW channel from ni_packet.json
  constexpr int AW_RSVD_LSB  = 105;  constexpr int AW_RSVD_MSB  = 107;
}
static_assert(aw::AW_RSVD_MSB + 1 == AW_WIDTH, "AW payload field LSB/MSB sum != AW_WIDTH");
namespace ar { /* mirror */ }
namespace w  { /* 5 fields: wlast, wuser, wstrb, wdata, w_rsvd */ }
namespace b  { /* 4 fields */ }
namespace r  { /* 6 fields */ }
}  // namespace payload
```

`sv_packet.py` emits `localparam int unsigned` equivalents.

`specgen/generated/json/ni_packet.json` gets `axi_ch.encoding`:
```json
{
  "name": "axi_ch",
  "width_param": "AXI_CH_WIDTH",
  "enabled": true,
  "encoding": { "0": "AW", "1": "W", "2": "AR", "3": "B", "4": "R" }
}
```

The codegen already supports `field.encoding` (per `constants.axi_channel_encoding`). Adding it here locks the encoding map at spec level (single source of truth, accessible from both C and SV codegen).

### Specgen file impact (full list)

| File | Change |
|---|---|
| `specgen/generated/json/ni_packet.json` | Add `axi_ch.encoding` map |
| `specgen/tools/elaborate/cpp_packet.py` | Emit per-payload-field LSB/MSB block + per-channel static_assert |
| `specgen/tools/elaborate/sv_packet.py` | Mirror SV emission |
| `specgen/generated/cpp/ni_flit_constants.h` | Regen (codegen output) |
| `specgen/generated/sv/ni_flit_pkg.sv` | Regen |
| `specgen/tests/golden/ni_flit_constants.h.golden` | Refresh |
| `specgen/tests/golden/ni_flit_pkg.sv.golden` | Refresh |
| `specgen/tests/test_constants_resolver.py` | Add per-channel payload position tests |
| `specgen/tests/test_codegen.py` | Add presence assertion for new namespace block |
| `specgen/tests/test_codegen_sv.py` | Same for SV |

`test_byte_identical_golden.py` will fail until goldens are refreshed (expected, refreshed in same commit).

---

## 6. Config (`c_model/config/port_params.yaml`)

```yaml
# existing port-pair fields (unchanged)
aw_queue_depth: 32
w_queue_depth:  32
ar_queue_depth: 32
b_queue_depth:  32
r_queue_depth:  32

# NEW: depacketize internal demux queues (NMU has B/R; NSU has AW/W/AR)
depacketize:
  aw_q_depth: 32
  w_q_depth:  32
  ar_q_depth: 32
  b_q_depth:  32
  r_q_depth:  32

# NEW: LoopbackNoc test fixture deque capacity
loopback_noc:
  req_depth: 32
  rsp_depth: 32

# NEW: MetaBuffer per-ID FIFO depth (default 4 outstanding per AXI ID)
meta_buffer:
  per_id_depth: 4
```

`PortParams` struct in `c_model/include/ni/port_params.hpp` extends to include these new fields. `load_port_params_yaml(path, side)` extended to parse them.

---

## 7. Test plan

### 7.1 Per-module unit tests

| Test file | Count | Coverage |
|---|---|---|
| `c_model/tests/nmu/test_packetize.cpp` | ~14 | push_aw/w/ar bit-perfect; sticky setter fail-loud (assert on missing/double set); write-metadata FIFO; NocReqOut backpressure; multiple-AW interleaved-W routing |
| `c_model/tests/nmu/test_depacketize.cpp` | ~12 | pop_b/r decode; demux mixed flits; per-channel backpressure (B full vs R progressing); **pending-flit head-of-line blocking** (W full → AR behind cannot progress); FIFO order |
| `c_model/tests/nsu/test_depacketize.cpp` | ~12 | mirror of NMU depacketize tests + MetaBuffer snapshot side-effects on AW/AR demux |
| `c_model/tests/nsu/test_packetize.cpp` | ~14 | push_b/r decode; metadata consume/peek; assert when no matching outstanding |
| `c_model/tests/nsu/test_meta_buffer.cpp` (optional, may inline above) | ~6 | snapshot_write/read; consume_write; peek_read+commit_read; per-ID FIFO ordering; depth boundary |

**Bit-perfect tests**: pack a beat, then decode the resulting flit bytes via `Flit::get_payload_field` / `get_header_field`, assert every field matches input. Repeat per-channel.

### 7.2 Integration e2e

`c_model/tests/integration/test_request_response_loopback.cpp`

Rig assembly (pseudocode):
```cpp
LoopbackNoc loopback(params.loopback_noc.req_depth, params.loopback_noc.rsp_depth);
MetaBuffer  nsu_meta(params.meta_buffer.per_id_depth);

nmu::Packetize    nmu_pkt   (loopback.req_out(), /*src_id=*/0x01);
nmu::Depacketize  nmu_depkt (loopback.rsp_in(),  params.depacketize.b_q_depth, params.depacketize.r_q_depth);
nsu::Depacketize  nsu_depkt (loopback.req_in(),  nsu_meta, params.depacketize.aw_q_depth, params.depacketize.w_q_depth, params.depacketize.ar_q_depth);
nsu::Packetize    nsu_pkt   (loopback.rsp_out(), nsu_meta, /*src_id=*/0x02);

TestPacketize     test_nmu_pkt(nmu_pkt, /*fixed_dst=*/0x02);   // wraps sticky-setter

AxiSlavePort   nmu_port(test_nmu_pkt, nmu_depkt, params);
AxiMasterPort  nsu_port(nsu_depkt,    nsu_pkt,   params);

AxiMaster    master(scenario_yaml, nmu_port, /*max_outstanding*/8, /*max_outstanding*/8);
Memory       memory(/*size*/64*1024);
AxiSlave     slave(memory);
Scoreboard   scoreboard;

master.on_write_completed([&](auto& w){ scoreboard.observe_write(w); });
master.on_read_observed  ([&](auto& r){ scoreboard.observe_read(r); });

// Per cycle — tick once in canonical order (each module ticks once per cycle)
for (cycle = 0; !master.done() || /* in-flight not drained */; ++cycle) {
  // === Drain side: response path (deepest → shallowest) ===
  nmu_depkt.tick();    // pull rsp flits, demux into B/R queues
  loopback.tick();     // age delayed flits in both pipes
  nsu_depkt.tick();    // pull req flits, demux into AW/W/AR queues + snapshot MetaBuffer

  // === Per-module tick (port-pair tick + Stage 2 tick) ===
  nmu_port.tick();     // upstream port forwards AW/W/AR to test_nmu_pkt; pops B/R from nmu_depkt
  nsu_port.tick();     // pops AW/W/AR from nsu_depkt; forwards B/R to nsu_pkt
  slave.tick();        // accepts AW/W/AR, generates B/R
  memory.tick();
  master.tick();       // drives next scenario_txn

  scoreboard.observe();
}
```

**Ordering rationale**: drain depacketize side BEFORE port/slave tick to free queue space; packetize calls happen inside port.tick() and don't need separate scheduling.

Test rig wraps the sticky-setter-per-push pattern via a **`TestPacketize` adapter** (defined in test code only):

```cpp
// c_model/tests/common/test_packetize_adapter.hpp
class TestPacketize : public ni::cmodel::Packetizer {
public:
  TestPacketize(nmu::Packetize& real, uint8_t fixed_dst_id)
    : real_(real), dst_(fixed_dst_id) {}
  bool push_aw(const axi::AwBeat& b) override { real_.set_aw_header_extras(dst_, 0, 0); return real_.push_aw(b); }
  bool push_w (const axi::WBeat&  b) override { return real_.push_w(b); }  // dst from real_'s w_meta_fifo_
  bool push_ar(const axi::ArBeat& b) override { real_.set_ar_header_extras(dst_, 0, 0); return real_.push_ar(b); }
  bool push_b (const axi::BBeat&)  override { assert(false); return false; }
  bool push_r (const axi::RBeat&)  override { assert(false); return false; }
private:
  nmu::Packetize& real_;
  uint8_t dst_;
};
```

`AxiSlavePort` is constructed with a `TestPacketize&` (which IS a `Packetizer&`). Production wiring (the future `nmu::AddrTrans` task) replaces this adapter with a real address-translation layer that computes dst_id from `awaddr`/`araddr`.

**Fixtures replayed (all from Stage 2 `c_model/tests/axi/fixtures/`)**:

1. `burst_incr_8beat.yaml` — basic INCR 8-beat burst, single dst
2. `multi_outstanding_stress.yaml` — multi-outstanding W (validates write-meta FIFO + MetaBuffer)
3. WRAP burst variant — `wrap_*.yaml`
4. Narrow transfer variant — `narrow_*.yaml`
5. Sparse WSTRB variant — `sparse_wstrb_*.yaml`
6. **Delayed-LoopbackNoc variant**: rerun fixture #2 with `req_delay=2, rsp_delay=3` cycles — catches one-cycle timing bugs the zero-latency rig hides

**Pass criterion**: Stage 2 `Scoreboard` reports zero mismatch across all 6 fixtures.

### 7.3 Test fixture YAML location

Reuse the existing `c_model/tests/axi/fixtures/*.yaml` (Stage 2 already covers needed scenarios). No new fixture files unless a coverage gap appears.

---

## 8. Open follow-ups (documented, NOT in this spec scope)

| Item | Owner task | Why deferred |
|---|---|---|
| `nmu::VcArb` + `nsu::VcArb` MUX (3→1) with credit | future task per `plan §3` | Independent module; needs round-robin + credit logic |
| `nmu::VcMapping` (NUM_VC>1) | future task | Trivial today (vc_id=0 hardcoded); useful when NUM_VC>1 |
| `route_par` computation (XOR over `dst_id` + `last`) | future helper | Field 0-filled with `TODO(route_par): see plan §3 / ni_packet.json route_par_coverage` |
| `flit_ecc` computation (SECDED Hamming over FLIT_DATA_WIDTH=398 bits) | future ECC module | Field 0-filled with `TODO(flit_ecc): SECDED Hamming`; standalone algorithm worth its own task |
| `nmu::AddrTrans` (awaddr/araddr → dst_id) | future task per `plan §3` | This spec uses test-helper sticky-setter wrapping; addr_trans replaces it |
| `nmu::Rob` (per-AXI-ID reorder, NoRoB / SimpleRoB / NormalRoB modes) | future task per `plan §3.1` | rob_req/rob_idx default 0; ROB task replaces with allocator |
| `MetaBuffer` atomic-ID tagging | future enhancement | Per-ID FIFO suffices for tested fixtures; atomic tagging is a safety net |
| `Packetizer` interface mis-fit for NSU side (push_aw/w/ar assert-false in nsu::Packetize) | follow-up if pain | Acceptable for now; clean split = `RequestPacketizer` vs `ResponsePacketizer` sub-interfaces |
| `NEXT_STEPS.md` / `docs/noc_cmodel_rtl_plan.md` §6 file-tree alignment update | post-merge housekeeping | Routine doc maintenance |

---

## 9. Karpathy 4-lens self-check

**Overcomplication**:
- Packetize is "stateless + small write-meta FIFO" (Codex Critical (j)). Not pure functional but still very thin.
- Depacketize is `switch(axi_ch)` demux + pending-flit stash. Standard NoC pattern.
- MetaBuffer is per-ID `std::deque`. No fancy data structures.
- LoopbackNoc inner adapters are 4 boilerplate stubs but unavoidable given C++ same-name virtual inheritance rules.
- No ECC, no MUX, no addr_trans, no ROB — all deferred per `plan §3`.

**Surgical**:
- `c_model/include/axi/` untouched (Stage 2 frozen).
- `c_model/include/{nmu,nsu}/{axi_slave_port,axi_master_port}.hpp` untouched (Stage 3 port-pair frozen).
- Only existing-prod-code change: `c_model/include/flit.hpp` deletion + replacement (functional API extension, ~drop-in for the one test that uses it).
- specgen extension is additive: new namespace block + new static_assert; existing constants and structure preserved.

**Surface assumptions** (explicit in spec):
- 1 beat = 1 flit always (every channel payload fits within PAYLOAD_WIDTH=352).
- LoopbackNoc does NOT model 1-flit/cycle physical NoC pacing.
- W beat dst_id/rob_* inherited from its AW via internal write-meta FIFO (NOT caller responsibility).
- Depacketize pending-flit stash creates head-of-line blocking — by design for single-FIFO NoC ingress.
- NSU response packetize is per-AXI-ID FIFO-ordered (matches AXI4 in-order-per-id ordering); cross-ID reorder is ROB's job.
- `route_par` / `flit_ecc` enabled in spec but 0-filled by this implementation (TODO-marked).

**Verifiable success**: Stage 2 `Scoreboard` zero mismatch across 5 fixtures + 1 delayed-loopback variant. All unit tests (~14 NMU packetize + ~12 NMU depacketize + ~12 NSU depacketize + ~14 NSU packetize + ~6 MetaBuffer ≈ ~58 unit + 6 integration = ~64) pass. Drift gates clean (specgen pytest, codegen --check, gen_inventory --check).

---

## 10. Drift gates (every commit)

```bash
cd specgen
py -3 -m pytest -q                                    # specgen tests pass (157 + new per-payload-position tests)
py -3 tools/codegen.py --check                        # byte-identical .h / .sv (post-golden-refresh)
py -3 tools/gen_inventory.py --check                  # FEATURE_INVENTORY clean
cd ../c_model && cmake --build build && ctest --test-dir build -j 1    # all tests pass (216 + ~52 new)
```

Expected final ctest total: ~268.
