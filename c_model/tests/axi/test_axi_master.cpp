// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/scenario_parser.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>

namespace axi = ni::cmodel::axi;

class ScenarioParser : public ::testing::Test {
protected:
  std::string write_tmp(const std::string& contents) {
    auto path = std::string(::testing::TempDir()) + "/scenario.yaml";
    std::ofstream f(path); f << contents; return path;
  }
};

TEST_F(ScenarioParser, MinimalWriteReadScenario) {
  auto path = write_tmp(R"YAML(
config:
  memory_base: 0x1000
  memory_size: 0x1000
transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: w.txt
  - op: read
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    dump_file: r.txt
)YAML");
  auto sc = axi::load_scenario(path);
  EXPECT_EQ(sc.config.memory_base, 0x1000u);
  ASSERT_EQ(sc.transactions.size(), 2u);
  EXPECT_EQ(sc.transactions[0].op, axi::ScenarioTransaction::Op::Write);
  EXPECT_EQ(sc.transactions[0].data_file, "w.txt");
  EXPECT_EQ(sc.transactions[1].dump_file, "r.txt");
}

TEST_F(ScenarioParser, DefaultsAppliedWhenConfigOmitted) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x0
    id: 0
    len: 0
    size: 0
    burst: INCR
    dump_file: r.txt
)YAML");
  auto sc = axi::load_scenario(path);
  EXPECT_EQ(sc.config.memory_base,   0u);
  EXPECT_EQ(sc.config.memory_size,   0x10000u);
  EXPECT_EQ(sc.config.write_latency, 1u);
  EXPECT_EQ(sc.config.max_outstanding_write, 1u);
}

TEST_F(ScenarioParser, UnknownConfigFieldThrows) {
  auto path = write_tmp(R"YAML(
config:
  bogus_field: 123
transactions:
  - op: read
    addr: 0
    id: 0
    len: 0
    size: 0
    burst: INCR
    dump_file: r.txt
)YAML");
  EXPECT_THROW(axi::load_scenario(path), std::runtime_error);
}

// Phase B-4: WRAP burst is now accepted with constraints (len ∈ {1,3,7,15}
// and addr aligned to (1<<size)). FIXED is accepted unconditionally.
TEST_F(ScenarioParser, WrapAcceptedWithValidLen) {
  for (uint8_t len : {uint8_t(1), uint8_t(3), uint8_t(7), uint8_t(15)}) {
    std::ostringstream y;
    y << "\ntransactions:\n"
         "  - op: read\n"
         "    addr: 0\n"
         "    id: 0\n"
         "    len: " << static_cast<unsigned>(len) << "\n"
         "    size: 5\n"
         "    burst: WRAP\n"
         "    dump_file: r.txt\n";
    auto path = write_tmp(y.str());
    EXPECT_NO_THROW(axi::load_scenario(path)) << "len=" << int(len);
  }
}

TEST_F(ScenarioParser, WrapRejectedWithInvalidLen) {
  for (uint8_t len : {uint8_t(0), uint8_t(2), uint8_t(4), uint8_t(5),
                       uint8_t(6), uint8_t(8), uint8_t(9), uint8_t(16)}) {
    std::ostringstream y;
    y << "\ntransactions:\n"
         "  - op: read\n"
         "    addr: 0\n"
         "    id: 0\n"
         "    len: " << static_cast<unsigned>(len) << "\n"
         "    size: 5\n"
         "    burst: WRAP\n"
         "    dump_file: r.txt\n";
    auto path = write_tmp(y.str());
    EXPECT_THROW(axi::load_scenario(path), std::runtime_error) << "len=" << int(len);
  }
}

TEST_F(ScenarioParser, WrapRejectedWithUnalignedAddr) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x1003
    id: 0
    len: 3
    size: 5
    burst: WRAP
    dump_file: r.txt
)YAML");
  EXPECT_THROW(axi::load_scenario(path), std::runtime_error);
}

TEST_F(ScenarioParser, FixedAccepted) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x1000
    id: 0
    len: 3
    size: 5
    burst: FIXED
    dump_file: r.txt
)YAML");
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].burst, axi::Burst::FIXED);
}

TEST_F(ScenarioParser, IncrUnalignedAccepted_PhaseB) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x1003
    id: 0
    len: 0
    size: 5
    burst: INCR
    dump_file: r.txt
)YAML");
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].addr, 0x1003u);
  EXPECT_EQ(sc.transactions[0].burst, axi::Burst::INCR);
}

TEST_F(ScenarioParser, StrbFileFieldAccepted) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: w.txt
    strb_file: s.txt
)YAML");
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].strb_file, "s.txt");
}

TEST_F(ScenarioParser, StrbFileOptional) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: w.txt
)YAML");
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].strb_file, "");
}

TEST_F(ScenarioParser, ReadTxnIgnoresStrbFile) {
  auto path = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x0
    id: 0x2
    len: 0
    size: 5
    burst: INCR
    dump_file: r.txt
    strb_file: s.txt
)YAML");
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].strb_file, "");
}

// Phase C: lock field parsing. Uses per-test unique tempfile names because
// the shared write_tmp() helper races with parallel ctest runners.
TEST_F(ScenarioParser, LockNormalAccepted) {
  std::string tmp_name = std::string("/lock_normal_") +
      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".yaml";
  auto path = std::string(::testing::TempDir()) + tmp_name;
  std::ofstream(path) << R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    lock: normal
    data_file: w.txt
)YAML";
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].lock, axi::LockType::Normal);
}

TEST_F(ScenarioParser, LockExclusiveAccepted) {
  std::string tmp_name = std::string("/lock_excl_") +
      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".yaml";
  auto path = std::string(::testing::TempDir()) + tmp_name;
  std::ofstream(path) << R"YAML(
transactions:
  - op: read
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    lock: exclusive
    dump_file: r.txt
)YAML";
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].lock, axi::LockType::Exclusive);
}

TEST_F(ScenarioParser, LockDefaultsToNormal) {
  std::string tmp_name = std::string("/lock_default_") +
      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".yaml";
  auto path = std::string(::testing::TempDir()) + tmp_name;
  std::ofstream(path) << R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: w.txt
)YAML";
  auto sc = axi::load_scenario(path);
  ASSERT_EQ(sc.transactions.size(), 1u);
  EXPECT_EQ(sc.transactions[0].lock, axi::LockType::Normal);
}

TEST_F(ScenarioParser, LockInvalidStringThrows) {
  std::string tmp_name = std::string("/lock_invalid_") +
      ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".yaml";
  auto path = std::string(::testing::TempDir()) + tmp_name;
  std::ofstream(path) << R"YAML(
transactions:
  - op: read
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    lock: foo
    dump_file: r.txt
)YAML";
  EXPECT_THROW(axi::load_scenario(path), std::runtime_error);
}

#include "axi/axi_master.hpp"
#include "mock_slave.hpp"

class AxiMasterTest : public ScenarioParser {};

TEST_F(AxiMasterTest, ConstructsFromYamlAndOpensDump) {
  auto wpath = std::string(::testing::TempDir()) + "/w.txt";
  std::ofstream(wpath) << "AB CD EF 12 34 56 78 9A BC DE F0 11 22 33 44 55 "
                          "66 77 88 99 AA BB CC DD EE FF 00 11 22 33 44 55\n";
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_out.txt", 1, 1);
  EXPECT_FALSE(master.done());
}

TEST_F(AxiMasterTest, SingleWriteTransactionExecutes) {
  auto wpath = std::string(::testing::TempDir()) + "/w_single.txt";
  std::ofstream(wpath) << "01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 "
                          "11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20\n";
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x7
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r.txt");

  bool fired = false;
  master.on_write_completed([&](const axi::WriteResult& r) {
    fired = true;
    EXPECT_EQ(r.id, 7);
    EXPECT_EQ(r.resp, axi::Resp::OKAY);
  });

  master.tick();
  EXPECT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_w.size(),  1u);
  EXPECT_EQ(mock.captured_aw[0].id, 7);
  EXPECT_EQ(mock.captured_w[0].data[0], 0x01);
  EXPECT_EQ(mock.captured_w[0].last, true);

  mock.queued_b.push_back(axi::BBeat{7, axi::Resp::OKAY, 0});
  master.tick();
  EXPECT_TRUE(fired);
  EXPECT_TRUE(master.done());
}

TEST_F(AxiMasterTest, SingleReadTransactionDumpsToFile) {
  auto dumpPath = std::string(::testing::TempDir()) + "/r_single.txt";
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: read
    addr: 0x0
    id: 0x9
    len: 0
    size: 5
    burst: INCR
    dump_file: )YAML") + dumpPath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(yaml, mock, dumpPath);

  master.tick();
  EXPECT_EQ(mock.captured_ar.size(), 1u);

  axi::RBeat r{}; r.id = 9; r.data.fill(0xAB);
  r.resp = axi::Resp::OKAY; r.last = true; r.user = 0;
  mock.queued_r.push_back(r);
  master.tick();
  EXPECT_TRUE(master.done());

  std::ifstream f(dumpPath); std::string line; std::getline(f, line);
  EXPECT_EQ(line.substr(0, 5), "AB AB");
}

TEST_F(AxiMasterTest, MaxOutstandingWriteLimitsConcurrency) {
  auto wpath = std::string(::testing::TempDir()) + "/w_concur.txt";
  std::ofstream(wpath) << "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF "
                          "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF\n";
  auto yaml = write_tmp(std::string(R"YAML(
config:
  max_outstanding_write: 2
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
  - op: write
    addr: 0x20
    id: 0x2
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML" + wpath + R"YAML(
  - op: write
    addr: 0x40
    id: 0x3
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML" + wpath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r.txt", 2, 1);
  master.tick();
  EXPECT_EQ(mock.captured_aw.size(), 2u);
  EXPECT_EQ(mock.captured_aw[0].id, 1);
  EXPECT_EQ(mock.captured_aw[1].id, 2);

  mock.queued_b.push_back(axi::BBeat{1, axi::Resp::OKAY, 0});
  master.tick();
  EXPECT_EQ(mock.captured_aw.size(), 3u);
  EXPECT_EQ(mock.captured_aw[2].id, 3);
}

TEST_F(AxiMasterTest, StrbFileMissingThrows) {
  auto wpath = std::string(::testing::TempDir()) + "/w_missing_strb.txt";
  std::ofstream(wpath) << "01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 "
                          "11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20\n";
  auto strb_missing = std::string(::testing::TempDir()) + "/does_not_exist.strb";
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
    strb_file: )YAML" + strb_missing + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_missing.txt");
  EXPECT_THROW(master.tick(), std::runtime_error);
}

TEST_F(AxiMasterTest, StrbFileLineCountMismatchThrows) {
  auto wpath = std::string(::testing::TempDir()) + "/w_lc.txt";
  std::ofstream(wpath) << "01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 "
                          "11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20\n";
  // expected beats = len + 1 = 1, but provide 2 strb tokens
  auto spath = std::string(::testing::TempDir()) + "/s_lc.txt";
  std::ofstream(spath) << "FFFFFFFF FFFFFFFF\n";
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x1
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
    strb_file: )YAML" + spath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_lc.txt");
  EXPECT_THROW(master.tick(), std::runtime_error);
}

// Phase B-2.2: AxiMaster aligns AW.addr DOWN to (1<<size) and masks first-beat
// WSTRB lanes 0..first_lane-1 where first_lane = txn.addr & (DATA_BYTES - 1).

namespace {
// Write 32 arbitrary non-zero bytes to a tmp data file and return the path.
std::string write_32byte_tmp_data(const std::string& tag) {
  auto path = std::string(::testing::TempDir()) + "/" + tag + ".dat";
  std::ofstream f(path);
  for (int j = 0; j < 32; ++j) {
    if (j) f << ' ';
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02X",
                  static_cast<unsigned>(0x40u + (j & 0x3Fu)));
    f << buf;
  }
  f << '\n';
  return path;
}

struct UnalignedCase {
  uint64_t    txn_addr;
  uint8_t     size;
  uint64_t    expected_aw_addr;
  // expected_strb = ((1 << (1<<size)) - 1) << (txn_addr & (DATA_BYTES-1)),
  // truncated to uint32. AXI4 lane-positioned semantic: only the bus lanes
  // [byte_lane, byte_lane + bpb) carry valid bytes for this beat.
  uint32_t    expected_strb;
  const char* label;  // GoogleTest case name suffix
};
}  // namespace

class AxiMasterUnalignedP
    : public AxiMasterTest,
      public ::testing::WithParamInterface<UnalignedCase> {};

TEST_P(AxiMasterUnalignedP, FirstBeatStrbMaskedAndAwAligned) {
  const auto& c = GetParam();
  auto wpath = write_32byte_tmp_data(std::string("u_") + c.label);
  std::ostringstream yaml_src;
  yaml_src << "\ntransactions:\n"
              "  - op: write\n"
              "    addr: 0x" << std::hex << c.txn_addr << "\n"
              "    id: 0x1\n"
              "    len: 0\n"
              "    size: " << std::dec << static_cast<unsigned>(c.size) << "\n"
              "    burst: INCR\n"
              "    data_file: " << wpath << "\n";
  auto yaml = write_tmp(yaml_src.str());
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock,
      std::string(::testing::TempDir()) + "/r_u_" + c.label + ".txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].addr, c.expected_aw_addr);
  ASSERT_FALSE(mock.captured_w.empty());
  EXPECT_EQ(mock.captured_w[0].strb, c.expected_strb);
  EXPECT_EQ(mock.captured_w[0].last, true);
}

INSTANTIATE_TEST_SUITE_P(
    UnalignedCases, AxiMasterUnalignedP,
    ::testing::Values(
        UnalignedCase{0x1003, 5, 0x1000u, 0xFFFFFFF8u, "Size5_Off3"},
        UnalignedCase{0x1001, 4, 0x1000u, 0x0001FFFEu, "Size4_Off1"},
        UnalignedCase{0x1007, 3, 0x1000u, 0x00007F80u, "Size3_Off7"},
        UnalignedCase{0x100F, 2, 0x100Cu, 0x00078000u, "Size2_OffF"},
        UnalignedCase{0x101F, 1, 0x101Eu, 0x80000000u, "Size1_Off1F"}),
    [](const ::testing::TestParamInfo<UnalignedCase>& info) {
      return std::string(info.param.label);
    });

TEST_F(AxiMasterTest, StrbFilePropagatesToWChannel) {
  // 2-beat write with sparse first beat (0x0F) and full second beat (0xFFFFFFFF).
  // Verify both strb masks ride the W channel in order.
  auto wpath = std::string(::testing::TempDir()) + "/w_strb_prop.txt";
  // 2 beats * DATA_BYTES bytes; arbitrary data, content not under test.
  {
    std::ofstream wf(wpath);
    for (int beat = 0; beat < 2; ++beat) {
      for (int j = 0; j < axi::DATA_BYTES; ++j) {
        if (j) wf << ' ';
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X",
                      static_cast<unsigned>((beat * axi::DATA_BYTES + j) & 0xFFu));
        wf << buf;
      }
      wf << '\n';
    }
  }
  auto spath = std::string(::testing::TempDir()) + "/s_strb_prop.txt";
  std::ofstream(spath) << "0000000F\nFFFFFFFF\n";

  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0
    id: 0x4
    len: 1
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
    strb_file: )YAML" + spath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_strb_prop.txt");

  master.tick();
  ASSERT_EQ(mock.captured_w.size(), 2u);
  EXPECT_EQ(mock.captured_w[0].strb, 0x0000000Fu);
  EXPECT_EQ(mock.captured_w[0].last, false);
  EXPECT_EQ(mock.captured_w[1].strb, 0xFFFFFFFFu);
  EXPECT_EQ(mock.captured_w[1].last, true);
}

// Phase B-3b: AxiMaster aligned narrow transfers (size<5). bpb = 1<<size;
// lane_mask = ((1<<bpb)-1) << byte_lane; user bytes land on bus lanes
// [byte_lane, byte_lane+bpb).
namespace {
// Write an arbitrary hex byte string to a tmp file. Variable-size companion
// to write_32byte_tmp_data (which is hardcoded to 32 bytes).
std::string write_hex_tmp_data(const std::string& tag, const std::string& hex_bytes) {
  auto path = std::string(::testing::TempDir()) + "/" + tag + ".dat";
  std::ofstream f(path);
  f << hex_bytes << '\n';
  return path;
}
}  // namespace

TEST_F(AxiMasterTest, NarrowSize0_1BytePerBeat) {
  // addr=0x1000 size=0 len=3: 4 beats x 1 byte. Beat n: byte_lane = n,
  // strb = 1 << n, data[n] = user[n].
  auto wpath = write_hex_tmp_data("narrow_s0", "AA BB CC DD");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x1
    len: 3
    size: 0
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_narrow_s0.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].addr, 0x1000u);
  EXPECT_EQ(mock.captured_aw[0].size, 0u);
  EXPECT_EQ(mock.captured_aw[0].len,  3u);
  ASSERT_EQ(mock.captured_w.size(), 4u);
  const uint8_t expected_bytes[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  for (std::size_t n = 0; n < 4; ++n) {
    EXPECT_EQ(mock.captured_w[n].strb, 1u << n) << "beat " << n;
    EXPECT_EQ(mock.captured_w[n].data[n], expected_bytes[n]) << "beat " << n;
    EXPECT_EQ(mock.captured_w[n].last, n == 3u) << "beat " << n;
  }
}

TEST_F(AxiMasterTest, NarrowSize1_2BytePerBeat) {
  // addr=0x1002 size=1 len=1: 2 beats x 2 bytes.
  // Beat 0: addr=0x1002, byte_lane=2, strb=0x3<<2=0xC, data[2..3]=user[0..1].
  // Beat 1: addr=0x1004, byte_lane=4, strb=0x3<<4=0x30, data[4..5]=user[2..3].
  auto wpath = write_hex_tmp_data("narrow_s1", "11 22 33 44");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1002
    id: 0x2
    len: 1
    size: 1
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_narrow_s1.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].addr, 0x1002u);  // aligned to bpb=2
  ASSERT_EQ(mock.captured_w.size(), 2u);
  EXPECT_EQ(mock.captured_w[0].strb, 0x0000000Cu);
  EXPECT_EQ(mock.captured_w[0].data[2], 0x11);
  EXPECT_EQ(mock.captured_w[0].data[3], 0x22);
  EXPECT_EQ(mock.captured_w[0].last, false);
  EXPECT_EQ(mock.captured_w[1].strb, 0x00000030u);
  EXPECT_EQ(mock.captured_w[1].data[4], 0x33);
  EXPECT_EQ(mock.captured_w[1].data[5], 0x44);
  EXPECT_EQ(mock.captured_w[1].last, true);
}

TEST_F(AxiMasterTest, NarrowSize2_4BytePerBeat) {
  // The canonical narrow case. addr=0x1004 size=2 len=1: 2 beats x 4 bytes.
  // Beat 0: byte_lane=4, strb=0xF<<4=0xF0,  data[4..7]=user[0..3].
  // Beat 1: byte_lane=8, strb=0xF<<8=0xF00, data[8..11]=user[4..7].
  auto wpath = write_hex_tmp_data("narrow_s2", "AB CD EF 12 34 56 78 9A");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1004
    id: 0x3
    len: 1
    size: 2
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_narrow_s2.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].addr, 0x1004u);
  ASSERT_EQ(mock.captured_w.size(), 2u);
  EXPECT_EQ(mock.captured_w[0].strb, 0x000000F0u);
  EXPECT_EQ(mock.captured_w[0].data[4], 0xAB);
  EXPECT_EQ(mock.captured_w[0].data[5], 0xCD);
  EXPECT_EQ(mock.captured_w[0].data[6], 0xEF);
  EXPECT_EQ(mock.captured_w[0].data[7], 0x12);
  EXPECT_EQ(mock.captured_w[0].last, false);
  EXPECT_EQ(mock.captured_w[1].strb, 0x00000F00u);
  EXPECT_EQ(mock.captured_w[1].data[8],  0x34);
  EXPECT_EQ(mock.captured_w[1].data[9],  0x56);
  EXPECT_EQ(mock.captured_w[1].data[10], 0x78);
  EXPECT_EQ(mock.captured_w[1].data[11], 0x9A);
  EXPECT_EQ(mock.captured_w[1].last, true);
}

TEST_F(AxiMasterTest, NarrowSize3_8BytePerBeat) {
  // addr=0x1000 size=3 len=1: 2 beats x 8 bytes.
  // Beat 0: byte_lane=0, strb=0xFF,    data[0..7] =user[0..7].
  // Beat 1: byte_lane=8, strb=0xFF<<8, data[8..15]=user[8..15].
  auto wpath = write_hex_tmp_data(
      "narrow_s3",
      "01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x4
    len: 1
    size: 3
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_narrow_s3.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].addr, 0x1000u);
  ASSERT_EQ(mock.captured_w.size(), 2u);
  EXPECT_EQ(mock.captured_w[0].strb, 0x000000FFu);
  for (std::size_t j = 0; j < 8; ++j) {
    EXPECT_EQ(mock.captured_w[0].data[j], static_cast<uint8_t>(0x01 + j));
  }
  EXPECT_EQ(mock.captured_w[0].last, false);
  EXPECT_EQ(mock.captured_w[1].strb, 0x0000FF00u);
  for (std::size_t j = 0; j < 8; ++j) {
    EXPECT_EQ(mock.captured_w[1].data[8 + j], static_cast<uint8_t>(0x09 + j));
  }
  EXPECT_EQ(mock.captured_w[1].last, true);
}

// Phase B-5a: split_into_sub_bursts. AXI4 (IHI 0022 A3.4.1) forbids INCR
// bursts crossing a 4KB boundary and caps a single burst at 256 beats. The
// helper carves a scenario_txn into a chain of legal sub-bursts.
namespace {
axi::ScenarioTransaction make_txn(uint64_t addr, uint8_t len, uint8_t size,
                                  axi::Burst burst) {
  axi::ScenarioTransaction t{};
  t.op = axi::ScenarioTransaction::Op::Write;
  t.addr = addr; t.len = len; t.size = size; t.burst = burst;
  return t;
}
}  // namespace

TEST(SplitIntoSubBursts, NoSplit_AlignedAt4KBStart) {
  auto subs = axi::split_into_sub_bursts(make_txn(0x1000, 0, 5, axi::Burst::INCR));
  ASSERT_EQ(subs.size(), 1u);
  EXPECT_EQ(subs[0].addr, 0x1000u);
  EXPECT_EQ(subs[0].len,  0u);
}

TEST(SplitIntoSubBursts, NoSplit_WithinPage) {
  // 0x1040 size=5 len=3 → 4 beats × 32B = 128B. Spans 0x1040..0x10C0,
  // entirely within page 0x1000..0x2000. 1 sub-burst.
  auto subs = axi::split_into_sub_bursts(make_txn(0x1040, 3, 5, axi::Burst::INCR));
  ASSERT_EQ(subs.size(), 1u);
  EXPECT_EQ(subs[0].addr, 0x1040u);
  EXPECT_EQ(subs[0].len,  3u);
}

TEST(SplitIntoSubBursts, Split4KBCross_2SubBursts) {
  // 0x0FE0 size=5 len=7 → 8 beats × 32B = 256B spans 0x0FE0..0x10E0.
  // Crosses 4KB at 0x1000. Beat 0 at 0x0FE0; beats 1..7 at 0x1000..0x10C0.
  auto subs = axi::split_into_sub_bursts(make_txn(0x0FE0, 7, 5, axi::Burst::INCR));
  ASSERT_EQ(subs.size(), 2u);
  EXPECT_EQ(subs[0].addr, 0x0FE0u);
  EXPECT_EQ(subs[0].len,  0u);          // 1 beat
  EXPECT_EQ(subs[1].addr, 0x1000u);
  EXPECT_EQ(subs[1].len,  6u);          // 7 beats
}

TEST(SplitIntoSubBursts, Split256BeatCap) {
  // 0x0000 size=5 len=255 → 256 beats × 32B = 8KB. Spans 2 pages.
  // Split into 128-beat halves: 128*32 = 4KB each.
  auto subs = axi::split_into_sub_bursts(make_txn(0x0000, 255, 5, axi::Burst::INCR));
  ASSERT_EQ(subs.size(), 2u);
  EXPECT_EQ(subs[0].addr, 0x0000u);
  EXPECT_EQ(subs[0].len,  127u);
  EXPECT_EQ(subs[1].addr, 0x1000u);
  EXPECT_EQ(subs[1].len,  127u);
}

TEST(SplitIntoSubBursts, WrapNoSplit) {
  // WRAP confines beats inside [wrap_lower, wrap_upper) by construction.
  // The split helper must NOT segment WRAP — its semantics differ from INCR.
  auto subs = axi::split_into_sub_bursts(make_txn(0x0FE0, 3, 5, axi::Burst::WRAP));
  ASSERT_EQ(subs.size(), 1u);
  EXPECT_EQ(subs[0].burst, axi::Burst::WRAP);
}

TEST(SplitIntoSubBursts, FixedNoSplit) {
  // FIXED reuses one address for every beat — no boundary cross to worry about.
  auto subs = axi::split_into_sub_bursts(make_txn(0x1000, 3, 5, axi::Burst::FIXED));
  ASSERT_EQ(subs.size(), 1u);
  EXPECT_EQ(subs[0].burst, axi::Burst::FIXED);
}

// Phase B-5a: AxiMaster with 4KB-crossing scenario_txn emits multiple AWs
// (one per sub-burst) sharing the same id. WriteResult fires ONCE after the
// final sub-burst's B response.
TEST_F(AxiMasterTest, Cross4KB_EmitsTwoAwsOneWriteResult) {
  // 8 beats × 32B = 256B starting at 0x0FE0 → 1 beat at 0x0FE0 + 7 beats at
  // 0x1000. Data file holds 8 × 32 = 256 user bytes.
  auto wpath = std::string(::testing::TempDir()) + "/w_cross_4kb.txt";
  {
    std::ofstream wf(wpath);
    for (int beat = 0; beat < 8; ++beat) {
      for (int j = 0; j < 32; ++j) {
        if (j) wf << ' ';
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X",
                      static_cast<unsigned>((beat * 32 + j) & 0xFFu));
        wf << buf;
      }
      wf << '\n';
    }
  }
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x0FE0
    id: 0x4
    len: 7
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + "\n");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_cross_4kb.txt");

  int write_callbacks = 0;
  master.on_write_completed([&](const axi::WriteResult& wr) {
    ++write_callbacks;
    EXPECT_EQ(wr.id, 4);
    EXPECT_EQ(wr.addr, 0x0FE0u) << "WriteResult.addr must be the ORIGINAL "
                                    "scenario_txn addr, not a sub-burst addr";
    EXPECT_EQ(wr.len,  7u);
    EXPECT_EQ(wr.size, 5u);
    EXPECT_EQ(wr.resp, axi::Resp::OKAY);
  });

  master.tick();
  // Two AWs emitted: 1-beat sub-burst at 0x0FE0, then 7-beat sub-burst at 0x1000.
  ASSERT_EQ(mock.captured_aw.size(), 2u);
  EXPECT_EQ(mock.captured_aw[0].id,   4);
  EXPECT_EQ(mock.captured_aw[0].addr, 0x0FE0u);
  EXPECT_EQ(mock.captured_aw[0].len,  0u);
  EXPECT_EQ(mock.captured_aw[1].id,   4);
  EXPECT_EQ(mock.captured_aw[1].addr, 0x1000u);
  EXPECT_EQ(mock.captured_aw[1].len,  6u);
  // 8 W beats total, with .last set on beat 0 (end of sub-burst 0) and beat 7.
  ASSERT_EQ(mock.captured_w.size(), 8u);
  EXPECT_EQ(mock.captured_w[0].last, true);   // end of sub-burst 0
  EXPECT_EQ(mock.captured_w[1].last, false);
  EXPECT_EQ(mock.captured_w[6].last, false);
  EXPECT_EQ(mock.captured_w[7].last, true);   // end of sub-burst 1

  // Only one B response so far → no callback.
  mock.queued_b.push_back(axi::BBeat{4, axi::Resp::OKAY, 0});
  master.tick();
  EXPECT_EQ(write_callbacks, 0) << "WriteResult must NOT fire after only 1/2 "
                                    "sub-burst B responses";

  // Second B response → operation complete, single callback.
  mock.queued_b.push_back(axi::BBeat{4, axi::Resp::OKAY, 0});
  master.tick();
  EXPECT_EQ(write_callbacks, 1)
      << "WriteResult must fire exactly once per scenario_txn";
  EXPECT_TRUE(master.done());
}

// Phase B-5a: 4KB-crossing read scenario_txn emits multiple ARs; the master
// accumulates R beats across sub-bursts into a single ReadResult.
TEST_F(AxiMasterTest, Cross4KB_EmitsTwoArsOneReadResult) {
  auto yaml = write_tmp(R"YAML(
transactions:
  - op: read
    addr: 0x0FE0
    id: 0xA
    len: 7
    size: 5
    burst: INCR
    dump_file: r.txt
)YAML");

  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_cross_4kb_read.txt");

  int read_callbacks = 0;
  master.on_read_observed([&](const axi::ReadResult& rr) {
    ++read_callbacks;
    EXPECT_EQ(rr.id, 0xA);
    EXPECT_EQ(rr.addr, 0x0FE0u);
    EXPECT_EQ(rr.len, 7u);
    EXPECT_EQ(rr.size, 5u);
  });

  master.tick();
  ASSERT_EQ(mock.captured_ar.size(), 2u);
  EXPECT_EQ(mock.captured_ar[0].addr, 0x0FE0u);
  EXPECT_EQ(mock.captured_ar[0].len,  0u);
  EXPECT_EQ(mock.captured_ar[1].addr, 0x1000u);
  EXPECT_EQ(mock.captured_ar[1].len,  6u);

  // Drain sub-burst 0 (1 beat, last=true).
  {
    axi::RBeat rb{}; rb.id = 0xA; rb.data.fill(0xAA);
    rb.resp = axi::Resp::OKAY; rb.last = true;
    mock.queued_r.push_back(rb);
  }
  master.tick();
  EXPECT_EQ(read_callbacks, 0) << "ReadResult must wait for sub-burst 1";

  // Drain sub-burst 1 (7 beats; last=true on beat 6).
  for (int i = 0; i < 7; ++i) {
    axi::RBeat rb{}; rb.id = 0xA; rb.data.fill(0xB0 + i);
    rb.resp = axi::Resp::OKAY; rb.last = (i == 6);
    mock.queued_r.push_back(rb);
  }
  master.tick();
  EXPECT_EQ(read_callbacks, 1)
      << "ReadResult must fire exactly once per scenario_txn";
  EXPECT_TRUE(master.done());
}

// Phase C: AxiMaster propagates scenario_txn.lock onto the AW/AR.lock wire as
// a 1-bit value (LockType::Exclusive → 1, Normal → 0). Pure wire-through —
// the master does not interpret the lock; the slave's exclusive monitor does.
TEST_F(AxiMasterTest, LockFieldPropagatesToAwLock) {
  auto wpath = write_hex_tmp_data(
      "w_lock_excl",
      "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F "
      "10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    lock: exclusive
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_lock_excl.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].lock, 1u);
}

// AR symmetry with the AW wire-through test above: lock=exclusive on a read
// scenario_txn must surface as ar.lock=1. The master never interprets the
// field; it only routes it onto the AR bus for the slave's exclusive monitor.
TEST_F(AxiMasterTest, LockFieldPropagatesToArLock) {
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: read
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    lock: exclusive
    dump_file: r_ar_lock.txt
)YAML"));
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_ar_lock.txt");
  master.tick();
  ASSERT_EQ(mock.captured_ar.size(), 1u);
  EXPECT_EQ(mock.captured_ar[0].lock, 1u);
}

TEST_F(AxiMasterTest, LockDefaultsToZero_OnNormalTxn) {
  auto wpath = write_hex_tmp_data(
      "w_lock_norm",
      "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F "
      "10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F");
  auto yaml = write_tmp(std::string(R"YAML(
transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + "\n");
  ni::cmodel::axi::testing::MockSlave mock;
  axi::AxiMasterT<ni::cmodel::axi::testing::MockSlave> master(
      yaml, mock, std::string(::testing::TempDir()) + "/r_lock_norm.txt");
  master.tick();
  ASSERT_EQ(mock.captured_aw.size(), 1u);
  EXPECT_EQ(mock.captured_aw[0].lock, 0u);
}

// Phase A (Task 1): two same-id writes must be admitted into the AxiMaster
// pipeline CONCURRENTLY (per AXI4 IHI 0022 §A5.3 — same-id multiple outstanding
// is legal).
//
// Pre-fix AxiMaster gated same-id admission with an early `break` on the
// admission loop, so only one same-id write was in flight at a time and the
// second was held in the scenario queue until the first's B drained. Both
// writes still eventually completed, so a simple done()-within-budget check
// would NOT detect the regression. This test directly observes the in-flight
// counter mid-run to gate the regression.
TEST_F(AxiMasterTest, SameIdConcurrentAdmissionVisibleInPipeline) {
  auto wpath = write_hex_tmp_data(
      "same_id_concurrent_obs",
      "AB CD EF 12 34 56 78 9A BC DE F0 11 22 33 44 55 "
      "66 77 88 99 AA BB CC DD EE FF 00 11 22 33 44 55");
  auto yaml = write_tmp(std::string(R"YAML(
config:
  memory_base: 0x0
  memory_size: 0x20000
  max_outstanding_write: 2
transactions:
  - op: write
    addr: 0x100
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
  - op: write
    addr: 0x10100
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML" + wpath + "\n");

  axi::Memory   memory(0, 0x20000, 0, 0);
  axi::AxiSlave slave(memory);
  slave.set_memory_bounds(0, 0x20000);
  axi::AxiMasterT<axi::AxiSlave> master(
      yaml, slave, std::string(::testing::TempDir()) + "/r_same_id_concurrent_obs.txt",
      /*max_out_w=*/2, /*max_out_r=*/2);

  // One tick is enough for the admission loop to pull BOTH scenario_txns into
  // active_write_ops_ (admission has no per-cycle throttling beyond
  // max_outstanding). Pre-fix code would `break` on same-id after admitting
  // op #1, leaving op #2 stuck in the scenario queue → count == 1.
  master.tick();
  ASSERT_EQ(master.active_write_count(), 2u)
      << "both same-id writes must be admitted concurrently after tick #1; "
         "pre-fix code would serialize them (count == 1)";

  // Drive to completion to confirm the rest of the pipeline still works
  // end-to-end with the new admission policy.
  constexpr int kMaxCycles = 500;
  int cycles = 0;
  for (; cycles < kMaxCycles && !master.done(); ++cycles) {
    master.tick();
    slave.tick();
    memory.tick();
  }
  EXPECT_TRUE(master.done()) << "master did not complete after " << cycles
                              << " cycles";
}

// Scenario smoke coverage: same-id multi-write scenario completes end-to-end
// against Memory + AxiSlave (B responses route to the per-id deque front in
// submission order). Companion to the concurrency-observing test above.
TEST_F(AxiMasterTest, SameIdMultiWriteScenarioCompletes) {
  auto wpath = write_hex_tmp_data(
      "same_id_concurrent",
      "AB CD EF 12 34 56 78 9A BC DE F0 11 22 33 44 55 "
      "66 77 88 99 AA BB CC DD EE FF 00 11 22 33 44 55");
  auto yaml = write_tmp(std::string(R"YAML(
config:
  memory_base: 0x0
  memory_size: 0x20000
  max_outstanding_write: 2
transactions:
  - op: write
    addr: 0x100
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML") + wpath + R"YAML(
  - op: write
    addr: 0x10100
    id: 0x5
    len: 0
    size: 5
    burst: INCR
    data_file: )YAML" + wpath + "\n");

  axi::Memory   memory(0, 0x20000, 0, 0);
  axi::AxiSlave slave(memory);
  slave.set_memory_bounds(0, 0x20000);
  axi::AxiMasterT<axi::AxiSlave> master(
      yaml, slave, std::string(::testing::TempDir()) + "/r_same_id_concurrent.txt",
      /*max_out_w=*/2, /*max_out_r=*/2);

  constexpr int kMaxCycles = 500;
  int cycles = 0;
  for (; cycles < kMaxCycles && !master.done(); ++cycles) {
    master.tick();
    slave.tick();
    memory.tick();
  }
  EXPECT_TRUE(master.done()) << "master did not complete after " << cycles
                              << " cycles";
}
