// Stage 3 integration: full request/response e2e loopback through the
// four packetize/depacketize modules + LoopbackNoc, sourced and oracled
// by the Stage 2 AxiMaster / AxiSlave / Memory / Scoreboard fixtures.
//
// Wiring (request path: NMU -> NoC -> NSU):
//
//   AxiMaster (Stage 2 traffic source, YAML-driven)
//      | push_aw / push_w / push_ar      ^ pop_b / pop_r
//      v                                  |
//   AxiSlavePort (NMU)                    |
//      | (Packetizer pushes)              | (Depacketizer pops)
//      v                                  |
//   nmu::Packetize                        nmu::Depacketize
//      |                                  ^
//      v push_flit                        | pop_flit
//   LoopbackNoc (req_out -> req_in,  rsp_out -> rsp_in,  optional N-cycle delay)
//      |                                  ^
//      v pop_flit                         | push_flit
//   nsu::Depacketize                      nsu::Packetize
//      ^                                  |
//      | (Depacketizer pops)              | (Packetizer pushes)
//      |                                  v
//   AxiMasterPort (NSU)
//      | pop_aw / pop_w / pop_ar          ^ push_b / push_r
//      v                                  |
//   AxiSlave + Memory + Scoreboard
//
// This test extends the Stage 3 port-pair loopback (test_port_pair_loopback.cpp)
// by inserting the real four-packetize layers (nmu::Packetize +
// nmu::Depacketize on the NMU side; nsu::Depacketize + nsu::Packetize on
// the NSU side, sharing the per-AXI-ID MetaBuffer) between the two ports
// instead of routing AW/W/AR/B/R beats through a single passthrough loopback.
//
// Per-fixture pass criterion: Scoreboard zero mismatch after master.done()
// AND every NoC / port queue / NSU AxiSlave holdover is drained.
//
// At least one variant uses LoopbackNoc.set_req_delay / set_rsp_delay so
// non-zero in-flight pipelines exercise multi-cycle ordering paths the
// zero-latency case hides.
//
// multi_dst_stress fixture acts as the real regression gate for ROB Enabled
// mode reorder: 4 NSU stacks with per-NSU latency variance (NSU_0=10c,
// NSU_1=2c, NSU_2=5c, NSU_3=3c) make NSU_1 return B for AW2 before NSU_0's B
// for AW1. Rob Enabled must reorder before handing to AxiMaster so AXI4
// IHI 0022 §A5.3 same-id submission order at the master boundary holds.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "common/test_logger.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/packetize.hpp"
#include "nmu/rob.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace axi  = ni::cmodel::axi;
namespace nmu  = ni::cmodel::nmu;
namespace nsu  = ni::cmodel::nsu;
namespace cmod = ni::cmodel;
namespace test = ni::cmodel::testing;

namespace {

constexpr std::size_t kMaxCycles = 200'000;

constexpr uint8_t kNmuSrcId = 0x01;
constexpr uint8_t kNsuSrcId = 0x02;

// Multi-NSU constants for the multi_dst_stress regression gate.
constexpr std::size_t kNumNsuMulti = 4;
constexpr std::array<uint8_t, kNumNsuMulti> kNsuSrcIdsMulti =
    {0x10, 0x11, 0x12, 0x13};

// PerIdOrderTracker — verifies per-id B/R beat arrival order at AxiMaster.
// multi_dst_stress: id=0x05 B beats must arrive in submission order (AW1, AW2)
// regardless of physical NoC latency (AXI4 IHI 0022 §A5.3). Without Rob
// Enabled mode reordering, NSU_1 (faster) returns B2 before NSU_0's B1,
// violating the per-id order. We assert this directly as the regression
// gate (positive ordering check, not expected-failure pattern).
struct PerIdOrderTracker {
    std::array<std::vector<uint64_t>, 256> b_seq;
    std::array<std::vector<uint64_t>, 256> r_seq;
    void record_b(uint8_t id, uint64_t marker) { b_seq[id].push_back(marker); }
    void record_r(uint8_t id, uint64_t marker) { r_seq[id].push_back(marker); }
    bool verify_b_in_order(uint8_t id) const {
        const auto& v = b_seq[id];
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (v[i] < v[i-1]) return false;
        }
        return true;
    }
    bool verify_r_in_order(uint8_t id) const {
        const auto& v = r_seq[id];
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (v[i] < v[i-1]) return false;
        }
        return true;
    }
};

struct LoopbackResult {
  std::size_t scoreboard_mismatches;
  std::size_t cycle_count;
  bool        b_order_ok = true;
  bool        r_order_ok = true;
  bool        is_multi_dst = false;
};

LoopbackResult run_fixture(const std::string& yaml_path,
                           const std::string& read_dump_path,
                           unsigned req_delay,
                           unsigned rsp_delay) {
  auto sc = axi::load_scenario(yaml_path);

  axi::Memory   mem(sc.config.memory_base, sc.config.memory_size,
                    sc.config.write_latency, sc.config.read_latency);
  axi::AxiSlave slave(mem);
  slave.set_memory_bounds(sc.config.memory_base, sc.config.memory_size);

  auto params = cmod::load_port_params_yaml("config/port_params.yaml", "nmu");

  const bool is_multi_dst =
      yaml_path.find("multi_dst_stress.yaml") != std::string::npos;
  const nmu::RobMode rob_mode =
      is_multi_dst ? nmu::RobMode::Enabled : nmu::RobMode::Disabled;

  // NoC test fixture: single-NSU for legacy fixtures (preserves existing
  // global req/rsp delay path), multi-NSU for multi_dst_stress so we can
  // wire 4 dst boundaries to 4 independent NSU stacks with per-NSU latency.
  std::unique_ptr<test::LoopbackNoc> loopback_ptr;
  if (is_multi_dst) {
    loopback_ptr = std::make_unique<test::LoopbackNoc>(
        /*num_nsu=*/kNumNsuMulti,
        /*req_q_depth_per_nsu=*/params.loopback_noc_req_depth,
        /*rsp_q_depth_total=*/params.loopback_noc_rsp_depth);
    // multi_dst_stress addresses: 0x100 -> dst=0, 0x10100 -> dst=1 via
    // addr_trans::xy_route. Map dst_id {0..3} -> NSU {0..3}.
    loopback_ptr->set_dst_route(0x00, 0);
    loopback_ptr->set_dst_route(0x01, 1);
    loopback_ptr->set_dst_route(0x02, 2);
    loopback_ptr->set_dst_route(0x03, 3);
    // Per-NSU response latency: NSU_0 slow (10c), NSU_1 fast (2c) exposes
    // out-of-order B arrival; Rob Enabled mode must reorder to preserve AXI4
    // IHI 0022 §A5.3 same-id submission order.
    loopback_ptr->set_nsu_latency(0, 10);
    loopback_ptr->set_nsu_latency(1, 2);
    loopback_ptr->set_nsu_latency(2, 5);
    loopback_ptr->set_nsu_latency(3, 3);
  } else {
    loopback_ptr = std::make_unique<test::LoopbackNoc>(
        params.loopback_noc_req_depth,
        params.loopback_noc_rsp_depth);
    loopback_ptr->set_req_delay(req_delay);
    loopback_ptr->set_rsp_delay(rsp_delay);
  }
  test::LoopbackNoc& loopback = *loopback_ptr;

  // Response-path stack (NMU side): nmu::Depacketize pulls rsp_in flits and
  // surfaces B/R beats to the AxiSlavePort.
  nmu::Depacketize nmu_depkt(loopback.nmu_rsp_in(),
                             params.depkt_b_q_depth,
                             params.depkt_r_q_depth);

  // Request-path packetizer (NMU side): Packetize self-computes dst via
  // addr_trans::xy_route. Wrapped in Rob (Disabled for legacy fixtures,
  // Enabled for multi_dst_stress so per-id B/R reorder runs across NSUs).
  nmu::Packetize    real_nmu_pkt(loopback.nmu_req_out(), /*src_id=*/kNmuSrcId);
  nmu::Rob          rob(real_nmu_pkt, nmu_depkt, rob_mode, rob_mode);

  // Ports straddle the packetize / depacketize stacks. The NMU port hands
  // requests to Rob (which forwards to real_nmu_pkt) and pops responses
  // from Rob (which forwards to nmu_depkt). Rob serves as both Packetizer
  // and Depacketizer via multi-inheritance.
  nmu::AxiSlavePort  nmu_port(rob, rob, params);

  // NSU stacks: 1 for legacy fixtures, 4 for multi_dst_stress.
  // Each NSU owns its own MetaBuffer / Depacketize / Packetize / AxiMasterPort
  // so per-NSU response ordering is independent and the rig stresses the
  // cross-NSU reorder path inside Rob (Enabled mode).
  const std::size_t nsu_count = is_multi_dst ? kNumNsuMulti : 1u;
  std::vector<std::unique_ptr<nsu::MetaBuffer>>    nsu_metas;
  std::vector<std::unique_ptr<nsu::Depacketize>>   nsu_depkts;
  std::vector<std::unique_ptr<nsu::Packetize>>     nsu_pkts;
  std::vector<std::unique_ptr<nsu::AxiMasterPort>> nsu_ports;
  nsu_metas.reserve(nsu_count);
  nsu_depkts.reserve(nsu_count);
  nsu_pkts.reserve(nsu_count);
  nsu_ports.reserve(nsu_count);
  for (std::size_t i = 0; i < nsu_count; ++i) {
    const uint8_t this_nsu_src =
        is_multi_dst ? kNsuSrcIdsMulti[i] : kNsuSrcId;
    nsu_metas.emplace_back(
        std::make_unique<nsu::MetaBuffer>(params.meta_buffer_per_id_depth));
    nsu_depkts.emplace_back(std::make_unique<nsu::Depacketize>(
        loopback.nsu_req_in(i), *nsu_metas[i],
        params.depkt_aw_q_depth,
        params.depkt_w_q_depth,
        params.depkt_ar_q_depth));
    nsu_pkts.emplace_back(std::make_unique<nsu::Packetize>(
        loopback.nsu_rsp_out(i), *nsu_metas[i], this_nsu_src));
    nsu_ports.emplace_back(std::make_unique<nsu::AxiMasterPort>(
        *nsu_depkts[i], *nsu_pkts[i], params));
  }

  // Per-fixture override for ROB stall coverage: multi_dst_stress needs
  // max_outstanding_write >= 2 so AxiMaster admits both same-id writes
  // concurrently, forcing Rob to stall the 2nd until the 1st B returns.
  // ScenarioConfig defaults to 1 outstanding when the YAML omits the field,
  // so keep the override rig-side rather than adding a YAML knob.
  std::size_t mow = sc.config.max_outstanding_write;
  std::size_t mor = sc.config.max_outstanding_read;
  if (is_multi_dst) {
    mow = std::max<std::size_t>(mow, 2);
    mor = std::max<std::size_t>(mor, 2);
  }

  // Stage 2 endpoints + oracle.
  axi::AxiMasterT<nmu::AxiSlavePort> master(yaml_path, nmu_port, read_dump_path,
                                            mow, mor);

  // Observability hook: AxiMasterObserver tracks per-transaction counts,
  // AXI4 IHI 0022 §A5.3 per-id ordering, and (under NOC_LOG=1) emits a
  // parse-friendly trace line per AW/B/AR/R. RAII dtor prints
  // [summary:NMU] at scope exit; ok()-driven hard fail deferred to a
  // future round per plan Task 3 spec.
  //
  // Wiring note: this testbench uses AxiMasterT<nmu::AxiSlavePort> (not
  // the default AxiMasterT<AxiSlave> = axi::AxiMaster type expected by
  // the observer's master-binding ctor), so we use the test-only ctor
  // and chain test_inject_write_result / test_inject_read_result into
  // the existing scoreboard callbacks below. This keeps the master's
  // single on_write_completed / on_read_observed slot owned by the
  // scoreboard (the test's primary oracle) while still feeding the
  // observer one event per completed transaction.
  test::AxiMasterObserver obs("NMU");

  axi::Scoreboard sb;
  PerIdOrderTracker tracker;
  // Submission-order markers: use scenario_line (strictly increasing in
  // YAML submission order; AW1=line 25, AW2=line 32 for multi_dst_stress).
  // record_b in completion order; verify_b_in_order checks the recorded
  // sequence is non-decreasing per id. If Rob fails to reorder and B2
  // arrives at AxiMaster before B1, the recorded sequence for id=0x05
  // is {line(AW2)=32, line(AW1)=25} and verify_b_in_order returns false.
  master.on_write_completed([&](const axi::WriteResult& wr) {
    sb.handle_write_completed(wr, wr.data, wr.strb_per_beat);
    tracker.record_b(wr.id, wr.scenario_line);
    obs.test_inject_write_result(wr);
  });
  master.on_read_observed([&](const axi::ReadResult& rr) {
    sb.handle_read_observed(rr);
    tracker.record_r(rr.id, rr.scenario_line);
    obs.test_inject_read_result(rr);
  });

  // Per-run AxiMasterPort <-> AxiSlave holdovers (one deque per NSU): when
  // AxiSlave's input queue or the NSU port's response queue is full, hold
  // the beat for next-cycle retry rather than dropping it. Function-local
  // to keep each test invocation clean (no state leak across fixtures).
  std::vector<std::deque<axi::BBeat>> b_holdovers(nsu_count);
  std::vector<std::deque<axi::RBeat>> r_holdovers(nsu_count);

  // Per-AXI-ID FIFO of NSU index that issued each outstanding AW / AR.
  // AxiSlave preserves per-id response order (AXI4 IHI 0022 §A5.3), so
  // popping the front entry on each B / R(last) recovers the original NSU.
  // This is essential for multi-NSU: when AW1 (id=5) routes via NSU_0 and
  // AW2 (id=5) routes via NSU_1, the slave still emits B1 before B2, and
  // the rig must hand B1 back to NSU_0 (whose MetaBuffer holds the
  // matching dst/src snapshot) and B2 back to NSU_1.
  std::array<std::deque<std::size_t>, 256> b_owner_nsu;
  std::array<std::deque<std::size_t>, 256> r_owner_nsu;

  std::size_t cycle = 0;
  while (!master.done()) {
    master.tick();

    // Response-path drain ordering: NMU depkt -> per-NSU depkts -> ports.
    // This pulls in-flight flits forward by one stage per cycle in both
    // directions before the ports forward their queues.
    nmu_depkt.tick();
    for (std::size_t i = 0; i < nsu_count; ++i) {
      nsu_depkts[i]->tick();
    }

    nmu_port.tick();
    for (std::size_t i = 0; i < nsu_count; ++i) {
      nsu_ports[i]->tick();
    }

    // Shuttle requests from each NSU AxiMasterPort downstream face into the
    // shared AxiSlave. Default queue sizing absorbs everything the ports
    // forward; a rejected push indicates a sizing mismatch, surfaced as
    // test failure.
    for (std::size_t i = 0; i < nsu_count; ++i) {
      auto* port = nsu_ports[i].get();
      while (auto aw = port->pop_aw()) {
        uint8_t id = aw->id;
        if (!slave.push_aw(*aw)) {
          ADD_FAILURE() << "AxiSlave rejected AW push; queue sizing mismatch "
                        << "(nsu=" << i << ")";
          break;
        }
        b_owner_nsu[id].push_back(i);
      }
      while (auto w = port->pop_w()) {
        if (!slave.push_w(*w)) {
          ADD_FAILURE() << "AxiSlave rejected W push; queue sizing mismatch "
                        << "(nsu=" << i << ")";
          break;
        }
      }
      while (auto ar = port->pop_ar()) {
        uint8_t id = ar->id;
        if (!slave.push_ar(*ar)) {
          ADD_FAILURE() << "AxiSlave rejected AR push; queue sizing mismatch "
                        << "(nsu=" << i << ")";
          break;
        }
        r_owner_nsu[id].push_back(i);
      }
    }

    slave.tick();
    mem.tick();

    // Shuttle responses from AxiSlave back into the correct NSU
    // AxiMasterPort upstream face. With multiple NSUs the rig must route
    // each B / R beat back to the NSU that owns the matching outstanding
    // AW / AR. AxiSlave preserves per-id submission order, so the front of
    // b_owner_nsu[id] / r_owner_nsu[id] is always the correct destination.
    // R bursts: keep the owner entry until r->last so every beat of a
    // multi-beat burst routes to the same NSU.
    // Drain holdovers first (per-NSU), then pull from slave.
    for (std::size_t i = 0; i < nsu_count; ++i) {
      while (!b_holdovers[i].empty()) {
        if (!nsu_ports[i]->push_b(b_holdovers[i].front())) break;
        b_holdovers[i].pop_front();
      }
      while (!r_holdovers[i].empty()) {
        if (!nsu_ports[i]->push_r(r_holdovers[i].front())) break;
        r_holdovers[i].pop_front();
      }
    }

    // Pull every B from the slave and route to the owning NSU.
    while (auto b = slave.pop_b()) {
      uint8_t id = b->id;
      if (b_owner_nsu[id].empty()) {
        ADD_FAILURE() << "B beat for id=" << static_cast<int>(id)
                      << " with no outstanding AW owner (FIFO empty)";
        break;
      }
      std::size_t i = b_owner_nsu[id].front();
      b_owner_nsu[id].pop_front();
      if (!b_holdovers[i].empty() || !nsu_ports[i]->push_b(*b)) {
        b_holdovers[i].push_back(*b);
      }
    }
    while (auto r = slave.pop_r()) {
      uint8_t id = r->id;
      if (r_owner_nsu[id].empty()) {
        ADD_FAILURE() << "R beat for id=" << static_cast<int>(id)
                      << " with no outstanding AR owner (FIFO empty)";
        break;
      }
      std::size_t i = r_owner_nsu[id].front();
      if (r->last) {
        r_owner_nsu[id].pop_front();
      }
      if (!r_holdovers[i].empty() || !nsu_ports[i]->push_r(*r)) {
        r_holdovers[i].push_back(*r);
      }
    }

    // Advance the loopback's per-cycle delay pipes after all producers /
    // consumers have run for this cycle. Mirrors port-pair test ordering.
    loopback.tick();

    if (++cycle > kMaxCycles) {
      LoopbackResult r{sb.mismatch_count(), cycle, true, true, is_multi_dst};
      if (is_multi_dst) {
        r.b_order_ok = tracker.verify_b_in_order(0x05);
        r.r_order_ok = tracker.verify_r_in_order(0x05);
      }
      return r;
    }
  }

  LoopbackResult result{sb.mismatch_count(), cycle, true, true, is_multi_dst};
  if (is_multi_dst) {
    result.b_order_ok = tracker.verify_b_in_order(0x05);
    result.r_order_ok = tracker.verify_r_in_order(0x05);
  }
  return result;
}

struct FixtureParam {
  std::string yaml;
  unsigned    req_delay;
  unsigned    rsp_delay;
};

}  // namespace

class PacketizeLoopbackFixture
    : public ::testing::TestWithParam<FixtureParam> {};

TEST_P(PacketizeLoopbackFixture, ScoreboardZeroMismatch) {
  SCENARIO("req/rsp loopback: NMU+NSU packetize/depacketize + LoopbackNoc end-to-end zero mismatch");
  auto p = GetParam();
  std::string yaml_path = "fixtures/" + p.yaml;
  std::string rpath = std::string(::testing::TempDir()) + "/" + p.yaml +
                      ".pkt_e2e_q" + std::to_string(p.req_delay) +
                      "_s" + std::to_string(p.rsp_delay) + ".read.txt";
  auto r = run_fixture(yaml_path, rpath, p.req_delay, p.rsp_delay);
  EXPECT_EQ(r.scoreboard_mismatches, 0u)
      << "scoreboard mismatches in " << p.yaml
      << " (req_delay=" << p.req_delay
      << " rsp_delay=" << p.rsp_delay << ")";
  EXPECT_LE(r.cycle_count, kMaxCycles)
      << "watchdog tripped on " << p.yaml
      << " (req_delay=" << p.req_delay
      << " rsp_delay=" << p.rsp_delay << ")";
  if (r.is_multi_dst) {
    EXPECT_TRUE(r.b_order_ok)
        << "multi_dst_stress: id=0x05 B beats arrived out of submission "
        << "order at AxiMaster. Rob Enabled mode failed to reorder despite "
        << "per-NSU latency variance (NSU_0=10c, NSU_1=2c).";
    EXPECT_TRUE(r.r_order_ok)
        << "multi_dst_stress: id=0x05 R beats arrived out of submission "
        << "order at AxiMaster. Rob Enabled mode failed to reorder despite "
        << "per-NSU latency variance (NSU_0=10c, NSU_1=2c).";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Fixtures, PacketizeLoopbackFixture,
    ::testing::Values(
        // Zero-latency loopback baseline across the five Stage 2 fixture
        // categories: INCR multi-beat, multi-outstanding stress, WRAP,
        // aligned narrow, sparse-strobe multibeat.
        FixtureParam{"burst_incr_8beat.yaml",         0u, 0u},
        FixtureParam{"multi_outstanding_stress.yaml", 0u, 0u},
        FixtureParam{"wrap_burst_aligned.yaml",       0u, 0u},
        FixtureParam{"narrow_aligned_multibeat.yaml", 0u, 0u},
        FixtureParam{"sparse_multibeat.yaml",         0u, 0u},
        // Configurable-latency variant on the multi-outstanding fixture:
        // 2-cycle request delay + 3-cycle response delay exercises
        // multi-cycle in-flight ordering and surfaces one-cycle
        // registration bugs that the zero-latency path hides.
        FixtureParam{"multi_outstanding_stress.yaml", 2u, 3u},
        // Multi-NSU regression gate: 2 same-id writes + 2 same-id reads at
        // different XYRouting dst boundaries (0x100 -> dst=0,
        // 0x10100 -> dst=1). Rig builds 4 NSU stacks with per-NSU latency
        // {10, 2, 5, 3}; Rob Enabled mode must reorder per-id B/R back into
        // submission order before AxiMaster observes them. The rig
        // overrides max_outstanding_{write,read} to 2 for this fixture so
        // AxiMaster admits both same-id transactions concurrently.
        FixtureParam{"multi_dst_stress.yaml",         0u, 0u}),
    [](const ::testing::TestParamInfo<FixtureParam>& info) {
      auto n = info.param.yaml;
      auto dot = n.rfind('.');
      if (dot != std::string::npos) n = n.substr(0, dot);
      return n + "_q" + std::to_string(info.param.req_delay) +
             "_s" + std::to_string(info.param.rsp_delay);
    });
