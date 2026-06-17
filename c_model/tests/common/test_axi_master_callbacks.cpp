#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scenario_parser.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <vector>

namespace axi = ni::cmodel::axi;

namespace {
// Minimal single-write + single-read scenario, 1 beat, 1 byte.
// The parser requires data_file for write ops and dump_file for read ops;
// schema_version is omitted (non-strict mode) to avoid metadata regex checks.
std::string write_inline_scenario() {
    const std::string dir = std::string(::testing::TempDir()) + "/cb_scn";
    const std::string data_path = dir + ".data";
    const std::string dump_path = dir + ".rdump";
    const std::string yaml_path = dir + ".yaml";

    std::ofstream(data_path) << "AA\n";
    {
        std::ofstream dummy(dump_path);
    }  // create empty placeholder for read dump

    std::ofstream yf(yaml_path);
    yf << "transactions:\n"
       << "  - op: write\n"
       << "    id: 0\n"
       << "    addr: 0\n"
       << "    size: 0\n"
       << "    len: 0\n"
       << "    burst: INCR\n"
       << "    data_file: " << data_path << "\n"
       << "    strb_file: \"\"\n"
       << "  - op: read\n"
       << "    id: 0\n"
       << "    addr: 0\n"
       << "    size: 0\n"
       << "    len: 0\n"
       << "    burst: INCR\n"
       << "    dump_file: " << dump_path << "\n";

    return yaml_path;
}
}  // namespace

TEST(AxiMasterCallbacks, FanoutAndIssueBeforeCompletion) {
    const std::string scn = write_inline_scenario();
    axi::Memory mem(/*base=*/0, /*size=*/4096, /*write_latency_ticks=*/0,
                    /*read_latency_ticks=*/0);
    axi::AxiSlave slave(mem);
    // axi::AxiMaster = AxiMasterT<AxiSlave> (axi_master.hpp:520). 3rd arg is the
    // read-dump path (the master opens it on construction).
    axi::AxiMaster master(scn, slave, std::string(::testing::TempDir()) + "/cb.read.txt",
                          /*max_out_w=*/1, /*max_out_r=*/1);

    int wc1 = 0, wc2 = 0, w_issue = 0;
    std::size_t issue_line = 999, complete_line = 888;
    bool issue_seen_before_complete = false;

    master.on_write_issued([&](const axi::IssueInfo& ii) {
        ++w_issue;
        issue_line = ii.scenario_line;
    });
    master.on_write_completed([&](const axi::WriteResult& wr) {
        ++wc1;
        complete_line = wr.scenario_line;
        issue_seen_before_complete = (w_issue == 1);
    });
    master.on_write_completed([&](const axi::WriteResult&) { ++wc2; });

    for (int cycle = 0; cycle < 200 && !master.done(); ++cycle) {
        master.tick();
        slave.tick();
        mem.tick();
    }
    ASSERT_TRUE(master.done());
    EXPECT_EQ(wc1, 1);  // first subscriber fired
    EXPECT_EQ(wc2, 1);  // second subscriber fired (H1: not overwritten)
    EXPECT_EQ(w_issue, 1);
    EXPECT_TRUE(issue_seen_before_complete);
    EXPECT_EQ(issue_line, complete_line);
}
