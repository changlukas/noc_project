// Dedicated perf harness: path-driven, mesh-agnostic per-component decomposition
// over the 2-node fabric. Migrated + generalized from test_router_loopback.cpp's
// two-pass perf block. Drives the canonical AX4-BAS-003 node 0 -> node 1 flow.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/nmu.hpp"
#include "nmu/port_params.hpp"
#include "noc/router.hpp"
#include "noc/router_adapters.hpp"
#include "noc/two_node_fabric.hpp"
#include "nsu/nsu.hpp"
#include "nsu/port_params.hpp"
#include "scenario_helpers.hpp"
#include "common/component_dwell_observer.hpp"
#include "common/flit_link_probe.hpp"
#include "common/isolated_scenario.hpp"
#include "common/ni_perf_observer.hpp"
#include "common/perf_report.hpp"
#include "common/router_path.hpp"
#include "common/scenario.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace axi = ni::cmodel::axi;
namespace nmu = ni::cmodel::nmu;
namespace nsu = ni::cmodel::nsu;
namespace rc = ni::cmodel::noc;
using ni::cmodel::noc::testing::TwoNodeFabric;

namespace {

inline uint8_t node_src_id(std::size_t node) {
    return static_cast<uint8_t>(node);
}

std::string scenario_path(const char* id) {
    return std::string(SCENARIO_TREE_ROOT) + std::string(::noc::tests::RequireKnownScenario(id)) +
           "/scenario.yaml";
}

std::string router_name(ni::cmodel::testing::NodeCoord c) {
    return "R(" + std::to_string(c.x) + "," + std::to_string(c.y) + ")";
}

// One direction's full datapath + oracle. 11-arg ctor binds NMU/NSU to
// caller-supplied (probe-wrapped) NI-edge interfaces so NI-edge crossings are
// captured. Copied verbatim from test_router_loopback.cpp (both are
// testbench-only; the two TUs do not share the helper).
struct Flow {
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

    void pre_tick() {
        master_.tick();
        nmu_.tick();
        nsu_.tick();
        auto& port = nsu_.axi_master_port();
        while (auto aw = port.pop_aw()) {
            uint8_t id = aw->id;
            if (!slave_.push_aw(*aw)) {
                ADD_FAILURE() << "AxiSlave rejected AW push";
                break;
            }
            b_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(0);
        }
        while (auto w = port.pop_w()) {
            if (!slave_.push_w(*w)) {
                ADD_FAILURE() << "AxiSlave rejected W push";
                break;
            }
            if (w->last && on_slave_req_beat_) on_slave_req_beat_(1);
        }
        while (auto ar = port.pop_ar()) {
            uint8_t id = ar->id;
            if (!slave_.push_ar(*ar)) {
                ADD_FAILURE() << "AxiSlave rejected AR push";
                break;
            }
            r_owner_[id].push_back(0);
            if (on_slave_req_beat_) on_slave_req_beat_(2);
        }
        slave_.tick();
        mem_.tick();
    }

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
                ADD_FAILURE() << "B beat no owner";
                break;
            }
            b_owner_[id].pop_front();
            if (!b_holdover_.empty() || !port.push_b(*b)) {
                b_holdover_.push_back(*b);
            } else if (on_slave_rsp_beat_) {
                on_slave_rsp_beat_(3);
            }
        }
        while (auto r = slave_.pop_r()) {
            uint8_t id = r->id;
            if (r_owner_[id].empty()) {
                ADD_FAILURE() << "R beat no owner";
                break;
            }
            if (r->last) r_owner_[id].pop_front();
            if (!r_holdover_.empty() || !port.push_r(*r)) {
                r_holdover_.push_back(*r);
            } else if (r->last && on_slave_rsp_beat_) {
                on_slave_rsp_beat_(4);
            }
        }
    }

    bool done() const { return master_.done(); }
    std::size_t mismatches() const { return sb_.mismatch_count(); }
    axi::AxiMasterT<nmu::AxiSlavePort>& master() { return master_; }
    const nmu::Rob& rob() const { return nmu_.rob(); }
    nsu::Nsu& nsu() { return nsu_; }

  private:
    static nmu::NmuConfig make_nmu_cfg(std::size_t num_vc, std::size_t node) {
        nmu::NmuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.write_rob_mode = nmu::RobMode::Disabled;
        cfg.read_rob_mode = nmu::RobMode::Disabled;
        cfg.port_params = nmu::load_nmu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.vc_mode = nmu::VcMode::ReadWriteSplit;
        cfg.write_vc = 0;
        cfg.read_vc = (num_vc >= 2) ? 1u : 0u;
        return cfg;
    }
    static nsu::NsuConfig make_nsu_cfg(std::size_t num_vc, std::size_t node) {
        nsu::NsuConfig cfg{};
        cfg.src_id = node_src_id(node);
        cfg.port_params = nsu::load_nsu_port_params("config/port_params.yaml");
        cfg.num_vc = num_vc;
        cfg.vc_mode = nsu::VcMode::ReadWriteSplit;
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
        EXPECT_EQ(nmu::addr_trans::xy_route(sc_.transactions.front().addr).dst_id, expected_dst_)
            << "flow request dst_id mismatch (master_node=" << master_node_ << ")";
    }

    axi::Scenario sc_;
    axi::Memory mem_;
    axi::AxiSlave slave_;
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

using ni::cmodel::testing::ComponentRecord;
using ni::cmodel::testing::direction;
using ni::cmodel::testing::FlitLog;
using ni::cmodel::testing::LinkProbe;
using ni::cmodel::testing::NIPerfObserver;
using ni::cmodel::testing::node_id;
using ni::cmodel::testing::NodeCoord;
using ni::cmodel::testing::opposite;
using ni::cmodel::testing::PerfReport;
using ni::cmodel::testing::ReqInProbe;
using ni::cmodel::testing::ReqOutProbe;
using ni::cmodel::testing::router_path;
using ni::cmodel::testing::RspInProbe;
using ni::cmodel::testing::RspOutProbe;
using ni::cmodel::testing::RunMeta;
using ni::cmodel::testing::SegmentDwell;
using ni::cmodel::testing::TxnRecord;

// Per-router latency on one leg, keyed by router name in path order.
struct LegResult {
    std::vector<std::string> component_path;         // ["NMU0", "R(0,0)", "R(1,0)", "NSU1"]
    std::map<std::string, uint64_t> router_latency;  // "R(x,y)" -> min dwell
};

// Path-driven Pass-1 characterization of the canonical node 0 -> node 1 flow.
// Wraps the NI edges + every inter-router link on the request and response
// paths, runs the isolated single-transaction scenario, and returns per-router
// latency for each leg plus the zero-load and NI occupancy.
struct IsolatedResult {
    uint64_t write_zero_load = 0;
    LegResult req_leg;
    LegResult rsp_leg;
    std::size_t nmu_occ_max = 0;
    std::size_t nmu_occ_cap = 0;
    std::size_t nsu_occ_max = 0;
    std::size_t nsu_occ_cap = 0;
    std::map<std::string, std::size_t> router_occ_max;  // "R(x,y)" -> peak LOCAL out fill
    std::size_t router_occ_cap = 0;
};

IsolatedResult characterize_signature() {
    using namespace ni::cmodel::testing;
    const std::size_t num_vc = 1;
    const uint8_t mesh_x = 2, mesh_y = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string base = scenario_path("AX4-BAS-003_single_write_read_aligned");
    const axi::Scenario src = axi::load_scenario(base);
    const std::string tmp = std::string(::testing::TempDir());
    const std::string iso_yaml = tmp + "/perf_iso.yaml";
    write_isolated_scenario(src, /*txn_index=*/0, /*dst_offset=*/0x100000000ull, iso_yaml);

    uint64_t now = 0;

    // Request leg path (node 0 -> node 1) and response leg path (reverse).
    const auto req_coords = router_path(/*src=*/0x00, /*dst=*/0x01, mesh_x, mesh_y);
    const auto rsp_coords = router_path(/*src=*/0x01, /*dst=*/0x00, mesh_x, mesh_y);

    // NI-edge probes.
    FlitLog nmu_req_out_log("NMU0.req_out"), nmu_rsp_in_log("NMU0.rsp_in");
    FlitLog nsu_req_in_log("NSU1.req_in"), nsu_rsp_out_log("NSU1.rsp_out");
    ReqOutProbe nmu_req_out_probe(ch.nmu_req_out(0), nmu_req_out_log, now);
    RspInProbe nmu_rsp_in_probe(ch.nmu_rsp_in(0), nmu_rsp_in_log, now);
    ReqInProbe nsu_req_in_probe(ch.nsu_req_in(1), nsu_req_in_log, now);
    RspOutProbe nsu_rsp_out_probe(ch.nsu_rsp_out(1), nsu_rsp_out_log, now);

    // Inter-router link probes, one per hop, generic over the path. Each link
    // log is the crossing INTO router r_{i+1} (entry to that router); paired
    // with the NEXT boundary it gives r_{i+1}'s latency. r_0's entry is the
    // NI-out boundary.
    std::vector<std::unique_ptr<FlitLog>> req_link_logs, rsp_link_logs;
    std::vector<std::unique_ptr<LinkProbe>> req_link_probes, rsp_link_probes;
    for (std::size_t i = 0; i + 1 < req_coords.size(); ++i) {
        const std::size_t dir = direction(req_coords[i], req_coords[i + 1]);
        auto log = std::make_unique<FlitLog>("req_link_" + std::to_string(i));
        auto probe = std::make_unique<LinkProbe>(
            ch.req_router_at(req_coords[i + 1].x, req_coords[i + 1].y).input(opposite(dir)), *log,
            now);
        ch.req_router_at(req_coords[i].x, req_coords[i].y).set_downstream(dir, *probe);
        req_link_logs.push_back(std::move(log));
        req_link_probes.push_back(std::move(probe));
    }
    for (std::size_t i = 0; i + 1 < rsp_coords.size(); ++i) {
        const std::size_t dir = direction(rsp_coords[i], rsp_coords[i + 1]);
        auto log = std::make_unique<FlitLog>("rsp_link_" + std::to_string(i));
        auto probe = std::make_unique<LinkProbe>(
            ch.rsp_router_at(rsp_coords[i + 1].x, rsp_coords[i + 1].y).input(opposite(dir)), *log,
            now);
        ch.rsp_router_at(rsp_coords[i].x, rsp_coords[i].y).set_downstream(dir, *probe);
        rsp_link_logs.push_back(std::move(log));
        rsp_link_probes.push_back(std::move(probe));
    }

    NIPerfObserver ni(now, "iso");
    Flow flow(ch, /*master_node=*/0, /*slave_node=*/1, iso_yaml, num_vc, tmp + "/perf_iso.read.txt",
              /*dst=*/0x01, nmu_req_out_probe, nmu_rsp_in_probe, nsu_req_in_probe,
              nsu_rsp_out_probe);

    flow.master().on_write_issued(
        [&](const axi::IssueInfo& i) { ni.on_issue(true, i.scenario_line); });
    flow.master().on_write_completed(
        [&](const axi::WriteResult& w) { ni.on_complete(true, w.scenario_line); });
    flow.master().on_read_issued(
        [&](const axi::IssueInfo& i) { ni.on_issue(false, i.scenario_line); });
    flow.master().on_read_observed(
        [&](const axi::ReadResult& r) { ni.on_complete(false, r.scenario_line); });

    IsolatedResult out;
    out.nmu_occ_cap = nmu::Rob::ROB_CAPACITY;
    out.nsu_occ_cap = flow.nsu().axi_master_port().params().aw_queue_depth;
    out.router_occ_cap = ch.req_router_at(req_coords[0].x, req_coords[0].y).output_fifo_depth();

    std::size_t cycle = 0;
    const std::size_t cap = 100000;
    while (!flow.done() && cycle < cap) {
        now = cycle;
        flow.pre_tick();
        ch.tick();
        // Sample NI occupancy (peak) per tick.
        out.nmu_occ_max =
            std::max(out.nmu_occ_max, flow.rob().write_occupancy() + flow.rob().read_occupancy());
        auto& port = flow.nsu().axi_master_port();
        const std::size_t nsu_busy = std::max({port.aw_q_size(), port.w_q_size(), port.ar_q_size(),
                                               port.b_q_size(), port.r_q_size()});
        out.nsu_occ_max = std::max(out.nsu_occ_max, nsu_busy);
        // Sample each request-leg router's LOCAL output FIFO (and the NSU-side).
        for (const auto& c : req_coords) {
            const std::size_t fill =
                ch.req_router_at(c.x, c.y).output_fifo_size(TwoNodeFabric::LOCAL);
            auto& peak = out.router_occ_max[router_name(c)];
            peak = std::max(peak, fill);
        }
        flow.post_tick();
        ++cycle;
    }

    out.write_zero_load = ni.write_latency().count() ? ni.write_latency().min() : 0;

    // Build the request-leg component path + per-router latency by pairing
    // consecutive crossings: [NI-out, link_0, ..., link_{k-1}, NI-in].
    out.req_leg.component_path.push_back("NMU0");
    {
        std::vector<const FlitLog*> chain;
        chain.push_back(&nmu_req_out_log);
        for (auto& l : req_link_logs) chain.push_back(l.get());
        chain.push_back(&nsu_req_in_log);
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            SegmentDwell seg;
            seg.pair(*chain[i], *chain[i + 1]);
            const std::string rn = router_name(req_coords[i]);
            out.req_leg.component_path.push_back(rn);
            if (seg.all().count() > 0) out.req_leg.router_latency[rn] = seg.all().min();
        }
    }
    out.req_leg.component_path.push_back("NSU1");

    out.rsp_leg.component_path.push_back("NSU1");
    {
        std::vector<const FlitLog*> chain;
        chain.push_back(&nsu_rsp_out_log);
        for (auto& l : rsp_link_logs) chain.push_back(l.get());
        chain.push_back(&nmu_rsp_in_log);
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            SegmentDwell seg;
            seg.pair(*chain[i], *chain[i + 1]);
            const std::string rn = router_name(rsp_coords[i]);
            out.rsp_leg.component_path.push_back(rn);
            if (seg.all().count() > 0) out.rsp_leg.router_latency[rn] = seg.all().min();
        }
    }
    out.rsp_leg.component_path.push_back("NMU0");

    return out;
}

// Sum of every measured component latency across both legs (NMU/NSU dwell ~0 in
// the pull-based model, so the routers carry it).
uint64_t sum_component_latency(const IsolatedResult& iso) {
    uint64_t s = 0;
    for (const auto& kv : iso.req_leg.router_latency) s += kv.second;
    for (const auto& kv : iso.rsp_leg.router_latency) s += kv.second;
    return s;
}

}  // namespace

TEST(PerfProbe, EveryRouterOnPathHasComponentEntry) {
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    // Path/component cross-check: every "R(x,y)" in the request path has a
    // latency entry, and likewise for the response path.
    for (const auto& name : iso.req_leg.component_path) {
        if (name.rfind("R(", 0) == 0) {
            EXPECT_EQ(iso.req_leg.router_latency.count(name), 1u)
                << "request-path router " << name << " has no component entry";
        }
    }
    for (const auto& name : iso.rsp_leg.component_path) {
        if (name.rfind("R(", 0) == 0) {
            EXPECT_EQ(iso.rsp_leg.router_latency.count(name), 1u)
                << "response-path router " << name << " has no component entry";
        }
    }
    // 2-node instance: exactly two routers per leg.
    EXPECT_EQ(iso.req_leg.router_latency.size(), 2u);
    EXPECT_EQ(iso.rsp_leg.router_latency.size(), 2u);
}

TEST(PerfProbe, DecompositionSanity) {
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    const uint64_t sum = sum_component_latency(iso);
    ASSERT_GE(iso.write_zero_load, sum)
        << "slave_remainder negative: sum=" << sum << " zero_load=" << iso.write_zero_load;
}

TEST(PerfProbe, MinLatencyAtLeastZeroLoadAndEmit) {
    using ni::cmodel::testing::write_isolated_scenario;
    const IsolatedResult iso = characterize_signature();
    ASSERT_GT(iso.write_zero_load, 0u) << "isolated write flow did not complete";
    const uint64_t zl = iso.write_zero_load;

    // Pass 2: contended bidirectional run (Flow A: 1->0, Flow B: 0->1).
    const std::size_t num_vc = 1;
    TwoNodeFabric ch(static_cast<uint8_t>(num_vc));
    const std::string base = scenario_path("AX4-BAS-003_single_write_read_aligned");
    const axi::Scenario src = axi::load_scenario(base);
    const std::string tmp = std::string(::testing::TempDir());

    // Flow A native (dst 0); Flow B remapped to node 1 via the lossless writer.
    const std::string yaml_a = base;
    const std::string yaml_b = tmp + "/perf_p2_b.yaml";
    write_isolated_scenario(src, /*txn_index=*/0, /*dst_offset=*/0x100000000ull, yaml_b);

    Flow flow_a(ch, 1, 0, yaml_a, num_vc, tmp + "/perf_p2_a.read.txt", 0x00, ch.nmu_req_out(1),
                ch.nmu_rsp_in(1), ch.nsu_req_in(0), ch.nsu_rsp_out(0));
    Flow flow_b(ch, 0, 1, yaml_b, num_vc, tmp + "/perf_p2_b.read.txt", 0x01, ch.nmu_req_out(0),
                ch.nmu_rsp_in(0), ch.nsu_req_in(1), ch.nsu_rsp_out(1));

    uint64_t now = 0;
    NIPerfObserver ni_b(now, "B");
    flow_b.master().on_write_issued(
        [&](const axi::IssueInfo& i) { ni_b.on_issue(true, i.scenario_line); });
    flow_b.master().on_write_completed(
        [&](const axi::WriteResult& w) { ni_b.on_complete(true, w.scenario_line); });

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
    ASSERT_GT(ni_b.write_latency().count(), 0u) << "flow B write did not complete";
    EXPECT_GE(ni_b.write_latency().min(), zl)
        << "measured=" << ni_b.write_latency().min() << " zero_load=" << zl;

    const uint64_t sum = sum_component_latency(iso);
    ASSERT_GE(zl, sum);
    const uint64_t slave_rem = zl - sum;

    PerfReport rep;
    rep.set_scenario("AX4-BAS-003");
    rep.set_run_meta(RunMeta{"AX4-BAS-003", 2, 1, static_cast<uint8_t>(num_vc), cycle, 1,
                             "build/cmodel/perf/AX4-BAS-003.json"});
    rep.set_slave_remainder(slave_rem);
    rep.add_transaction(TxnRecord{1, "write", 0, "NMU0", "NSU1", iso.req_leg.component_path,
                                  iso.rsp_leg.component_path, ni_b.write_latency().min(), zl});
    // NI records with real occupancy.
    rep.add_ni(ComponentRecord{"NMU0", "nmu", 0, 0.0, 0, iso.nmu_occ_max, iso.nmu_occ_cap, true});
    rep.add_ni(ComponentRecord{"NSU1", "nsu", 0, 0.0, 0, iso.nsu_occ_max, iso.nsu_occ_cap, true});
    // Every router on the request leg (mesh-agnostic; loops the path).
    // Name tagged ":req" / ":rsp" so req and rsp records for the same physical
    // router are distinct JSON keys; neither leg's latency is silently dropped.
    for (const auto& kv : iso.req_leg.router_latency) {
        const std::string rec_name = kv.first + ":req";
        const std::size_t occ =
            iso.router_occ_max.count(kv.first) ? iso.router_occ_max.at(kv.first) : 0;
        rep.add_router(ComponentRecord{rec_name, "router", kv.second,
                                       static_cast<double>(kv.second), kv.second, occ,
                                       iso.router_occ_cap, true});
    }
    // Every router on the response leg — same occupancy-lookup pattern.
    for (const auto& kv : iso.rsp_leg.router_latency) {
        const std::string rec_name = kv.first + ":rsp";
        const std::size_t occ =
            iso.router_occ_max.count(kv.first) ? iso.router_occ_max.at(kv.first) : 0;
        rep.add_router(ComponentRecord{rec_name, "router", kv.second,
                                       static_cast<double>(kv.second), kv.second, occ,
                                       iso.router_occ_cap, true});
    }

    // Assert that the EMITTED JSON contains a record for every router on BOTH
    // legs (with the leg-tagged name).  This catches the class of bug where a
    // leg's latencies are computed + summed but never written out.
    std::ostringstream oss;
    rep.write_json(oss);
    const std::string emitted = oss.str();
    for (const auto& kv : iso.req_leg.router_latency) {
        const std::string key = kv.first + ":req";
        EXPECT_NE(emitted.find('"' + key + '"'), std::string::npos)
            << "req-leg router record missing from emitted JSON: " << key;
    }
    for (const auto& kv : iso.rsp_leg.router_latency) {
        const std::string key = kv.first + ":rsp";
        EXPECT_NE(emitted.find('"' + key + '"'), std::string::npos)
            << "rsp-leg router record missing from emitted JSON: " << key;
    }

    rep.emit();
}
