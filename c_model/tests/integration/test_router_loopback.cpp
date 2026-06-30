// Task 5 integration: bidirectional loopback over a hand-assembled two-node fabric.
//
// Two full-NI nodes share a single TwoNodeFabric (2-node, 1-hop, full-duplex
// fabric hand-assembled from individual Routers + LOCAL adapters). Each direction
// is one self-contained Flow (master -> NMU -> REQ net -> NSU -> AxiSlave/Memory,
// response back through the RSP net), validated by its own scoreboard:
//
//   Flow A: NMU at node 1, NSU at node 0. Scenario addresses map to dst=(0,0).
//   Flow B: NMU at node 0, NSU at node 1. A +0x100000000 address offset sets
//           bit 32 so addr_trans::xy_route yields dst_id=0x01 -> node (1,0).
//
// The Flow body (master/nmu/nsu construction, the NSU AxiMasterPort <-> AxiSlave
// response shuttle, the b/r holdover deques, and the b_owner_nsu/r_owner_nsu
// per-id owner FIFOs) is the single-NSU pattern from
// test_request_response_loopback.cpp, scoped to this flow's one NSU. The
// per-cycle work is split into pre_tick() (producers that PUSH into the channel)
// and post_tick() (drain channel -> NSU + response shuttle) so the loop drives
// exactly one ch.tick() between them, honoring the §5 inject tick boundary
// (at most one push per inject per ch.tick()).
//
// Pass criterion (per num_vc in {1,2,4,8}): both flows done() and both
// scoreboards report zero mismatch within the cycle cap.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/nmu.hpp"
#include "nmu/port_params.hpp"
#include "router/router.hpp"
#include "router/router_adapters.hpp"
#include "router/two_node_fabric.hpp"
#include "nsu/nsu.hpp"
#include "nsu/port_params.hpp"
#include "scenario_helpers.hpp"
#include "common/scenario.hpp"
#include "common/ni_perf_observer.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace axi = ni::cmodel::axi;
namespace nmu = ni::cmodel::nmu;
namespace nsu = ni::cmodel::nsu;
namespace rc = ni::cmodel::router;  // fabric/addr_trans namespace; ::router::tests is the helper ns
using ni::cmodel::router::testing::TwoNodeFabric;

namespace {

// src_id IS the node's routing identity. A response flit carries dst_id =
// requesting NMU's src_id, and the RSP fabric routes by dst_id (router.hpp:
// dst_x = dst_id & X_MASK, dst_y = dst_id >> X_WIDTH). With y=0 the node index
// equals the dst_id byte, so each NMU's src_id MUST equal its own node index or
// its responses eject at the wrong NMU. (Single-flow tests never hit this: one
// NMU absorbs every response regardless of src_id.)
inline uint8_t node_src_id(std::size_t node) {
    return static_cast<uint8_t>(node);
}

// Resolve the scenario YAML path the same way test_request_response_loopback
// does: SCENARIO_TREE_ROOT + id + /scenario.yaml, guarded by RequireKnownScenario
// so a stale id aborts at startup rather than failing silently.
std::string scenario_path(const char* id) {
    return std::string(SCENARIO_TREE_ROOT) +
           std::string(::router::tests::RequireKnownScenario(id)) + "/scenario.yaml";
}

// AxiMasterT only accepts a YAML path (it re-parses internally), so Flow B's
// +0x100000000 address offset is applied by writing a shifted copy of the scenario
// to TEST_TMPDIR (plan mechanism 3: ScenarioConfig/AxiMaster expose no offset
// field, and the master owns its own parse so an in-memory mutation would not
// reach it). data_file/dump_file are rewritten to absolute paths so they still
// resolve from the temp directory. memory_base is shifted by the same offset so
// the NSU's Memory covers the (unmodified) local_addr the NSU emits on the wire
// (addr_trans keeps local_addr == addr).
std::string shifted_scenario_path(const std::string& base_yaml, uint64_t offset, uint8_t tag) {
    auto sc = axi::load_scenario(base_yaml);
    std::string out = std::string(::testing::TempDir()) + "/router_loopback_shift_0x" +
                      std::to_string(offset) + "_n" + std::to_string(tag) + ".yaml";
    std::ofstream f(out);
    f << "schema_version: 1\n";
    f << "metadata:\n";
    f << "  name: " << sc.metadata.name << "\n";
    f << "  category: " << sc.metadata.category << "\n";
    f << "config:\n";
    f << "  memory_base: 0x" << std::hex << (sc.config.memory_base + offset) << std::dec << "\n";
    f << "  memory_size: " << sc.config.memory_size << "\n";
    f << "  write_latency: " << sc.config.write_latency << "\n";
    f << "  read_latency: " << sc.config.read_latency << "\n";
    f << "transactions:\n";
    for (const auto& t : sc.transactions) {
        f << "  - op: " << (t.op == axi::ScenarioTransaction::Op::Write ? "write" : "read") << "\n";
        f << "    addr: 0x" << std::hex << (t.addr + offset) << std::dec << "\n";
        f << "    id: 0x" << std::hex << static_cast<unsigned>(t.id) << std::dec << "\n";
        f << "    len: " << static_cast<unsigned>(t.len) << "\n";
        f << "    size: " << static_cast<unsigned>(t.size) << "\n";
        const char* burst = (t.burst == axi::Burst::INCR)
                                ? "INCR"
                                : (t.burst == axi::Burst::WRAP ? "WRAP" : "FIXED");
        f << "    burst: " << burst << "\n";
        if (t.op == axi::ScenarioTransaction::Op::Write)
            f << "    data_file: " << t.data_file << "\n";  // already absolute (resolved on load)
        else
            f << "    dump_file: " << (t.dump_file.empty() ? std::string("unused") : t.dump_file)
              << "\n";
    }
    return out;
}

// One direction's full datapath + oracle. Mirrors a single (NMU, NSU=1) stack
// from test_request_response_loopback. Non-copyable / non-movable: Nmu and Nsu
// have deleted move ctors and routers hold raw refs, so each Flow is built
// in-place at the test scope.
struct Flow {
    // master_node hosts the NMU + AXI master (traffic source); slave_node hosts
    // the NSU + AxiSlave/Memory (the responder). For Flow A: 1 -> 0. For Flow B:
    // 0 -> 1 with a +0x100000000 address offset so dst_id resolves to slave_node.
    // 7-arg constructor: binds NMU/NSU to raw fabric adapters (default usage).
    Flow(TwoNodeFabric& ch, std::size_t master_node, std::size_t slave_node,
         const std::string& yaml_path, std::size_t num_vc, const std::string& read_dump,
         uint8_t expected_dst)
        : sc_(axi::load_scenario(yaml_path)),
          mem_(sc_.config.memory_base, sc_.config.memory_size, sc_.config.write_latency,
               sc_.config.read_latency),
          slave_(mem_),
          p_req_out_(&ch.nmu_req_out(master_node)),
          p_rsp_in_(&ch.nmu_rsp_in(master_node)),
          p_req_in_(&ch.nsu_req_in(slave_node)),
          p_rsp_out_(&ch.nsu_rsp_out(slave_node)),
          nmu_(make_nmu_cfg(num_vc, master_node), *p_req_out_, *p_rsp_in_),
          nsu_(make_nsu_cfg(num_vc, slave_node), *p_req_in_, *p_rsp_out_),
          master_(yaml_path, nmu_.axi_slave_port(), read_dump, sc_.config.max_outstanding_write,
                  sc_.config.max_outstanding_read),
          expected_dst_(expected_dst),
          master_node_(master_node) {
        init_common();
    }

    // 11-arg constructor: binds NMU/NSU to caller-supplied NI-edge interfaces
    // (probe-wrapped or raw). Use when flit logs must capture NI-edge crossings.
    Flow(TwoNodeFabric& ch, std::size_t master_node, std::size_t slave_node,
         const std::string& yaml_path, std::size_t num_vc, const std::string& read_dump,
         uint8_t expected_dst, rc::NocReqOut& noc_req_out, rc::NocRspIn& noc_rsp_in,
         rc::NocReqIn& noc_req_in, rc::NocRspOut& noc_rsp_out)
        : sc_(axi::load_scenario(yaml_path)),
          mem_(sc_.config.memory_base, sc_.config.memory_size, sc_.config.write_latency,
               sc_.config.read_latency),
          slave_(mem_),
          p_req_out_(&noc_req_out),
          p_rsp_in_(&noc_rsp_in),
          p_req_in_(&noc_req_in),
          p_rsp_out_(&noc_rsp_out),
          nmu_(make_nmu_cfg(num_vc, master_node), *p_req_out_, *p_rsp_in_),
          nsu_(make_nsu_cfg(num_vc, slave_node), *p_req_in_, *p_rsp_out_),
          master_(yaml_path, nmu_.axi_slave_port(), read_dump, sc_.config.max_outstanding_write,
                  sc_.config.max_outstanding_read),
          expected_dst_(expected_dst),
          master_node_(master_node) {
        init_common();
    }

    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;

    void set_on_slave_req_beat(std::function<void(uint8_t)> cb) {
        on_slave_req_beat_ = std::move(cb);
    }
    void set_on_slave_rsp_beat(std::function<void(uint8_t)> cb) {
        on_slave_rsp_beat_ = std::move(cb);
    }

    // Producer half: master/nmu/nsu ticks that PUSH into the channel.
    void pre_tick() {
        master_.tick();
        nmu_.tick();
        nsu_.tick();

        // Shuttle requests from the NSU AxiMasterPort downstream face into the
        // backing AxiSlave (single-NSU scope of the existing test's shuttle).
        auto& port = nsu_.axi_master_port();
        while (auto aw = port.pop_aw()) {
            uint8_t id = aw->id;
            if (!slave_.push_aw(*aw)) {
                ADD_FAILURE() << "AxiSlave rejected AW push; queue sizing mismatch";
                break;
            }
            b_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(0);
        }
        while (auto w = port.pop_w()) {
            if (!slave_.push_w(*w)) {
                ADD_FAILURE() << "AxiSlave rejected W push; queue sizing mismatch";
                break;
            }
            if (w->last && on_slave_req_beat_) on_slave_req_beat_(1);
        }
        while (auto ar = port.pop_ar()) {
            uint8_t id = ar->id;
            if (!slave_.push_ar(*ar)) {
                ADD_FAILURE() << "AxiSlave rejected AR push; queue sizing mismatch";
                break;
            }
            r_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(2);
        }

        slave_.tick();
        mem_.tick();
    }

    // Consumer half: route AxiSlave responses back into the NSU AxiMasterPort
    // upstream face. Drain holdovers first, then pull from the slave. Single-NSU
    // scope, so b_owner_/r_owner_ entries are bookkeeping placeholders (one NSU)
    // kept verbatim from the existing per-id owner-tracking shuttle.
    void post_tick() {
        auto& port = nsu_.axi_master_port();
        while (!b_holdover_.empty()) {
            if (!port.push_b(b_holdover_.front())) break;
            b_holdover_.pop_front();
        }
        while (!r_holdover_.empty()) {
            if (!port.push_r(r_holdover_.front())) break;
            r_holdover_.pop_front();
        }

        while (auto b = slave_.pop_b()) {
            uint8_t id = b->id;
            if (b_owner_[id].empty()) {
                ADD_FAILURE() << "B beat for id=" << static_cast<int>(id)
                              << " with no outstanding AW owner (FIFO empty)";
                break;
            }
            b_owner_[id].pop_front();
            if (!b_holdover_.empty() || !port.push_b(*b)) {
                b_holdover_.push_back(*b);
            } else {
                if (on_slave_rsp_beat_) on_slave_rsp_beat_(3);
            }
        }
        while (auto r = slave_.pop_r()) {
            uint8_t id = r->id;
            if (r_owner_[id].empty()) {
                ADD_FAILURE() << "R beat for id=" << static_cast<int>(id)
                              << " with no outstanding AR owner (FIFO empty)";
                break;
            }
            if (r->last) {
                r_owner_[id].pop_front();
            }
            if (!r_holdover_.empty() || !port.push_r(*r)) {
                r_holdover_.push_back(*r);
            } else {
                if (r->last && on_slave_rsp_beat_) on_slave_rsp_beat_(4);
            }
        }
    }

    bool done() const { return master_.done(); }
    std::size_t mismatches() const { return sb_.mismatch_count(); }

    // Observer wiring accessors.
    axi::AxiMasterT<nmu::AxiSlavePort>& master() { return master_; }
    const nmu::Rob& rob() const { return nmu_.rob(); }
    uint8_t node_index() const { return static_cast<uint8_t>(master_node_); }

  private:
    static nmu::NmuConfig make_nmu_cfg(std::size_t num_vc, std::size_t node) {
        nmu::NmuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.write_rob_mode = nmu::RobMode::Disabled;
        cfg.read_rob_mode = nmu::RobMode::Disabled;
        cfg.port_params = nmu::load_nmu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.write_vc = 0;
        cfg.read_vc = (num_vc >= 2) ? 1u : 0u;
        return cfg;
    }
    static nsu::NsuConfig make_nsu_cfg(std::size_t num_vc, std::size_t node) {
        nsu::NsuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.port_params = nsu::load_nsu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.write_rsp_vc = 0;
        cfg.read_rsp_vc = (num_vc >= 2) ? 1u : 0u;
        return cfg;
    }

    void init_common() {
        slave_.set_memory_bounds(sc_.config.memory_base, sc_.config.memory_size);
        master_.on_write_completed([this](const axi::WriteResult& wr) {
            sb_.handle_write_completed(wr, wr.data, wr.strb_per_beat);
        });
        master_.on_read_observed(
            [this](const axi::ReadResult& rr) { sb_.handle_read_observed(rr); });

        // Sanity: a request from this flow must route to the expected node.
        // addr_trans::xy_route is the same derivation the NMU packetizer uses, so
        // a wrong offset (or a topology mismatch) fails loudly here instead of
        // silently routing to the wrong NSU and deadlocking.
        EXPECT_EQ(nmu::addr_trans::xy_route(sc_.transactions.front().addr).dst_id, expected_dst_)
            << "flow request dst_id mismatch (master_node=" << master_node_ << ")";
    }

    axi::Scenario sc_;
    axi::Memory mem_;
    axi::AxiSlave slave_;
    // NI-edge interface pointers (set by ctors; non-null before nmu_/nsu_ init).
    rc::NocReqOut* p_req_out_ = nullptr;
    rc::NocRspIn* p_rsp_in_ = nullptr;
    rc::NocReqIn* p_req_in_ = nullptr;
    rc::NocRspOut* p_rsp_out_ = nullptr;
    nmu::Nmu nmu_;
    nsu::Nsu nsu_;
    axi::AxiMasterT<nmu::AxiSlavePort> master_;
    axi::Scoreboard sb_;
    uint8_t expected_dst_;
    std::size_t master_node_;

    std::function<void(uint8_t)> on_slave_req_beat_;
    std::function<void(uint8_t)> on_slave_rsp_beat_;

    std::deque<axi::BBeat> b_holdover_;
    std::deque<axi::RBeat> r_holdover_;
    std::array<std::deque<std::size_t>, 256> b_owner_;
    std::array<std::deque<std::size_t>, 256> r_owner_;
};

constexpr std::size_t kCycleCap = 100'000;

}  // namespace

class RouterLoopbackParam : public ::testing::TestWithParam<std::size_t> {};  // num_vc

TEST_P(RouterLoopbackParam, BidirectionalZeroMismatch) {
    SCENARIO("TwoNodeFabric: two simultaneous flows (1,0)<->(0,0), both scoreboards clean");
    const std::size_t num_vc = GetParam();
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));

    const std::string base = scenario_path("AX4-BAS-001_single_write_read_aligned");
    const std::string tmp = std::string(::testing::TempDir());
    const std::string rpath_a =
        tmp + "/router_loopback_a_vc" + std::to_string(num_vc) + ".read.txt";
    const std::string rpath_b =
        tmp + "/router_loopback_b_vc" + std::to_string(num_vc) + ".read.txt";

    // Flow A: master at node 1, slave at node 0; addresses map to dst=(0,0)=0x00.
    Flow flow_a(ch, /*master_node=*/1, /*slave_node=*/0, base, num_vc, rpath_a, /*dst=*/0x00);
    // Flow B: master at node 0, slave at node 1; +0x100000000 sets bit 32 -> dst=0x01.
    const std::string yaml_b = shifted_scenario_path(base, 0x100000000, /*tag=*/num_vc);
    Flow flow_b(ch, /*master_node=*/0, /*slave_node=*/1, yaml_b, num_vc, rpath_b, /*dst=*/0x01);

    std::size_t cycle = 0;
    while ((!flow_a.done() || !flow_b.done()) && cycle < kCycleCap) {
        flow_a.pre_tick();
        flow_b.pre_tick();
        ch.tick();
        flow_a.post_tick();
        flow_b.post_tick();
        ++cycle;
    }

    EXPECT_TRUE(flow_a.done()) << "flow A did not complete (num_vc=" << num_vc << ")";
    EXPECT_TRUE(flow_b.done()) << "flow B did not complete (num_vc=" << num_vc << ")";
    EXPECT_EQ(flow_a.mismatches(), 0u) << "flow A scoreboard mismatch (num_vc=" << num_vc << ")";
    EXPECT_EQ(flow_b.mismatches(), 0u) << "flow B scoreboard mismatch (num_vc=" << num_vc << ")";
    EXPECT_LT(cycle, kCycleCap) << "cycle cap hit (num_vc=" << num_vc << ")";
}

INSTANTIATE_TEST_SUITE_P(NumVc, RouterLoopbackParam, ::testing::Values(1, 2, 4, 8),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "vc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// Non-intrusive A/B test: observers must not perturb cycle-to-completion.
// Runs the bidirectional num_vc=1 fabric twice (plain, then with observers
// attached) and asserts identical cycle count and zero-mismatch scoreboards.
// ---------------------------------------------------------------------------

namespace {

std::size_t run_loopback(bool with_observers) {
    using namespace ni::cmodel::testing;
    const std::size_t num_vc = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string base = scenario_path("AX4-BAS-001_single_write_read_aligned");
    const std::string tmp = std::string(::testing::TempDir());
    Flow flow_a(ch, /*master_node=*/1, /*slave_node=*/0, base, num_vc, tmp + "/ab_a.read.txt",
                /*dst=*/0x00);
    const std::string yaml_b = shifted_scenario_path(base, 0x100000000, /*tag=*/42);
    Flow flow_b(ch, /*master_node=*/0, /*slave_node=*/1, yaml_b, num_vc, tmp + "/ab_b.read.txt",
                /*dst=*/0x01);

    uint64_t now = 0;
    NIPerfObserver ni_a(now, "A");
    NIPerfObserver ni_b(now, "B");

    if (with_observers) {
        flow_a.master().on_write_issued(
            [&](const axi::IssueInfo& i) { ni_a.on_issue(true, i.scenario_line); });
        flow_a.master().on_read_issued(
            [&](const axi::IssueInfo& i) { ni_a.on_issue(false, i.scenario_line); });
        flow_a.master().on_write_completed(
            [&](const axi::WriteResult& w) { ni_a.on_complete(true, w.scenario_line); });
        flow_a.master().on_read_observed(
            [&](const axi::ReadResult& r) { ni_a.on_complete(false, r.scenario_line); });
        flow_b.master().on_write_issued(
            [&](const axi::IssueInfo& i) { ni_b.on_issue(true, i.scenario_line); });
        flow_b.master().on_read_issued(
            [&](const axi::IssueInfo& i) { ni_b.on_issue(false, i.scenario_line); });
        flow_b.master().on_write_completed(
            [&](const axi::WriteResult& w) { ni_b.on_complete(true, w.scenario_line); });
        flow_b.master().on_read_observed(
            [&](const axi::ReadResult& r) { ni_b.on_complete(false, r.scenario_line); });
    }

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while ((!flow_a.done() || !flow_b.done()) && cycle < cap) {
        now = cycle;
        flow_a.pre_tick();
        flow_b.pre_tick();
        ch.tick();
        flow_a.post_tick();
        flow_b.post_tick();
        ++cycle;
    }
    EXPECT_EQ(flow_a.mismatches(), 0u);
    EXPECT_EQ(flow_b.mismatches(), 0u);
    return cycle;
}

}  // namespace

TEST(RouterLoopbackPerf, ObserversAreNonIntrusive) {
    const std::size_t plain = run_loopback(/*with_observers=*/false);
    const std::size_t obs = run_loopback(/*with_observers=*/true);
    EXPECT_EQ(plain, obs) << "attaching observers changed cycle-to-completion (plain=" << plain
                          << " obs=" << obs << ")";
}
