// Unit tests for the test_logger framework: AxiMasterObserver.
//
// These exercise the observer's invariants in isolation by using the
// test-only no-master ctor + test_inject_* injection methods, without
// spinning up a full AxiMaster scenario. Coverage:
//   - OrderedBPass:       happy path via real AxiMaster + scenario YAML.
//   - OutOfOrderBFail:    AXI4 §A5.3 per-id B order violation auto-detect.
//   - NonOkayResp:        SLVERR bumps mismatches_ but does not fail by default.
//   - StuckCountMismatch: RAII destructor path fires print_summary() with
//                         stuck-write FAIL context (smoke).
//   - OutOfOrderRFail:    AXI4 §A5.3 per-id R order violation auto-detect.
#include "common/test_logger.hpp"
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

using ni::cmodel::testing::AxiMasterObserver;
namespace axi = ni::cmodel::axi;

namespace {

// Build a tiny AxiMaster + AxiSlave + Memory triple from an inline scenario
// YAML body. Backing files (data, scenario, read dump) live under TempDir so
// the harness cleans up.
struct TestRig {
    axi::Memory                       memory{0, 0x10000, 0, 0};
    axi::AxiSlave                     slave{memory};
    std::string                       yaml_path;
    std::string                       data_path;
    std::string                       dump_path;
    std::unique_ptr<axi::AxiMaster>   master;

    TestRig(std::size_t max_out_w = 2, std::size_t max_out_r = 2) {
        const std::string tmp = ::testing::TempDir();
        yaml_path = tmp + "/test_logger_obs.yaml";
        data_path = tmp + "/test_logger_obs_data.hex";
        dump_path = tmp + "/test_logger_obs_rdump.txt";

        // One beat of size=5 (32 bytes) of zero data — shared by both writes.
        {
            std::ofstream df(data_path);
            for (int i = 0; i < 32; ++i) df << "00 ";
            df << '\n';
        }

        // Two same-id writes, distinct addresses, in submission order.
        {
            std::ofstream yf(yaml_path);
            yf << "transactions:\n"
               << "  - op: write\n"
               << "    id: 0x05\n"
               << "    addr: 0x100\n"
               << "    size: 5\n"
               << "    len: 0\n"
               << "    burst: INCR\n"
               << "    data_file: " << data_path << "\n"
               << "    strb_file: \"\"\n"
               << "  - op: write\n"
               << "    id: 0x05\n"
               << "    addr: 0x200\n"
               << "    size: 5\n"
               << "    len: 0\n"
               << "    burst: INCR\n"
               << "    data_file: " << data_path << "\n"
               << "    strb_file: \"\"\n";
        }

        master = std::make_unique<axi::AxiMaster>(
            yaml_path, slave, dump_path, max_out_w, max_out_r);
    }

    void run_to_completion(int max_cycles = 200) {
        for (int i = 0; i < max_cycles && !master->done(); ++i) {
            master->tick();
            slave.tick();
            memory.tick();
        }
    }
};

}  // namespace

TEST(AxiMasterObserver, OrderedBPass) {
    SCENARIO("Observer: 2 same-id writes complete in submission order -> ok()=true, no FAIL");
    TestRig rig;
    AxiMasterObserver obs(*rig.master, "Test");
    rig.run_to_completion();
    EXPECT_TRUE(obs.ok());
    EXPECT_EQ(obs.aw_count(), 2u);
    EXPECT_EQ(obs.b_count(),  2u);
    EXPECT_EQ(obs.mismatches(), 0u);
}

TEST(AxiMasterObserver, OutOfOrderBFail) {
    SCENARIO("Observer: same-id B arrives out of submission order -> ok()=false, FAIL context emitted");
    AxiMasterObserver obs("Test");
    // First B: scenario_line=7 (later submission).
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb_per_beat=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/7});
    // Second B: scenario_line=5 (older submission) -> violation.
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x200, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb_per_beat=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_FALSE(obs.ok());
    EXPECT_EQ(obs.failures().size(), 1u);
}

TEST(AxiMasterObserver, NonOkayResp) {
    SCENARIO("Observer: B with SLVERR -> mismatches++, no FAIL by default");
    AxiMasterObserver obs("Test");
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb_per_beat=*/{},
        /*resp=*/axi::Resp::SLVERR, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_TRUE(obs.ok());           // mismatches alone don't fail
    EXPECT_EQ(obs.mismatches(), 1u);
}

TEST(AxiMasterObserver, StuckCountMismatch) {
    SCENARIO("Observer: aw_count > b_count at end of test -> stuck-write FAIL surfaced");
    AxiMasterObserver obs("Test");
    obs.test_inject_write_result(axi::WriteResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        axi::LockType::Normal, /*data=*/{}, /*strb_per_beat=*/{},
        /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    // Synthetic: 2 AWs issued, only 1 B observed.
    obs.test_set_aw_count(2);
    obs.print_summary();
    EXPECT_FALSE(obs.ok());
    ASSERT_EQ(obs.failures().size(), 1u);
    EXPECT_NE(obs.failures()[0].find("stuck_writes"), std::string::npos);
}

TEST(AxiMasterObserver, OutOfOrderRFail) {
    SCENARIO("Observer: same-id R arrives out of submission order -> ok()=false, R-order FAIL");
    AxiMasterObserver obs("Test");
    obs.test_inject_read_result(axi::ReadResult{
        /*addr=*/0x100, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        /*data=*/{}, /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/7});
    obs.test_inject_read_result(axi::ReadResult{
        /*addr=*/0x200, /*size=*/5, /*len=*/0, axi::Burst::INCR,
        /*data=*/{}, /*resp=*/axi::Resp::OKAY, /*id=*/0x05, /*scenario_line=*/5});
    EXPECT_FALSE(obs.ok());
    ASSERT_EQ(obs.failures().size(), 1u);
    // Distinguishes from B-order: failure context says "R" not "B".
    EXPECT_NE(obs.failures()[0].find("expected R in submission order"),
              std::string::npos);
}
