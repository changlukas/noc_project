// Stage 3 integration: end-to-end loopback through the NMU AxiSlavePort +
// NSU AxiMasterPort pair, with the Stage 2 AxiMaster / AxiSlave / Memory /
// Scoreboard as the traffic source + endpoint + oracle.
//
// Wiring:
//   AxiMaster (master.hpp)
//      | push_aw/w/ar          | pop_b/r
//      v                        ^
//   AxiSlavePort (NMU)
//      | (Packetizer pushes)    ^ (Depacketizer pops)
//      v                        |
//   LoopbackChannelSet (shared deques, zero or configurable latency)
//      ^                        |
//      | (Depacketizer pops)    v (Packetizer pushes)
//   AxiMasterPort (NSU)
//      | pop_aw/w/ar             ^ push_b/r
//      v                        |
//   AxiSlave + Memory + Scoreboard
//
// Replayed fixtures (per Stage 3 brief): pick a representative spread of
// Stage 2 fixtures — INCR multi-beat, multi-outstanding stress, aligned
// narrow, WRAP. Each fixture: end-to-end scoreboard zero mismatch == pass.
//
// At least one variant uses a configurable-latency loopback (N-cycle delay
// between packetizer push and depacketizer pop) to catch one-cycle
// registration bugs that the zero-latency loopback would hide.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/scenario.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include "ni/port_params.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nsu/axi_master_port.hpp"
#include <cstddef>
#include <deque>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace axi  = ni::cmodel::axi;
namespace nmu  = ni::cmodel::nmu;
namespace nsu  = ni::cmodel::nsu;
namespace cmod = ni::cmodel;

namespace {

constexpr std::size_t kMaxCycles = 200'000;

// -------------------------------------------------------------------------
// DelayedLoopback: implements both Packetizer and Depacketizer. Each
// channel has a small "pipeline" deque whose tail is the head visible to
// the depacketizer side. delay_cycles_ controls how many tick() calls must
// elapse between a successful push and the beat becoming visible to pop.
// delay_cycles_ == 0 reduces to a plain bounded loopback (still respects
// capacity though: push fails when total in-flight beats == capacity).
// -------------------------------------------------------------------------
template <typename T>
struct DelayChannel {
  std::deque<std::pair<std::size_t, T>> pipe;  // (cycle_visible_at, beat)
  std::deque<T> visible;
  std::size_t capacity = 32;
  std::size_t in_flight() const { return pipe.size() + visible.size(); }
  bool push(const T& v, std::size_t now, std::size_t delay) {
    if (in_flight() >= capacity) return false;
    pipe.push_back({now + delay, v});
    return true;
  }
  std::optional<T> pop() {
    if (visible.empty()) return std::nullopt;
    T v = visible.front(); visible.pop_front(); return v;
  }
  void advance(std::size_t now) {
    while (!pipe.empty() && pipe.front().first <= now) {
      visible.push_back(pipe.front().second);
      pipe.pop_front();
    }
  }
};

class DelayedLoopback : public cmod::Packetizer, public cmod::Depacketizer {
 public:
  explicit DelayedLoopback(std::size_t delay_cycles) : delay_(delay_cycles) {}

  // Bump every channel's visibility window by one cycle.
  void tick_advance() {
    ++now_;
    aw_.advance(now_); w_.advance(now_); ar_.advance(now_);
    b_.advance(now_);  r_.advance(now_);
  }

  // Packetizer (request side: NMU; response side: NSU).
  bool push_aw(const axi::AwBeat& b) override { return aw_.push(b, now_, delay_); }
  bool push_w (const axi::WBeat&  b) override { return w_.push (b, now_, delay_); }
  bool push_ar(const axi::ArBeat& b) override { return ar_.push(b, now_, delay_); }
  bool push_b (const axi::BBeat&  b) override { return b_.push (b, now_, delay_); }
  bool push_r (const axi::RBeat&  b) override { return r_.push (b, now_, delay_); }

  // Depacketizer (response side: NMU; request side: NSU).
  std::optional<axi::BBeat>  pop_b()  override { return b_.pop();  }
  std::optional<axi::RBeat>  pop_r()  override { return r_.pop();  }
  std::optional<axi::AwBeat> pop_aw() override { return aw_.pop(); }
  std::optional<axi::WBeat>  pop_w()  override { return w_.pop();  }
  std::optional<axi::ArBeat> pop_ar() override { return ar_.pop(); }

 private:
  std::size_t now_ = 0;
  std::size_t delay_;
  DelayChannel<axi::AwBeat> aw_;
  DelayChannel<axi::WBeat>  w_;
  DelayChannel<axi::ArBeat> ar_;
  DelayChannel<axi::BBeat>  b_;
  DelayChannel<axi::RBeat>  r_;
};

// Both ports take the same shared cmod::PortParams; load it once per run.
cmod::PortParams load_default_params(const std::string& side) {
  return cmod::load_port_params_yaml("config/port_params.yaml", side);
}

// -------------------------------------------------------------------------
// One run of one fixture through the port-pair loopback. delay_cycles=0 is
// the zero-latency path; >0 inserts a per-beat pipeline delay in both
// directions to flush any one-cycle registration bugs.
// -------------------------------------------------------------------------
struct LoopbackResult {
  std::size_t scoreboard_mismatches;
  std::size_t cycle_count;
};

LoopbackResult run_fixture(const std::string& yaml_path,
                            const std::string& read_dump_path,
                            std::size_t delay_cycles) {
  auto sc = axi::load_scenario(yaml_path);

  axi::Memory   mem(sc.config.memory_base, sc.config.memory_size,
                    sc.config.write_latency, sc.config.read_latency);
  axi::AxiSlave slave(mem);
  slave.set_memory_bounds(sc.config.memory_base, sc.config.memory_size);

  DelayedLoopback loopback(delay_cycles);
  nmu::AxiSlavePort  nmu_port(loopback, loopback, load_default_params("nmu"));
  nsu::AxiMasterPort nsu_port(loopback, loopback, load_default_params("nsu"));

  // The AxiMaster type is templated on the slave-side adaptor. AxiSlavePort
  // exposes the exact push_aw/push_w/push_ar + pop_b/pop_r API AxiMaster
  // requires, so we instantiate AxiMasterT<AxiSlavePort> directly.
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

  // Per-run holdovers: when AxiSlave produces a B/R beat but the NSU port's
  // response queue is full, we hold the beat here for next-cycle retry
  // rather than dropping it. Function-local so each test invocation starts
  // clean (no state leak between fixtures).
  std::deque<axi::BBeat> b_holdover;
  std::deque<axi::RBeat> r_holdover;
  std::size_t cycle = 0;
  while (!master.done()) {
    // Ordering: drive upstream traffic, port forwards, NSU port surfaces
    // requests, harness shuttles between AxiMasterPort and AxiSlave, slave
    // produces responses, harness shuttles them back. Then advance the
    // loopback's per-cycle pipeline.
    master.tick();
    nmu_port.tick();
    nsu_port.tick();

    // Shuttle requests from NSU AxiMasterPort downstream face into AxiSlave.
    while (auto aw = nsu_port.pop_aw()) {
      if (!slave.push_aw(*aw)) {
        // AxiSlave rejected: re-queue by pushing back into the loopback?
        // We can't easily do that with the pop_* API. In practice with
        // default depths the slave's 32-deep queue absorbs anything the
        // ports forward; if this ever fires it indicates a sizing mismatch
        // — treat as test failure to surface the issue.
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
    // full; in that case we hold the beat for the next cycle.
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

    loopback.tick_advance();
    if (++cycle > kMaxCycles) {
      return {sb.mismatch_count(), cycle};
    }
  }
  return {sb.mismatch_count(), cycle};
}

struct FixtureParam {
  std::string yaml;
  std::size_t delay_cycles;
};

}  // namespace

class PortPairLoopbackP : public ::testing::TestWithParam<FixtureParam> {};

TEST_P(PortPairLoopbackP, EndToEndZeroMismatch) {
  SCENARIO("port-pair loopback: NMU+NSU port pair end-to-end fixture run produces zero scoreboard mismatches");
  auto p = GetParam();
  std::string yaml_path = "fixtures/" + p.yaml;
  std::string rpath = std::string(::testing::TempDir()) + "/" + p.yaml +
                      ".port_pair_d" + std::to_string(p.delay_cycles) + ".read.txt";
  auto r = run_fixture(yaml_path, rpath, p.delay_cycles);
  EXPECT_EQ(r.scoreboard_mismatches, 0u)
      << "scoreboard mismatches in " << p.yaml
      << " (delay=" << p.delay_cycles << ")";
  EXPECT_LE(r.cycle_count, kMaxCycles)
      << "watchdog tripped on " << p.yaml
      << " (delay=" << p.delay_cycles << ")";
}

INSTANTIATE_TEST_SUITE_P(
    PortPairFixtures, PortPairLoopbackP,
    ::testing::Values(
        // Zero-latency loopback baseline: catches structural bugs.
        FixtureParam{"burst_incr_8beat.yaml",          0},
        FixtureParam{"multi_outstanding_stress.yaml",  0},
        FixtureParam{"wrap_burst_aligned.yaml",        0},
        FixtureParam{"narrow_aligned_multibeat.yaml",  0},
        // Configurable-latency variant: 2-cycle and 3-cycle delays exercise
        // multi-cycle in-flight ordering and surface one-cycle
        // registration bugs the zero-latency path hides.
        FixtureParam{"burst_incr_8beat.yaml",          2},
        FixtureParam{"multi_outstanding_stress.yaml",  3}),
    [](const ::testing::TestParamInfo<FixtureParam>& info) {
      auto n = info.param.yaml;
      auto dot = n.rfind('.');
      if (dot != std::string::npos) n = n.substr(0, dot);
      return n + "_d" + std::to_string(info.param.delay_cycles);
    });
