// S2 T2d config-space narrow smoke: end-to-end through the real NMU/NSU
// packetize/depacketize/Rob pipeline, oracled by the Scoreboard, using
// sim/topologies/mesh_2x2_vc1.yaml's real SAM (loaded exactly as co-sim
// would via nmu::addr_trans::load_sam_table). Every node carries both a
// memory tile and a config tile, so one NSU serves both classes.
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
#include "common/tmp_path.hpp"
#include "nmu/nmu.hpp"
#include "nmu/port_params.hpp"
#include "nmu/sam_yaml.hpp"
#include "nsu/nsu.hpp"
#include "nsu/port_params.hpp"
#include <cassert>
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

// Config tile base: mesh_2x2_vc1.yaml packs each node's config tile inside
// that node's own 0x100000000 block, above its 0x2000000 memory tile, so
// node (0,0)'s config aperture starts at 0x2000000, size 0x1000.
constexpr uint64_t kConfigBase = 0x2000000;
constexpr std::size_t kConfigWindowSize = 0x1000;  // the SAM's config tile size
constexpr uint64_t kMemoryAddr = 0x2000;           // inside node (0,0)'s memory tile

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

// Two memory windows behind one IMemoryPort, one per AXI class -- what
// sim/tb/user_node_endpoint.sv's tile crossbar does in real hardware (one RAM
// per address space, decoded from the master's address, not one RAM spanning
// the gap between them). A single flat axi::Memory can no longer stand in
// for the destination: the data-class and config-class windows are ~32 MB
// apart (the node's memory tile size), and stretching one axi::Memory across
// that gap would mean allocating ~32 MB for a unit test.
class DualWindowMemoryPort : public axi::IMemoryPort {
  public:
    DualWindowMemoryPort(uint64_t data_base, std::size_t data_size, uint64_t config_base,
                         std::size_t config_size, std::size_t write_lat, std::size_t read_lat)
        : data_base_(data_base),
          data_size_(data_size),
          config_base_(config_base),
          config_size_(config_size),
          data_(data_base, data_size, write_lat, read_lat),
          config_(config_base, config_size, write_lat, read_lat) {}

    bool submit_write(const axi::MemWriteReq& req) override {
        return window_for_(req.addr).submit_write(req);
    }
    bool submit_read(const axi::MemReadReq& req) override {
        return window_for_(req.addr).submit_read(req);
    }
    std::optional<axi::MemWriteResp> pop_write_resp() override {
        if (auto r = data_.pop_write_resp()) return r;
        return config_.pop_write_resp();
    }
    std::optional<axi::MemReadResp> pop_read_resp() override {
        if (auto r = data_.pop_read_resp()) return r;
        return config_.pop_read_resp();
    }
    void tick() {
        data_.tick();
        config_.tick();
    }

  private:
    axi::Memory& window_for_(uint64_t addr) {
        if (addr >= data_base_ && addr < data_base_ + data_size_) return data_;
        if (addr >= config_base_ && addr < config_base_ + config_size_) return config_;
        // Neither window claims this address: a bug in the test's own
        // stimulus (or the SAM it was derived from), not a case to paper
        // over by defaulting into one of the two.
        assert(false && "address lands in neither the data nor the config window");
        std::abort();
    }

    uint64_t data_base_;
    std::size_t data_size_;
    uint64_t config_base_;
    std::size_t config_size_;
    axi::Memory data_;
    axi::Memory config_;
};

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
    const std::string yaml_path = write_narrow_smoke_scenario();
    auto sc = axi::load_scenario(yaml_path);

    DualWindowMemoryPort mem_port(sc.config.memory_base, sc.config.memory_size, kConfigBase,
                                  kConfigWindowSize, sc.config.write_latency,
                                  sc.config.read_latency);
    axi::AxiSlave slave(mem_port);

    // Real SAM, loaded exactly as co-sim's NmuWrap::init(config_path) would.
    auto sam = nmu::addr_trans::load_sam_table(TOPOLOGY_DIR "/mesh_2x2_vc1.yaml");

    // This test's subject is the narrow-class lane re-anchor path, but
    // sb.mismatch_count() below is class-agnostic -- it would pass just as
    // well if kConfigBase silently decoded as Data class. Assert the class
    // directly off the same SAM the pipeline uses, so a config base that
    // drifts back inside the memory tile fails here for the right reason
    // instead of testing the wrong datapath in green.
    EXPECT_EQ(sam.translate(kConfigBase + 0x8).cls, axi::AxiClass::Narrow);
    EXPECT_EQ(sam.translate(kConfigBase + 0x120).cls, axi::AxiClass::Narrow);
    EXPECT_EQ(sam.translate(kMemoryAddr).cls, axi::AxiClass::Data);

    nmu::NmuConfig nmu_cfg{};
    nmu_cfg.src_id = kNmuSrcId;
    nmu_cfg.sam = sam;
    nmu_cfg.read_rob_mode = nmu::RobMode::Enabled;  // exercises the RoB read-entry lane path too
    test::ChannelModelParams cm_params{};
    test::ChannelModel channel(cm_params.req_depth, cm_params.rsp_depth);
    // DAT face (S3a T6 steering): the Data-class (memory aperture) write/read
    // in this test's item 3 now rides DAT for real. ChannelModel is
    // network-agnostic (keys off the flit's own header fields), so the same
    // REQ/RSP adapter objects serve as the DAT sink/source -- see
    // test_request_response_loopback.cpp's identical fix for rationale.
    nmu::Nmu nmu_inst(nmu_cfg, channel.nmu_req_out(), channel.nmu_rsp_in(), channel.nmu_req_out(),
                      channel.nmu_rsp_in());

    nsu::NsuConfig nsu_cfg{};
    nsu_cfg.src_id = kNsuSrcId;
    nsu::Nsu nsu_inst(nsu_cfg, channel.req_in(), channel.rsp_out(), channel.req_in(),
                      channel.rsp_out());

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
        mem_port.tick();

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
