// S2 T2d config-space narrow smoke: end-to-end through the real NMU/NSU
// packetize/depacketize/Rob pipeline, oracled by the Scoreboard, using
// sim/topologies/mesh_2x2_config_narrow_vc1.yaml's real SAM (loaded exactly
// as co-sim would via nmu::addr_trans::load_sam_table). Node (0,0) carries
// both a memory tile and a config tile, so one NSU serves both classes.
//
// Three write+read pairs, same run (S2 design doc §7 Q2: narrow lands WITH
// the flip, never after):
//   1. Narrow class WRAP burst, nonzero starting lane (addr & 63 == 8) --
//      exercises all 4 lane re-anchor sites (S2 design doc §2) across a
//      64 B word, including the wrap-around beats landing back at lane 0/4.
//   2. Narrow class INCR at a third, distinct lane (addr & 63 == 32) --
//      size-aligned (AxiMaster's own aligned-down + first-beat-mask path,
//      the data-class UnalignedCases in test_axi_master.cpp, is a separate
//      concern from this test's target: the lane re-anchor sites).
//   3. Data class (memory aperture) single beat -- dual-class in one run.
//
// Any wrong word, lane slip, or strb truncation at any of the 4 lane
// re-anchor sites shows up as a Scoreboard mismatch.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scoreboard.hpp"
#include "common/channel_model.hpp"
#include "common/channel_model_params.hpp"
#include "common/scenario.hpp"
#include "common/tmp_path.hpp"
#include "nmu/nmu.hpp"
#include "nmu/port_params.hpp"
#include "nmu/sam_yaml.hpp"
#include "nsu/nsu.hpp"
#include "nsu/port_params.hpp"
#include <deque>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace axi = ni::cmodel::axi;
namespace nmu = ni::cmodel::nmu;
namespace nsu = ni::cmodel::nsu;
namespace test = ni::cmodel::testing;

namespace {

constexpr std::size_t kMaxCycles = 20'000;
constexpr uint8_t kNmuSrcId = 0x01;
constexpr uint8_t kNsuSrcId = 0x02;

// Config tile base: mesh_2x2_config_narrow_vc1.yaml packs 4 memory tiles
// (0x100000 each) before the one config tile, so the config aperture starts
// at 4 * 0x100000 = 0x400000, size 0x1000.
constexpr uint64_t kConfigBase = 0x400000;
constexpr uint64_t kMemoryAddr = 0x2000;  // inside node (0,0)'s memory tile

std::string hex_bytes(unsigned first, unsigned count) {
    std::string out;
    for (unsigned i = 0; i < count; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", (first + i) & 0xFFu);
        if (i) out += ' ';
        out += buf;
    }
    return out;
}

// "0x"-prefixed hex, since scenario_parser's addr field parses plain digit
// strings as decimal (std::hex alone, without std::showbase, does not emit
// the prefix -- easy to drop by accident when streaming into a raw string).
std::string hex0x(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string write_narrow_smoke_scenario() {
    const std::string dir = test::unique_temp_dir("narrow_smoke");
    std::ofstream(dir + "/wrap_data.txt") << hex_bytes(0x00, 16) << '\n';   // 4 beats * 4B
    std::ofstream(dir + "/lane32_data.txt") << hex_bytes(0x40, 4) << '\n';  // 1 beat * 4B
    std::ofstream(dir + "/data_class.txt") << hex_bytes(0x80, 32) << '\n';  // 1 beat * 32B
    std::ofstream(dir + "/scenario.yaml")
        << R"YAML(
schema_version: 1
metadata:
  name: AX4-BAS-004_narrow_config_smoke
  category: basic
config:
  memory_base: 0x0
  memory_size: 0x500000
  write_latency: 0
  read_latency: 0
transactions:
  # 1. Narrow class WRAP, starting lane 8 (config base is 64 B aligned).
  - { op: write, addr: )YAML"
        << hex0x(kConfigBase + 0x8)
        << R"YAML(, id: 0x1, len: 3, size: 2, burst: WRAP, data_file: wrap_data.txt }
  - { op: read,  addr: )YAML"
        << hex0x(kConfigBase + 0x8)
        << R"YAML(, id: 0x1, len: 3, size: 2, burst: WRAP, dump_file: wrap_dump.txt }
  # 2. Narrow class INCR at lane 32 (distinct from the WRAP case's lanes).
  - { op: write, addr: )YAML"
        << hex0x(kConfigBase + 0x120)
        << R"YAML(, id: 0x2, len: 0, size: 2, burst: INCR, data_file: lane32_data.txt }
  - { op: read,  addr: )YAML"
        << hex0x(kConfigBase + 0x120)
        << R"YAML(, id: 0x2, len: 0, size: 2, burst: INCR, dump_file: lane32_dump.txt }
  # 3. Data class (memory aperture) -- dual-class in one run.
  - { op: write, addr: )YAML"
        << hex0x(kMemoryAddr)
        << R"YAML(, id: 0x3, len: 0, size: 5, burst: INCR, data_file: data_class.txt }
  - { op: read,  addr: )YAML"
        << hex0x(kMemoryAddr)
        << R"YAML(, id: 0x3, len: 0, size: 5, burst: INCR, dump_file: data_class_dump.txt }
)YAML";
    return dir + "/scenario.yaml";
}

}  // namespace

TEST(NarrowClassSmoke, ConfigSpaceEndToEndZeroMismatch) {
    SCENARIO(
        "S2 T2d narrow class smoke: WRAP + INCR narrow writes at 3 distinct byte lanes, one "
        "data-class write, all read back bit-perfect through the real NMU/NSU pipeline");

    const std::string yaml_path = write_narrow_smoke_scenario();
    auto sc = axi::load_scenario(yaml_path);

    axi::Memory mem(sc.config.memory_base, sc.config.memory_size, sc.config.write_latency,
                    sc.config.read_latency);
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(sc.config.memory_base, sc.config.memory_size);

    // Real SAM, loaded exactly as co-sim's NmuWrap::init(config_path) would.
    auto sam = nmu::addr_trans::load_sam_table("topologies/mesh_2x2_config_narrow_vc1.yaml");

    nmu::NmuConfig nmu_cfg{};
    nmu_cfg.src_id = kNmuSrcId;
    nmu_cfg.sam = sam;
    nmu_cfg.read_rob_mode = nmu::RobMode::Enabled;  // exercises the RoB read-entry lane path too
    test::ChannelModelParams cm_params{};
    test::ChannelModel channel(cm_params.req_depth, cm_params.rsp_depth);
    nmu::Nmu nmu_inst(nmu_cfg, channel.nmu_req_out(), channel.nmu_rsp_in());

    nsu::NsuConfig nsu_cfg{};
    nsu_cfg.src_id = kNsuSrcId;
    nsu::Nsu nsu_inst(nsu_cfg, channel.req_in(), channel.rsp_out());

    axi::AxiMasterT<nmu::AxiSlavePort> master(
        yaml_path, nmu_inst.axi_slave_port(),
        std::string(::testing::TempDir()) + "/narrow_smoke_read.txt",
        sc.config.max_outstanding_write, sc.config.max_outstanding_read);

    axi::Scoreboard sb;
    master.on_write_completed([&](const axi::WriteResult& wr) {
        sb.handle_write_completed(wr, wr.data, wr.strb_per_beat);
    });
    master.on_read_observed([&](const axi::ReadResult& rr) { sb.handle_read_observed(rr); });

    std::deque<axi::BBeat> b_holdover;
    std::deque<axi::RBeat> r_holdover;

    std::size_t cycle = 0;
    while (!master.done() && cycle <= kMaxCycles) {
        master.tick();
        nmu_inst.tick();
        nsu_inst.tick();

        auto& port = nsu_inst.axi_master_port();
        while (auto aw = port.pop_aw()) ASSERT_TRUE(slave.push_aw(*aw));
        while (auto w = port.pop_w()) ASSERT_TRUE(slave.push_w(*w));
        while (auto ar = port.pop_ar()) ASSERT_TRUE(slave.push_ar(*ar));

        slave.tick();
        mem.tick();

        while (!b_holdover.empty()) {
            if (!port.push_b(b_holdover.front())) break;
            b_holdover.pop_front();
        }
        while (!r_holdover.empty()) {
            if (!port.push_r(r_holdover.front())) break;
            r_holdover.pop_front();
        }
        while (auto b = slave.pop_b()) {
            if (!b_holdover.empty() || !port.push_b(*b)) b_holdover.push_back(*b);
        }
        while (auto r = slave.pop_r()) {
            if (!r_holdover.empty() || !port.push_r(*r)) r_holdover.push_back(*r);
        }

        channel.tick();
        ++cycle;
    }

    EXPECT_LE(cycle, kMaxCycles) << "watchdog tripped -- narrow class smoke did not complete";
    EXPECT_EQ(sb.mismatch_count(), 0u)
        << "narrow/data class round-trip mismatch (lane re-anchor bug)";
}
