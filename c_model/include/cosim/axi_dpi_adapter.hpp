// AxiDpiAdapter: single owner of the C-model integration loop exposed as a
// DPI service.
//
// Owns: LoopbackNoc, Nmu, Nsu (single instance), AxiSlave, Memory,
// AxiMasterT<nmu::AxiSlavePort>, Scoreboard.
//
// Wiring mirrors test_request_response_loopback.cpp (single-NSU, non-multi_dst
// path) verbatim. Do NOT change the member construction order — it matches the
// ref dependency chain documented in nmu/nmu.hpp and nsu/nsu.hpp.
//
// init(yaml) loads the scenario, instantiates all components, wires them, and
// sets up the scoreboard callbacks exactly as the integration testbench does.
// tick() drives one simulation cycle. done() / scoreboard_clean() expose the
// terminal condition for the DPI bridge layer (Task 5+).
//
// Per-channel pin snapshot getters expose pin state captured mid-tick:
//
//   NMU AW/W/AR: captured after master_->tick() (master drives the channel)
//     and before nmu_->tick() (NMU consumes the channel). This models the
//     RTL cycle: master drives AWVALID+beat; slave (NMU) processes on clock
//     edge. valid = beat present in port queue at that instant.
//
//   NMU B/R: captured after nmu_->tick() (NMU depacketizes into port queues)
//     and before master_->tick() (master drains via pop_b/pop_r). valid =
//     response beat available.
//
//   NSU AW/W/AR: captured after nsu_->tick() drains from depacketizer.
//     valid = depacketized beat ready for the AXI slave.
//
//   NSU B/R: captured after slave responses are pushed back into NSU port.
//     valid = response beat present.
//
// ready is always true in this FIFO model — no combinatorial backpressure.
#pragma once
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/loopback_noc.hpp"
#include "cosim/pin_snapshot.hpp"
#include "nmu/nmu.hpp"
#include "nsu/nsu.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ni::cmodel::cosim {

class AxiDpiAdapter {
  public:
    AxiDpiAdapter() = default;
    ~AxiDpiAdapter() = default;
    AxiDpiAdapter(const AxiDpiAdapter&) = delete;
    AxiDpiAdapter(AxiDpiAdapter&&) = delete;
    AxiDpiAdapter& operator=(const AxiDpiAdapter&) = delete;
    AxiDpiAdapter& operator=(AxiDpiAdapter&&) = delete;

    // Load scenario and instantiate all components. Must be called once before
    // tick(). Throws std::runtime_error on YAML parse failure or bad path.
    void init(const std::string& scenario_yaml_path);

    // Advance simulation by one cycle. Mirrors the per-cycle body of
    // run_fixture() in test_request_response_loopback.cpp (single-NSU path).
    // Updates the internal pin snapshot cache after each sub-step so that the
    // get_* getters return the state captured at the canonical sample point.
    void tick();

    // True when the AxiMaster has drained all transactions (all B/R responses
    // received). Same predicate as AxiMasterT::done().
    bool done() const;

    // True when the Scoreboard has recorded zero mismatches.
    bool scoreboard_clean() const;

    // Per-channel NMU AXI boundary pin snapshots.
    void get_nmu_aw(AwPins& out) const { out = snapshot_.nmu_aw; }
    void get_nmu_w(WPins& out) const { out = snapshot_.nmu_w; }
    void get_nmu_ar(ArPins& out) const { out = snapshot_.nmu_ar; }
    void get_nmu_b(BPins& out) const { out = snapshot_.nmu_b; }
    void get_nmu_r(RPins& out) const { out = snapshot_.nmu_r; }

    // Per-channel NSU AXI boundary pin snapshots.
    void get_nsu_aw(AwPins& out) const { out = snapshot_.nsu_aw; }
    void get_nsu_w(WPins& out) const { out = snapshot_.nsu_w; }
    void get_nsu_ar(ArPins& out) const { out = snapshot_.nsu_ar; }
    void get_nsu_b(BPins& out) const { out = snapshot_.nsu_b; }
    void get_nsu_r(RPins& out) const { out = snapshot_.nsu_r; }

  private:
    // Fixed config constants mirroring the single-NSU legacy path in
    // test_request_response_loopback.cpp. Rob disabled (no multi_dst here).
    static constexpr uint8_t kNmuSrcId = 0x01;
    static constexpr uint8_t kNsuSrcId = 0x02;
    static constexpr std::size_t kNumVc = 1;

    // Per-cycle pin state cache. Updated during tick() at the canonical sample
    // points (see file-level comment). Returned directly by the get_* getters.
    struct CycleSnapshot {
        AwPins nmu_aw{};
        WPins nmu_w{};
        ArPins nmu_ar{};
        BPins nmu_b{};
        RPins nmu_r{};
        AwPins nsu_aw{};
        WPins nsu_w{};
        ArPins nsu_ar{};
        BPins nsu_b{};
        RPins nsu_r{};
    };
    CycleSnapshot snapshot_{};

    // Component ownership. Construction order in init() must match the ref
    // dependency chain (LoopbackNoc → Nmu/Nsu → AxiSlave/Memory → AxiMaster).
    std::unique_ptr<testing::LoopbackNoc> loopback_;
    std::unique_ptr<nmu::Nmu> nmu_;
    std::unique_ptr<nsu::Nsu> nsu_;
    std::unique_ptr<axi::Memory> mem_;
    std::unique_ptr<axi::AxiSlave> slave_;
    std::unique_ptr<axi::AxiMasterT<nmu::AxiSlavePort>> master_;
    axi::Scoreboard scoreboard_;

    // Per-cycle B/R holdover queues (single NSU).
    std::deque<axi::BBeat> b_holdover_;
    std::deque<axi::RBeat> r_holdover_;

    // Per-AXI-ID FIFO of NSU index for B/R routing.
    std::array<std::deque<std::size_t>, 256> b_owner_nsu_;
    std::array<std::deque<std::size_t>, 256> r_owner_nsu_;

    // Snapshot helpers — convert port queue front to PinSnapshot POD.
    static void capture_nmu_aw_(AwPins& out, const nmu::AxiSlavePort& port);
    static void capture_nmu_w_(WPins& out, const nmu::AxiSlavePort& port);
    static void capture_nmu_ar_(ArPins& out, const nmu::AxiSlavePort& port);
    static void capture_nmu_b_(BPins& out, const nmu::AxiSlavePort& port);
    static void capture_nmu_r_(RPins& out, const nmu::AxiSlavePort& port);
    static void capture_nsu_aw_(AwPins& out, const nsu::AxiMasterPort& port);
    static void capture_nsu_w_(WPins& out, const nsu::AxiMasterPort& port);
    static void capture_nsu_ar_(ArPins& out, const nsu::AxiMasterPort& port);
    static void capture_nsu_b_(BPins& out, const nsu::AxiMasterPort& port);
    static void capture_nsu_r_(RPins& out, const nsu::AxiMasterPort& port);
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline void AxiDpiAdapter::init(const std::string& scenario_yaml_path) {
    auto sc = axi::load_scenario(scenario_yaml_path);

    // Memory + slave
    mem_ = std::make_unique<axi::Memory>(sc.config.memory_base, sc.config.memory_size,
                                         sc.config.write_latency, sc.config.read_latency);
    slave_ = std::make_unique<axi::AxiSlave>(*mem_);
    slave_->set_memory_bounds(sc.config.memory_base, sc.config.memory_size);

    // Port params from config/port_params.yaml (relative to binary CWD).
    auto params = load_port_params_yaml("config/port_params.yaml", "nmu");

    // Single-NSU LoopbackNoc (legacy ctor; all dst_id route to NSU_0).
    loopback_ = std::make_unique<testing::LoopbackNoc>(params.loopback_noc_req_depth,
                                                       params.loopback_noc_rsp_depth);

    // NMU: Rob disabled (no multi_dst reorder needed for single-NSU PoC).
    nmu::NmuConfig nmu_cfg{};
    nmu_cfg.src_id = kNmuSrcId;
    nmu_cfg.write_rob_mode = nmu::RobMode::Disabled;
    nmu_cfg.read_rob_mode = nmu::RobMode::Disabled;
    nmu_cfg.port_params = params;
    nmu_cfg.depkt_b_q_depth = params.depkt_b_q_depth;
    nmu_cfg.depkt_r_q_depth = params.depkt_r_q_depth;
    nmu_cfg.num_vc = kNumVc;
    nmu_cfg.vc_mode = nmu::VcMode::ReadWriteSplit;
    nmu_cfg.write_vc = 0;
    nmu_cfg.read_vc = 0;
    nmu_ = std::make_unique<nmu::Nmu>(nmu_cfg, loopback_->nmu_req_out(), loopback_->nmu_rsp_in());

    // NSU (single instance).
    nsu::NsuConfig nsu_cfg{};
    nsu_cfg.src_id = kNsuSrcId;
    nsu_cfg.port_params = params;
    nsu_cfg.meta_buffer_per_id_depth = params.meta_buffer_per_id_depth;
    nsu_cfg.depkt_aw_q_depth = params.depkt_aw_q_depth;
    nsu_cfg.depkt_w_q_depth = params.depkt_w_q_depth;
    nsu_cfg.depkt_ar_q_depth = params.depkt_ar_q_depth;
    nsu_cfg.num_vc = kNumVc;
    nsu_cfg.vc_mode = nsu::VcMode::ReadWriteSplit;
    nsu_cfg.write_rsp_vc = 0;
    nsu_cfg.read_rsp_vc = 0;
    nsu_ = std::make_unique<nsu::Nsu>(nsu_cfg, loopback_->nsu_req_in(0), loopback_->nsu_rsp_out(0));

    // AxiMaster binds to NMU's AxiSlavePort. read_dump goes to a temp file.
    const std::string read_dump_path = "axi_dpi_adapter_read_dump.tmp";
    master_ = std::make_unique<axi::AxiMasterT<nmu::AxiSlavePort>>(
        scenario_yaml_path, nmu_->axi_slave_port(), read_dump_path, sc.config.max_outstanding_write,
        sc.config.max_outstanding_read);

    // Scoreboard callbacks (exact copy of integration testbench wiring).
    master_->on_write_completed([this](const axi::WriteResult& wr) {
        scoreboard_.handle_write_completed(wr, wr.data, wr.strb_per_beat);
    });
    master_->on_read_observed(
        [this](const axi::ReadResult& rr) { scoreboard_.handle_read_observed(rr); });

    // Reset holdovers and routing tables.
    b_holdover_.clear();
    r_holdover_.clear();
    for (auto& dq : b_owner_nsu_) dq.clear();
    for (auto& dq : r_owner_nsu_) dq.clear();
    snapshot_ = CycleSnapshot{};
}

inline void AxiDpiAdapter::tick() {
    if (!master_) {
        throw std::runtime_error("AxiDpiAdapter::tick() called before init()");
    }

    // Step 1: AxiMaster pushes requests into NMU AxiSlavePort.
    //         Capture NMU AW/W/AR snapshot here — this is the sample point
    //         where the master has driven AWVALID/WVALID/ARVALID.
    master_->tick();
    capture_nmu_aw_(snapshot_.nmu_aw, nmu_->axi_slave_port());
    capture_nmu_w_(snapshot_.nmu_w, nmu_->axi_slave_port());
    capture_nmu_ar_(snapshot_.nmu_ar, nmu_->axi_slave_port());

    // Step 2: NMU processes one cycle (drains port queues into packetizer,
    //         drains depacketizer output into port queues).
    nmu_->tick();

    // Step 3: Capture NMU B/R snapshot — NMU depacketizer has now produced
    //         response beats into the port's b_q_/r_q_.
    capture_nmu_b_(snapshot_.nmu_b, nmu_->axi_slave_port());
    capture_nmu_r_(snapshot_.nmu_r, nmu_->axi_slave_port());

    // Step 4: NSU processes one cycle.
    nsu_->tick();

    // Step 5: Shuttle requests from NSU AxiMasterPort downstream face into
    //         the slave. Capture NSU AW/W/AR snapshot first (what NSU output
    //         looks like before the slave drains it).
    auto& nsu_port = nsu_->axi_master_port();
    capture_nsu_aw_(snapshot_.nsu_aw, nsu_port);
    capture_nsu_w_(snapshot_.nsu_w, nsu_port);
    capture_nsu_ar_(snapshot_.nsu_ar, nsu_port);

    while (auto aw = nsu_port.pop_aw()) {
        uint8_t id = aw->id;
        if (slave_->push_aw(*aw)) {
            b_owner_nsu_[id].push_back(0);
        }
    }
    while (auto w = nsu_port.pop_w()) {
        slave_->push_w(*w);
    }
    while (auto ar = nsu_port.pop_ar()) {
        uint8_t id = ar->id;
        if (slave_->push_ar(*ar)) {
            r_owner_nsu_[id].push_back(0);
        }
    }

    // Step 6: Slave + memory tick.
    slave_->tick();
    mem_->tick();

    // Step 7: Drain B/R holdovers and route slave responses back to the NSU.
    while (!b_holdover_.empty()) {
        if (!nsu_port.push_b(b_holdover_.front())) break;
        b_holdover_.pop_front();
    }
    while (!r_holdover_.empty()) {
        if (!nsu_port.push_r(r_holdover_.front())) break;
        r_holdover_.pop_front();
    }

    while (auto b = slave_->pop_b()) {
        uint8_t id = b->id;
        if (!b_owner_nsu_[id].empty()) {
            b_owner_nsu_[id].pop_front();
        }
        if (!b_holdover_.empty() || !nsu_port.push_b(*b)) {
            b_holdover_.push_back(*b);
        }
    }
    while (auto r = slave_->pop_r()) {
        uint8_t id = r->id;
        if (!r_owner_nsu_[id].empty() && r->last) {
            r_owner_nsu_[id].pop_front();
        }
        if (!r_holdover_.empty() || !nsu_port.push_r(*r)) {
            r_holdover_.push_back(*r);
        }
    }

    // Step 8: Capture NSU B/R snapshot — slave has now pushed responses in.
    capture_nsu_b_(snapshot_.nsu_b, nsu_port);
    capture_nsu_r_(snapshot_.nsu_r, nsu_port);

    // Step 9: Advance LoopbackNoc delay pipes after all producers/consumers.
    loopback_->tick();
}

inline bool AxiDpiAdapter::done() const {
    if (!master_) return false;
    return master_->done();
}

inline bool AxiDpiAdapter::scoreboard_clean() const {
    return scoreboard_.mismatch_count() == 0;
}

// ---------------------------------------------------------------------------
// Snapshot helpers
// ---------------------------------------------------------------------------

inline void AxiDpiAdapter::capture_nmu_aw_(AwPins& out, const nmu::AxiSlavePort& port) {
    out = AwPins{};
    auto b = port.peek_aw();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.addr = b->addr;
    out.len = b->len;
    out.size = b->size;
    out.burst = static_cast<uint8_t>(b->burst);
    out.lock = b->lock;
    out.cache = b->cache;
    out.prot = b->prot;
    out.qos = b->qos;
}

inline void AxiDpiAdapter::capture_nmu_w_(WPins& out, const nmu::AxiSlavePort& port) {
    out = WPins{};
    auto b = port.peek_w();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.data = b->data;
    out.strb = b->strb;
    out.last = b->last;
}

inline void AxiDpiAdapter::capture_nmu_ar_(ArPins& out, const nmu::AxiSlavePort& port) {
    out = ArPins{};
    auto b = port.peek_ar();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.addr = b->addr;
    out.len = b->len;
    out.size = b->size;
    out.burst = static_cast<uint8_t>(b->burst);
    out.lock = b->lock;
    out.cache = b->cache;
    out.prot = b->prot;
    out.qos = b->qos;
}

inline void AxiDpiAdapter::capture_nmu_b_(BPins& out, const nmu::AxiSlavePort& port) {
    out = BPins{};
    auto b = port.peek_b();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.resp = static_cast<uint8_t>(b->resp);
}

inline void AxiDpiAdapter::capture_nmu_r_(RPins& out, const nmu::AxiSlavePort& port) {
    out = RPins{};
    auto b = port.peek_r();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.data = b->data;
    out.resp = static_cast<uint8_t>(b->resp);
    out.last = b->last;
}

inline void AxiDpiAdapter::capture_nsu_aw_(AwPins& out, const nsu::AxiMasterPort& port) {
    out = AwPins{};
    auto b = port.peek_aw();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.addr = b->addr;
    out.len = b->len;
    out.size = b->size;
    out.burst = static_cast<uint8_t>(b->burst);
    out.lock = b->lock;
    out.cache = b->cache;
    out.prot = b->prot;
    out.qos = b->qos;
}

inline void AxiDpiAdapter::capture_nsu_w_(WPins& out, const nsu::AxiMasterPort& port) {
    out = WPins{};
    auto b = port.peek_w();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.data = b->data;
    out.strb = b->strb;
    out.last = b->last;
}

inline void AxiDpiAdapter::capture_nsu_ar_(ArPins& out, const nsu::AxiMasterPort& port) {
    out = ArPins{};
    auto b = port.peek_ar();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.addr = b->addr;
    out.len = b->len;
    out.size = b->size;
    out.burst = static_cast<uint8_t>(b->burst);
    out.lock = b->lock;
    out.cache = b->cache;
    out.prot = b->prot;
    out.qos = b->qos;
}

inline void AxiDpiAdapter::capture_nsu_b_(BPins& out, const nsu::AxiMasterPort& port) {
    out = BPins{};
    auto b = port.peek_b();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.resp = static_cast<uint8_t>(b->resp);
}

inline void AxiDpiAdapter::capture_nsu_r_(RPins& out, const nsu::AxiMasterPort& port) {
    out = RPins{};
    auto b = port.peek_r();
    if (!b) return;
    out.valid = true;
    out.ready = true;
    out.id = b->id;
    out.data = b->data;
    out.resp = static_cast<uint8_t>(b->resp);
    out.last = b->last;
}

}  // namespace ni::cmodel::cosim
