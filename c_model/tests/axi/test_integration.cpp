// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/scenario.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

namespace axi = ni::cmodel::axi;

// Watchdog cap to catch hangs / deadlocks in any single fixture run.
constexpr std::size_t kMaxCycles = 100'000;

struct IntegrationResult {
  bool        file_diff_pass;
  std::size_t scoreboard_mismatches;
  std::size_t cycle_count;
};

inline bool diff_files(const std::string& a, const std::string& b) {
  std::ifstream fa(a), fb(b);
  std::stringstream sa, sb;
  sa << fa.rdbuf();
  sb << fb.rdbuf();
  return sa.str() == sb.str();
}

static IntegrationResult run_scenario(const std::string& yaml_path,
                                      const std::string& write_data_path,
                                      const std::string& read_dump_path) {
  auto sc = axi::load_scenario(yaml_path);

  axi::Memory   mem(sc.config.memory_base, sc.config.memory_size,
                    sc.config.write_latency, sc.config.read_latency);
  axi::AxiSlave slave(mem);
  slave.set_memory_bounds(sc.config.memory_base, sc.config.memory_size);
  axi::AxiMasterT<axi::AxiSlave> master(yaml_path, slave, read_dump_path,
                                        sc.config.max_outstanding_write,
                                        sc.config.max_outstanding_read);
  axi::Scoreboard sb;
  master.on_write_completed([&](const axi::WriteResult& wr) {
    sb.handle_write_completed(wr, wr.data, wr.strb_per_beat);
  });
  master.on_read_observed([&](const axi::ReadResult& rr) {
    sb.handle_read_observed(rr);
  });

  std::size_t cycle = 0;
  while (!master.done()) {
    master.tick();
    slave.tick();
    mem.tick();
    if (++cycle > kMaxCycles) {
      return IntegrationResult{false, sb.mismatch_count(), cycle};
    }
  }
  bool fdiff = write_data_path.empty() ? true : diff_files(write_data_path, read_dump_path);
  return IntegrationResult{fdiff, sb.mismatch_count(), cycle};
}

struct FixtureParam {
  std::string yaml;
  std::string write_data;  // file to diff against the read dump (empty = skip diff)
  bool        expect_file_diff_pass;
  bool        expect_zero_mismatches;
};

// Generates a readable name for each TEST_P instance, e.g.
// AxiFixtures/IntegrationP.RunFixture/single_write_read_aligned
struct FixtureName {
  std::string operator()(const ::testing::TestParamInfo<FixtureParam>& info) const {
    auto n = info.param.yaml;
    auto dot = n.rfind('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
    return n;
  }
};

class IntegrationP : public ::testing::TestWithParam<FixtureParam> {};

TEST_P(IntegrationP, RunFixture) {
  SCENARIO("axi integration: end-to-end YAML fixture runs Master+Slave+Mem to completion under watchdog");
  auto p = GetParam();
  std::string yaml_path = "fixtures/" + p.yaml;
  std::string wpath     = p.write_data.empty() ? std::string{} : ("fixtures/" + p.write_data);
  std::string rpath     = std::string(::testing::TempDir()) + "/" + p.yaml + ".read.txt";
  auto r = run_scenario(yaml_path, wpath, rpath);
  if (p.expect_file_diff_pass) {
    EXPECT_TRUE(r.file_diff_pass) << "file diff failed: " << p.yaml
                                  << " (wpath=" << wpath << ", rpath=" << rpath << ")";
  }
  if (p.expect_zero_mismatches) {
    EXPECT_EQ(r.scoreboard_mismatches, 0u) << "scoreboard mismatches: " << p.yaml;
  }
  EXPECT_LE(r.cycle_count, kMaxCycles) << "watchdog tripped: " << p.yaml;
}

INSTANTIATE_TEST_SUITE_P(
    AxiFixtures, IntegrationP,
    ::testing::Values(
        FixtureParam{"single_write_read_aligned.yaml",  "single_write_read_aligned_data.txt",  true,  true},
        FixtureParam{"burst_incr_2beat.yaml",           "burst_incr_2beat_data.txt",           true,  true},
        FixtureParam{"burst_incr_8beat.yaml",           "burst_incr_8beat_data.txt",           true,  true},
        FixtureParam{"multi_txn_same_id.yaml",          "multi_txn_same_id_data.txt",          true,  true},
        FixtureParam{"multi_txn_diff_id.yaml",          "multi_txn_diff_id_data.txt",          true,  true},
        FixtureParam{"decerr_oob_write.yaml",           "",                                    false, true},
        FixtureParam{"decerr_oob_read.yaml",            "",                                    false, true},
        FixtureParam{"latency_stress.yaml",             "latency_stress_data.txt",             true,  true},
        FixtureParam{"single_read_default_fill.yaml",   "",                                    false, true},
        FixtureParam{"burst_crosses_oob_boundary.yaml", "",                                    false, true},
        // backpressure_retry: 4 concurrent writes + 4 concurrent reads on the
        // same addresses race each other (no AXI write-before-read ordering),
        // so the read dump is non-deterministic relative to write completion.
        // We verify watchdog + scoreboard only; file diff is skipped.
        FixtureParam{"backpressure_retry.yaml",         "",                                    false, true},
        FixtureParam{"multi_outstanding_stress.yaml",   "multi_outstanding_stress_data.txt",   true,  true},
        // Phase B-2: INCR with unaligned start addr 0x1005 size=5; first beat
        // WSTRB clears lanes 0..4. Read dump cannot byte-match the write data
        // (alignment differs); scoreboard validates byte-level correctness.
        // NOTE: c_model emits 1 beat where AXI4 spec would require 2 (the burst
        // logically crosses lane 31 into a 2nd bus word). Data file uses
        // trailing 0 padding to compensate; only 27 user bytes are committed.
        FixtureParam{"unaligned_start.yaml",            "",                                    false, true},
        // Phase B-3b: aligned narrow bursts. data_file is packed user bytes
        // ((len+1)*bpb total); read dump is per-beat bus-image and does not
        // match the packed layout, so file diff is skipped. Scoreboard
        // validates byte-level write->read equivalence.
        FixtureParam{"narrow_transfer_size2.yaml",      "",                                    false, true},
        FixtureParam{"narrow_transfer_size0.yaml",      "",                                    false, true},
        // Phase B-4: WRAP and FIXED bursts (AXI4 IHI 0022 B1.4.3).
        // - wrap_burst_aligned: 2-beat WRAP that stays inside the window (no
        //   actual wrap). Read dump matches the packed data file byte-for-byte
        //   because beats arrive in receive order on aligned size=5.
        // - wrap_burst_actual_wrap: 4-beat WRAP that crosses wrap_upper. Beats
        //   arrive in receive order (0x1060, 0x1000, 0x1020, 0x1040) and the
        //   write data file is laid out in the same receive order, so file
        //   diff still matches.
        // - fixed_burst: 4-beat FIXED with all beats at addr 0x1000. Memory
        //   sees last-beat-wins, so the read dump returns 4 copies of beat 3
        //   while the data file has 4 distinct beats; file diff is skipped.
        //   Scoreboard models last-beat-wins per byte via map assignment.
        FixtureParam{"wrap_burst_aligned.yaml",         "wrap_burst_aligned_data.txt",         true,  true},
        FixtureParam{"wrap_burst_actual_wrap.yaml",     "wrap_burst_actual_wrap_data.txt",     true,  true},
        FixtureParam{"fixed_burst.yaml",                "",                                    false, true},
        // Phase B-5a: INCR txn at 0x0FE0 size=5 len=7 (256B) crosses the 4KB
        // boundary at 0x1000. AxiMaster auto-segments into 1-beat + 7-beat
        // sub-bursts via split_into_sub_bursts. Both addr=0x0FE0 and 0x1000
        // are 32-byte aligned, so byte_lane=0 throughout — the read dump
        // matches the packed data file byte-for-byte.
        FixtureParam{"cross_4kb_auto_split.yaml",       "cross_4kb_auto_split_data.txt",       true,  true},
        // Phase B-5b combined fixtures.
        // - narrow_aligned_multibeat: aligned narrow multi-beat (addr=0x1004,
        //   size=2, len=3). Read dump is bus-image-per-beat, not packed, so
        //   file diff is skipped; scoreboard validates byte equivalence.
        //   (Renamed from narrow_unaligned.yaml in Phase C audit D1-1 —
        //   addr=0x1004 is (1<<size)=4-aligned, not unaligned.)
        // - sparse_multibeat: 4-beat size=5 burst with per-beat sparse WSTRB
        //   patterns (0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000). Each
        //   beat enables a different byte-group; scoreboard validates that
        //   only enabled bytes update memory.
        FixtureParam{"narrow_aligned_multibeat.yaml",   "",                                    false, true},
        FixtureParam{"sparse_multibeat.yaml",           "",                                    false, true},
        // Phase C: AXI4 exclusive access (IHI 0022 §A7).
        // - exclusive_pair_success: matched AR(excl)/AW(excl) pair → EXOKAY;
        //   memory commits; final verify-read returns the exclusive payload.
        // - exclusive_intervening_write: AR(excl) → normal AW(same addr, diff
        //   id) invalidates the tag → exclusive AW returns OKAY, memory NOT
        //   committed; final read returns the intervening normal write value.
        // - exclusive_no_prior_read: exclusive AW with no prior AR → silent
        //   OKAY, no memory commit; subsequent read returns 0s.
        // - exclusive_wrap_pair_success: 2-beat WRAP exclusive pair → EXOKAY;
        //   tag range uses wrap window.
        // file_diff skipped: dump_file emits per-beat bus image, not packed
        // user-byte layout. Scoreboard validates byte-level correctness.
        FixtureParam{"exclusive_pair_success.yaml",         "",  false, true},
        FixtureParam{"exclusive_intervening_write.yaml",    "",  false, true},
        FixtureParam{"exclusive_no_prior_read.yaml",        "",  false, true},
        FixtureParam{"exclusive_wrap_pair_success.yaml",    "",  false, true}),
    FixtureName{});
