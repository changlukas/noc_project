#include "axi/scenario_parser.hpp"
#include "common/isolated_scenario.hpp"
#include "nmu/addr_trans.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <string>

namespace axi = ni::cmodel::axi;
using ni::cmodel::testing::write_isolated_scenario;

namespace {
// Build a two-transaction scenario in-memory with a strb_file/lock/qos set on
// the 2nd txn so the round-trip can prove every field survives.
std::string make_source(const std::string& dir) {
    const std::string data = dir + "/iso_src_data.txt";
    {
        std::ofstream(data) << "00\n";
    }
    const std::string path = dir + "/iso_src.yaml";
    std::ofstream f(path);
    f << "schema_version: 1\n";
    f << "metadata:\n  name: AX4-BAS-003_single_write_read_aligned\n  category: basic\n";
    f << "config:\n  memory_base: 0x0\n  memory_size: 0x10000\n"
         "  write_latency: 2\n  read_latency: 3\n"
         "  max_outstanding_write: 4\n  max_outstanding_read: 5\n";
    f << "transactions:\n";
    f << "  - op: read\n    addr: 0x40\n    id: 0x1\n    len: 0\n    size: 3\n"
         "    burst: INCR\n    dump_file: unused\n";
    f << "  - op: write\n    addr: 0x80\n    id: 0x2\n    len: 1\n    size: 3\n"
         "    burst: INCR\n    data_file: "
      << data << "\n    lock: exclusive\n    qos: 7\n";
    return path;
}
}  // namespace

TEST(IsolatedScenario, PreservesEveryFieldAndRemapsDestination) {
    const std::string dir = ::testing::TempDir();
    const axi::Scenario src = axi::load_scenario(make_source(dir));

    const uint64_t offset = 0x100000000ull;  // sets bit 32 -> dst_id 1
    const std::string out = dir + "/iso_out.yaml";
    write_isolated_scenario(src, /*txn_index=*/1, offset, out);

    const axi::Scenario got = axi::load_scenario(out);
    ASSERT_EQ(got.transactions.size(), 1u);
    const auto& t = got.transactions.front();

    // Destination remapped: native 0x80 -> 0x100000080 routes to node 1.
    EXPECT_EQ(t.addr, 0x80ull + offset);
    EXPECT_EQ(ni::cmodel::nmu::addr_trans::xy_route(t.addr).dst_id, 0x01u);
    // memory_base shifted by the same offset so the NSU Memory covers it.
    EXPECT_EQ(got.config.memory_base, offset);

    // Every other field preserved.
    EXPECT_EQ(t.op, axi::ScenarioTransaction::Op::Write);
    EXPECT_EQ(t.id, 0x2u);
    EXPECT_EQ(t.len, 1u);
    EXPECT_EQ(t.size, 3u);
    EXPECT_EQ(t.burst, axi::Burst::INCR);
    EXPECT_EQ(t.lock, axi::LockType::Exclusive);
    EXPECT_EQ(t.qos, 7u);
    EXPECT_EQ(got.config.write_latency, 2u);
    EXPECT_EQ(got.config.read_latency, 3u);
    EXPECT_EQ(got.config.max_outstanding_write, 4u);
    EXPECT_EQ(got.config.max_outstanding_read, 5u);
}
