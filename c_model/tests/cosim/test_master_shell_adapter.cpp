// Unit tests for MasterShellAdapter — Stage 5b T8.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Two cases cover the key behavioral invariants:
//   1. Adapter drives awvalid within 50 cycles for a single-write scenario.
//   2. awvalid holds stable (does not drop) until awready is asserted — per
//      AXI4 IHI 0022 §A3.2.1 (valid must remain asserted until handshake).
//
// Fixture helper: write_temp_yaml/write_temp_data create test scenario files in
// the platform temp directory so the scenario_parser can open them normally.
#include "common/scenario.hpp"
#include "cosim/master_shell_adapter.hpp"
#include "cosim/master_shell_io.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using ni::cmodel::cosim::MasterInputs;
using ni::cmodel::cosim::MasterOutputs;
using ni::cmodel::cosim::MasterShellAdapter;

namespace {

// Return the OS temp directory as a string.
std::string tmp_dir() {
    const char* t = std::getenv("TEMP");
    if (!t) t = std::getenv("TMP");
    if (!t) t = "/tmp";
    return std::string(t);
}

// Write content to a temp file; return its path.
std::string write_temp_file(const std::string& name, const std::string& content) {
    auto path = tmp_dir() + "/" + name;
    std::ofstream f(path);
    f << content;
    return path;
}

// One 32-byte write beat: 32 space-separated hex bytes on a single line.
// size=5 → 1<<5 = 32 bytes per beat; len=0 → 1 beat total.
const std::string kSingleBeatData =
    "AA BB CC DD EE FF 00 11 22 33 44 55 66 77 88 99 "
    "AA BB CC DD EE FF 00 11 22 33 44 55 66 77 88 99\n";

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: adapter drives awvalid within 50 cycles for a write scenario.
// awready is not asserted → master queues the AW and re-presents it each tick.
// ---------------------------------------------------------------------------
TEST(MasterShellAdapter, drives_aw_for_single_write_scenario) {
    SCENARIO("MasterShellAdapter drives awvalid within 50 cycles for a single-write scenario");

    auto data_file = write_temp_file("t8_write_data.txt", kSingleBeatData);
    auto dump_file = write_temp_file("t8_read_dump.txt", "");
    auto yaml_path = write_temp_file("t8_scenario.yaml",
        "transactions:\n"
        "  - op: write\n"
        "    addr: 0x100\n"
        "    id: 0\n"
        "    len: 0\n"
        "    size: 5\n"
        "    burst: INCR\n"
        "    data_file: " + data_file + "\n");

    MasterShellAdapter adapter;
    adapter.init(yaml_path, dump_file);

    MasterInputs  in{};   // awready/wready/arready all false → master stalls
    MasterOutputs out{};
    bool saw_awvalid = false;

    for (int cycle = 0; cycle < 50 && !saw_awvalid; ++cycle) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.awvalid) saw_awvalid = true;
    }

    EXPECT_TRUE(saw_awvalid) << "awvalid never asserted in 50 cycles";
    if (saw_awvalid) {
        EXPECT_EQ(out.awid,   0u);
        EXPECT_EQ(out.awaddr, 0x100u);
        EXPECT_EQ(out.awlen,  0u);
        EXPECT_EQ(out.awsize, 5u);
    }
}

// ---------------------------------------------------------------------------
// Test 2: awvalid holds until awready — AXI4 §A3.2.1.
// Once the master asserts awvalid it must not deassert it until the handshake.
// With awready always false the output must stay valid every cycle once asserted.
// ---------------------------------------------------------------------------
TEST(MasterShellAdapter, awvalid_holds_until_awready) {
    SCENARIO("AXI4 §A3.2.1: awvalid must remain asserted once raised, until awready");

    auto data_file = write_temp_file("t8_hold_data.txt", kSingleBeatData);
    auto dump_file = write_temp_file("t8_hold_dump.txt", "");
    auto yaml_path = write_temp_file("t8_hold_scenario.yaml",
        "transactions:\n"
        "  - op: write\n"
        "    addr: 0x200\n"
        "    id: 0\n"
        "    len: 0\n"
        "    size: 5\n"
        "    burst: INCR\n"
        "    data_file: " + data_file + "\n");

    MasterShellAdapter adapter;
    adapter.init(yaml_path, dump_file);

    // awready stays false for the entire run — master must hold awvalid.
    MasterInputs  in{};
    MasterOutputs out{};
    bool saw_awvalid       = false;
    int  valid_high_cycles = 0;

    for (int cycle = 0; cycle < 30; ++cycle) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);

        if (out.awvalid) {
            saw_awvalid = true;
            ++valid_high_cycles;
        } else if (saw_awvalid) {
            // awvalid dropped before handshake — AXI4 protocol violation.
            FAIL() << "awvalid deasserted before awready at cycle " << cycle;
        }
    }

    EXPECT_TRUE(saw_awvalid)           << "awvalid never asserted in 30 cycles";
    EXPECT_GE(valid_high_cycles, 2)    << "awvalid should remain high across multiple cycles";
}
