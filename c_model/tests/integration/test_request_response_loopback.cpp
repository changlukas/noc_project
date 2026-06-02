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
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/loopback_noc.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/packetize.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include <cstddef>
#include <deque>
#include <gtest/gtest.h>
#include <string>

namespace axi  = ni::cmodel::axi;
namespace nmu  = ni::cmodel::nmu;
namespace nsu  = ni::cmodel::nsu;
namespace cmod = ni::cmodel;
namespace test = ni::cmodel::testing;

namespace {

constexpr std::size_t kMaxCycles = 200'000;

constexpr uint8_t kNmuSrcId = 0x01;
constexpr uint8_t kNsuSrcId = 0x02;

struct LoopbackResult {
  std::size_t scoreboard_mismatches;
  std::size_t cycle_count;
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

  // NoC test fixture between the two ports.
  test::LoopbackNoc loopback(params.loopback_noc_req_depth,
                             params.loopback_noc_rsp_depth);
  loopback.set_req_delay(req_delay);
  loopback.set_rsp_delay(rsp_delay);

  // Response-path stack (NMU side): nmu::Depacketize pulls rsp_in flits and
  // surfaces B/R beats to the AxiSlavePort.
  nmu::Depacketize nmu_depkt(loopback.rsp_in(),
                             params.depkt_b_q_depth,
                             params.depkt_r_q_depth);

  // Request-path packetizer (NMU side): Packetize self-computes dst via
  // addr_trans::xy_route, so the AxiSlavePort talks to it directly through
  // the Packetizer interface (no adapter needed). Task 8 will wrap this in
  // a Rob instance to add ROB-stall coverage.
  nmu::Packetize    real_nmu_pkt(loopback.req_out(), /*src_id=*/kNmuSrcId);

  // Per-AXI-ID metadata FIFO shared between nsu::Depacketize (snapshot on
  // AW/AR ingress) and nsu::Packetize (peek+commit on B/R egress).
  nsu::MetaBuffer  nsu_meta(params.meta_buffer_per_id_depth);

  // Request-path depacketizer + response-path packetizer (NSU side).
  nsu::Depacketize nsu_depkt(loopback.req_in(), nsu_meta,
                             params.depkt_aw_q_depth,
                             params.depkt_w_q_depth,
                             params.depkt_ar_q_depth);
  nsu::Packetize   nsu_pkt(loopback.rsp_out(), nsu_meta, /*src_id=*/kNsuSrcId);

  // Ports straddle the packetize / depacketize stacks. The NMU port hands
  // requests to real_nmu_pkt directly and pops responses from nmu_depkt.
  // The NSU port pops requests from nsu_depkt and pushes responses into
  // nsu_pkt.
  nmu::AxiSlavePort  nmu_port(real_nmu_pkt, nmu_depkt, params);
  nsu::AxiMasterPort nsu_port(nsu_depkt,    nsu_pkt,   params);

  // Stage 2 endpoints + oracle.
  axi::AxiMasterT<nmu::AxiSlavePort> master(yaml_path, nmu_port, read_dump_path,
                                            sc.config.max_outstanding_write,
                                            sc.config.max_outstanding_read);

  axi::Scoreboard sb;
  master.on_write_completed([&](const axi::WriteResult& wr) {
    sb.handle_write_completed(wr, wr.data, wr.strb_per_beat);
  });
  master.on_read_observed([&](const axi::ReadResult& rr) {
    sb.handle_read_observed(rr);
  });

  // Per-run AxiMasterPort <-> AxiSlave holdovers: when AxiSlave's input
  // queue or the NSU port's response queue is full, we hold the beat for
  // next-cycle retry rather than dropping it. Function-local to keep each
  // test invocation clean (no state leak across fixtures).
  std::deque<axi::BBeat> b_holdover;
  std::deque<axi::RBeat> r_holdover;

  std::size_t cycle = 0;
  while (!master.done()) {
    master.tick();

    // Response-path drain ordering: NMU depkt -> loopback advance ->
    // NSU depkt. This pulls in-flight flits forward by one stage per
    // cycle in both directions before the ports forward their queues.
    nmu_depkt.tick();
    nsu_depkt.tick();

    nmu_port.tick();
    nsu_port.tick();

    // Shuttle requests from NSU AxiMasterPort downstream face into AxiSlave.
    // Default queue sizing absorbs everything the ports forward; a rejected
    // push indicates a sizing mismatch, surface as test failure.
    while (auto aw = nsu_port.pop_aw()) {
      if (!slave.push_aw(*aw)) {
        ADD_FAILURE() << "AxiSlave rejected AW push; queue sizing mismatch";
        break;
      }
    }
    while (auto w = nsu_port.pop_w()) {
      if (!slave.push_w(*w)) {
        ADD_FAILURE() << "AxiSlave rejected W push; queue sizing mismatch";
        break;
      }
    }
    while (auto ar = nsu_port.pop_ar()) {
      if (!slave.push_ar(*ar)) {
        ADD_FAILURE() << "AxiSlave rejected AR push; queue sizing mismatch";
        break;
      }
    }

    slave.tick();
    mem.tick();

    // Shuttle responses from AxiSlave back into NSU AxiMasterPort upstream
    // face. push_b / push_r return false when the port's response queue is
    // full; hold the beat for the next cycle.
    while (!b_holdover.empty()) {
      if (!nsu_port.push_b(b_holdover.front())) break;
      b_holdover.pop_front();
    }
    while (!r_holdover.empty()) {
      if (!nsu_port.push_r(r_holdover.front())) break;
      r_holdover.pop_front();
    }
    if (b_holdover.empty()) {
      while (auto b = slave.pop_b()) {
        if (!nsu_port.push_b(*b)) { b_holdover.push_back(*b); break; }
      }
    }
    if (r_holdover.empty()) {
      while (auto r = slave.pop_r()) {
        if (!nsu_port.push_r(*r)) { r_holdover.push_back(*r); break; }
      }
    }

    // Advance the loopback's per-cycle delay pipes after all producers /
    // consumers have run for this cycle. Mirrors port-pair test ordering.
    loopback.tick();

    if (++cycle > kMaxCycles) {
      return {sb.mismatch_count(), cycle};
    }
  }

  return {sb.mismatch_count(), cycle};
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
        FixtureParam{"multi_outstanding_stress.yaml", 2u, 3u}),
    [](const ::testing::TestParamInfo<FixtureParam>& info) {
      auto n = info.param.yaml;
      auto dot = n.rfind('.');
      if (dot != std::string::npos) n = n.substr(0, dot);
      return n + "_q" + std::to_string(info.param.req_delay) +
             "_s" + std::to_string(info.param.rsp_delay);
    });
