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
    bool file_diff_pass;
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

    axi::Memory mem(sc.config.memory_base, sc.config.memory_size, sc.config.write_latency,
                    sc.config.read_latency);
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(sc.config.memory_base, sc.config.memory_size);
    axi::AxiMasterT<axi::AxiSlave> master(yaml_path, slave, read_dump_path,
                                          sc.config.max_outstanding_write,
                                          sc.config.max_outstanding_read);
    axi::Scoreboard sb;
    master.on_write_completed([&](const axi::WriteResult& wr) {
        sb.handle_write_completed(wr, wr.data, wr.strb_per_beat);
    });
    master.on_read_observed([&](const axi::ReadResult& rr) { sb.handle_read_observed(rr); });

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
    bool expect_file_diff_pass;
    bool expect_zero_mismatches;
};

// Generates a readable name for each TEST_P instance, e.g.
// AxiFixtures/IntegrationP.RunFixture/single_write_read_aligned
// The param value is "<layer>/<name>"; gtest names disallow '/', so we
// keep only the trailing <name> component.
struct FixtureName {
    std::string operator()(const ::testing::TestParamInfo<FixtureParam>& info) const {
        auto n = info.param.yaml;
        auto slash = n.rfind('/');
        if (slash != std::string::npos) n = n.substr(slash + 1);
        return n;
    }
};

class IntegrationP : public ::testing::TestWithParam<FixtureParam> {};

// p.yaml is a path stem like "common/single_write_read_aligned" or
// "c-model-only/sparse_multibeat" — the layer prefix lets a single test list
// span common/ and c-model-only/ scenarios. The scenario.yaml path is built
// from SCENARIO_TREE_ROOT (injected by CMake) so the test runs from any cwd.
TEST_P(IntegrationP, RunFixture) {
    SCENARIO(
        "axi integration: end-to-end YAML fixture runs Master+Slave+Mem to completion under "
        "watchdog");
    auto p = GetParam();
    std::string yaml_path =
        std::string(SCENARIO_TREE_ROOT) + p.yaml + "/scenario.yaml";
    std::string wpath = p.write_data.empty()
                            ? std::string{}
                            : (std::string(SCENARIO_TREE_ROOT) + p.yaml + "/" + p.write_data);
    // Use scenario name (last path component of p.yaml) for the read-dump
    // temp filename so the path stays readable and short.
    auto last_slash = p.yaml.rfind('/');
    std::string short_name = (last_slash == std::string::npos) ? p.yaml : p.yaml.substr(last_slash + 1);
    std::string rpath = std::string(::testing::TempDir()) + "/" + short_name + ".read.txt";
    auto r = run_scenario(yaml_path, wpath, rpath);
    if (p.expect_file_diff_pass) {
        EXPECT_TRUE(r.file_diff_pass)
            << "file diff failed: " << p.yaml << " (wpath=" << wpath << ", rpath=" << rpath << ")";
    }
    if (p.expect_zero_mismatches) {
        EXPECT_EQ(r.scoreboard_mismatches, 0u) << "scoreboard mismatches: " << p.yaml;
    }
    EXPECT_LE(r.cycle_count, kMaxCycles) << "watchdog tripped: " << p.yaml;
}

// FixtureParam.yaml encodes "<layer>/<scenario-dir>" (no .yaml suffix);
// FixtureParam.write_data is the diff target's bare filename inside the
// scenario dir (typically "data.txt"; empty = skip file diff).
INSTANTIATE_TEST_SUITE_P(
    AxiFixtures, IntegrationP,
    ::testing::Values(
        FixtureParam{"common/single_write_read_aligned", "data.txt", true, true},
        FixtureParam{"common/burst_incr_2beat", "data.txt", true, true},
        FixtureParam{"common/burst_incr_8beat", "data.txt", true, true},
        FixtureParam{"common/multi_txn_same_id", "data.txt", true, true},
        FixtureParam{"common/multi_txn_diff_id", "data.txt", true, true},
        FixtureParam{"c-model-only/decerr_oob_write", "", false, true},
        FixtureParam{"c-model-only/decerr_oob_read", "", false, true},
        FixtureParam{"c-model-only/latency_stress", "data.txt", true, true},
        FixtureParam{"c-model-only/single_read_default_fill", "", false, true},
        FixtureParam{"c-model-only/burst_crosses_oob_boundary", "", false, true},
        // backpressure_retry: 4 concurrent writes + 4 concurrent reads on the
        // same addresses race each other (no AXI write-before-read ordering),
        // so the read dump is non-deterministic relative to write completion.
        // We verify watchdog + scoreboard only; file diff is skipped.
        FixtureParam{"common/backpressure_retry", "", false, true},
        FixtureParam{"c-model-only/multi_outstanding_stress", "data.txt", true, true},
        // Phase B-2: INCR with unaligned start addr 0x1005 size=5; first beat
        // WSTRB clears lanes 0..4. Read dump cannot byte-match the write data
        // (alignment differs); scoreboard validates byte-level correctness.
        // NOTE: c_model emits 1 beat where AXI4 spec would require 2 (the burst
        // logically crosses lane 31 into a 2nd bus word). Data file uses
        // trailing 0 padding to compensate; only 27 user bytes are committed.
        FixtureParam{"c-model-only/unaligned_start", "", false, true},
        // Phase B-3b: aligned narrow bursts. data_file is packed user bytes
        // ((len+1)*bpb total); read dump is per-beat bus-image and does not
        // match the packed layout, so file diff is skipped. Scoreboard
        // validates byte-level write->read equivalence.
        FixtureParam{"c-model-only/narrow_transfer_size2", "", false, true},
        FixtureParam{"c-model-only/narrow_transfer_size0", "", false, true},
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
        FixtureParam{"c-model-only/wrap_burst_aligned", "data.txt", true, true},
        FixtureParam{"c-model-only/wrap_burst_actual_wrap", "data.txt", true, true},
        FixtureParam{"c-model-only/fixed_burst", "", false, true},
        // Phase B-5a: INCR txn at 0x0FE0 size=5 len=7 (256B) crosses the 4KB
        // boundary at 0x1000. AxiMaster auto-segments into 1-beat + 7-beat
        // sub-bursts via split_into_sub_bursts. Both addr=0x0FE0 and 0x1000
        // are 32-byte aligned, so byte_lane=0 throughout — the read dump
        // matches the packed data file byte-for-byte.
        FixtureParam{"c-model-only/cross_4kb_auto_split", "data.txt", true, true},
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
        FixtureParam{"c-model-only/narrow_aligned_multibeat", "", false, true},
        FixtureParam{"c-model-only/sparse_multibeat", "", false, true},
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
        FixtureParam{"c-model-only/exclusive_pair_success", "", false, true},
        FixtureParam{"c-model-only/exclusive_intervening_write", "", false, true},
        FixtureParam{"c-model-only/exclusive_no_prior_read", "", false, true},
        FixtureParam{"c-model-only/exclusive_wrap_pair_success", "", false, true}),
    FixtureName{});
