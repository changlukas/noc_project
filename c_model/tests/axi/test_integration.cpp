// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "common/scenario.hpp"
#include "scenarios_list.hpp"
#include <algorithm>
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

// Returns the resolved write data_file for the first write txn that has one,
// else empty string.
static std::string first_write_data_file(axi::Scenario const& sc) {
    for (auto const& t : sc.transactions) {
        if (t.op == axi::ScenarioTransaction::Op::Write && !t.data_file.empty()) {
            return t.data_file;
        }
    }
    return "";
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

class IntegrationP : public ::testing::TestWithParam<std::string_view> {};

TEST_P(IntegrationP, RunsToCompletion) {
    SCENARIO("axi integration: scenario runs to completion under watchdog");
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id + "/scenario.yaml";

    // Validate-before-skip: malformed YAML must fail loudly.
    auto sc = axi::load_scenario(scenario_path);

    // INF scenarios are reserved for dedicated tests.
    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }

    auto write_data = first_write_data_file(sc);
    std::string wpath = write_data.empty()
                            ? std::string{}
                            : (std::string(SCENARIO_TREE_ROOT) + scenario_id + "/" + write_data);
    std::string rpath = std::string(::testing::TempDir()) + "/" + scenario_id + ".read.txt";
    auto r = run_scenario(scenario_path, wpath, rpath);

    EXPECT_EQ(r.scoreboard_mismatches, 0u) << "scoreboard mismatches: " << scenario_id;
    EXPECT_LE(r.cycle_count, kMaxCycles) << "watchdog tripped: " << scenario_id;
}

INSTANTIATE_TEST_SUITE_P(AxiFixtures, IntegrationP,
                         ::testing::ValuesIn(router::tests::kAllAxi4Scenarios),
                         [](::testing::TestParamInfo<std::string_view> const& info) {
                             std::string name{info.param};
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
