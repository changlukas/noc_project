// Unit tests for NmuWrap, covering the
// wait_valid / context-gated ready policy.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Three cases cover the key behavioral invariants:
//   1. Idle adapter keeps awready/wready/arready LOW (wait_valid: capacity
//      alone never asserts ready).
//   2. Single AW + W: two-phase AW handshake (ready pulses the tick after
//      valid appears), then the W window opens and wready pre-asserts.
//   3. AWLEN=7 (8-beat W burst): wready holds for the whole burst (one bubble
//      at burst start, then full rate). A second AW presented mid-burst still
//      gets its ready pulse — multi-outstanding AW (post addresses ahead of
//      data) is legitimate AXI4 and load-bearing for the RoB/multi-ID paths.
#include "common/mesh_config.hpp"
#include "ni_flit_constants.h"
#include "wrap/flit_byte_conv.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nmu_wrap_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::wrap::flit_from_bytes;
using ni::cmodel::wrap::NmuInputs;
using ni::cmodel::wrap::NmuOutputs;
using ni::cmodel::wrap::NmuWrap;

// NmuWrap::init takes a config file, no default map. The handshake tests
// below care only that the stimulus address resolves, so the smallest shipped
// mesh serves; the SAM-specific tests write their own YAML.
constexpr const char* kTopologyYaml = CONFIG_DIR "/mesh_2x2.yml";

TEST(NmuWrap, idle_adapter_keeps_readys_low) {
    NmuWrap adapter;
    adapter.init(kTopologyYaml);

    NmuInputs in{};  // all valid signals false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    NmuOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.awready) << "wait_valid: no AWVALID -> awready must stay low";
    EXPECT_FALSE(out.wready) << "no open W burst window -> wready must stay low";
    EXPECT_FALSE(out.arready) << "wait_valid: no ARVALID -> arready must stay low";
    EXPECT_FALSE(out.bvalid) << "no B response without a prior AW+W";
    EXPECT_FALSE(out.rvalid) << "no R response without a prior AR";
    EXPECT_FALSE(out.tx_req_valid) << "no REQ flit without any AXI request";
}

TEST(NmuWrap, single_aw_w_two_phase_handshake) {
    NmuWrap adapter;
    adapter.init(kTopologyYaml);

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: AWVALID first seen — awready pulses; W window still closed.
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x200;
    in.awlen = 0;    // 1 beat
    in.awsize = 5;   // 32 bytes/beat -- a legal half-width beat on the 64 B bus,
                     // not full-bus (that's size=6); this test checks handshake
                     // timing, not bus width, so any legal size works.
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: AWVALID observed + capacity -> awready pulses";
    EXPECT_FALSE(out.wready) << "cycle 1: AW not yet handshaken -> W window closed";

    // Cycle 2: valid held && prev ready -> AW handshake tick; window opens.
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awready) << "cycle 2: handshake done -> awready back low";
    EXPECT_TRUE(out.wready) << "cycle 2: W window open -> wready pre-asserts without wvalid";

    // Cycle 3: drive the single W beat (prev wready=1 -> consumed; WLAST
    // closes the window).
    in = NmuInputs{};
    in.wvalid = true;
    in.wdata[0] = 0x55;
    in.wstrb = 0xFFFF'FFFFu;
    in.wlast = true;
    in.bready = true;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.wready) << "cycle 3: WLAST consumed -> W window closed";
}

TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available) {
    NmuWrap adapter;
    adapter.init(kTopologyYaml);

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: AW (len=7 -> 8 beats) first seen.
    in.awvalid = true;
    in.awid = 0x00;
    in.awaddr = 0x100;
    in.awlen = 7;    // 8 beats
    in.awsize = 5;   // 32 bytes/beat
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: awready pulses for the observed AW";

    // Cycle 2: AW handshake tick — window opens (w_expected=8).
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awready) << "cycle 2: AW consumed";
    EXPECT_TRUE(out.wready) << "cycle 2: burst window open, wready holds";

    // Cycles 3-10: drive 8 W beats one per cycle. A beat is accepted when the
    // PREVIOUS tick's wready (the wire value this cycle) was high. Present a
    // second AW alongside the first W beat: it must be gated (no awready)
    // while the burst is open.
    int beats_accepted = 0;
    bool prev_wready = out.wready;  // wire value seen by the first W beat
    for (int beat = 0; beat < 8; ++beat) {
        in = NmuInputs{};
        in.bready = true;
        in.wvalid = true;
        in.wdata.fill(0);
        in.wdata[0] = static_cast<uint8_t>(0x10 + beat);
        in.wstrb = 0xFFFF'FFFFu;
        in.wlast = (beat == 7);
        if (beat == 0) {
            in.awvalid = true;  // second AW presented mid-burst
            in.awid = 0x02;
            in.awaddr = 0x300;
            in.awlen = 0;
            in.awsize = 5;
            in.awburst = 1;
        }
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (prev_wready) ++beats_accepted;
        prev_wready = out.wready;
        if (beat == 0) {
            EXPECT_TRUE(out.awready) << "second AW presented mid-burst must still get its ready "
                                        "pulse (multi-outstanding AW preserved)";
        }
        if (beat < 7) {
            EXPECT_TRUE(out.wready)
                << "beat " << beat << ": burst window still open -> wready holds";
        }
    }

    EXPECT_EQ(beats_accepted, 8)
        << "all 8 W beats must transfer at full rate after the one-bubble start";
    EXPECT_FALSE(out.wready) << "after WLAST the window closes -> wready low";
}

// The legacy default (4 GB tiles) resolves the same address to a different dst_id/local_addr, so
// observing the config-mapped values proves init(config_path) actually loaded the config file's
// address ranges.
TEST(NmuWrap, init_with_config_path_loads_sam_from_yaml) {
    const std::string path = ni::cmodel::testing::write_config(
        "nmu_wrap_sam.yml",
        ni::cmodel::testing::mesh_config_yaml(
            2, 2, "      - { base: 0x0, size: 0x1000, stride: 0x1000, space: memory }\n"));

    NmuWrap adapter;
    adapter.init(path.c_str());

    NmuInputs in{};
    NmuOutputs out{};

    // 0x1040 -> under the 4 KB/tile 2x2 SAM this is tile (x=1,y=0),
    // dst_id = (y<<X_WIDTH)|x = 1, and the address rides through untouched.
    // The tile pick comes from this YAML's sizes, so observing it pins the load.
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x1040;
    in.awlen = 0;
    in.awsize = 2;   // 4 B/beat: keeps the burst inside the 4 KB tile
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    ASSERT_TRUE(out.awready) << "cycle 1: AWVALID observed -> awready pulses";

    adapter.set_inputs(in);  // valid held; prev ready -> AW handshake tick
    adapter.tick();
    adapter.get_outputs(out);

    // Drain the DAT egress face for the AW flit (S3a T6: Data-class AW/W
    // steer to DAT, spec :348 -- the tile entry above has no "space"
    // annotation, so it is Data class). DAT is credit-flow (init() pre-seeds
    // NMU_ARBITER_FIFO_DEPTH toward the DatMergeWrap stage), so no external
    // credit pulse is needed for one flit; tx_req_ready is irrelevant here.
    bool saw_aw_flit = false;
    in = NmuInputs{};
    for (int i = 0; i < 32 && !saw_aw_flit; ++i) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.tx_dat_valid) {
            auto flit = flit_from_bytes(out.tx_dat_flit);
            if (flit.get_header_field("axi_ch") == ni::AXI_CH_DataAw) {
                EXPECT_EQ(flit.get_header_field("dst_id"), 0x01u);
                EXPECT_EQ(flit.get_payload_field("AW", "awaddr"), 0x1040ull);
                saw_aw_flit = true;
            }
        }
    }
    ASSERT_TRUE(saw_aw_flit) << "NmuWrap never produced an AW flit from the config-path SAM";
}

// AWUSER plumb (S4 T6): NmuInputs.awuser reaches axi::AwBeat::user whole, so
// a collective AWUSER driven through the wrap face is translated and stamped
// into the AW flit header. This is the wrap-level weld the DPI awuser
// argument lands on; the translate itself is T2-tested at the Rob level.
TEST(NmuWrap, awuser_collective_reaches_flit_header) {
    const std::string path = ni::cmodel::testing::write_config(
        "nmu_wrap_awuser_sam.yml",
        ni::cmodel::testing::mesh_config_yaml(
            2, 2, "      - { base: 0x0, size: 0x1000, stride: 0x1000, space: memory }\n"));

    NmuWrap adapter;
    adapter.init(path.c_str());

    NmuInputs in{};
    NmuOutputs out{};

    // The address names (0,0) local 0x40; address-mask bit 12 wildcards the x tile bit
    // under the 4 KB packing, naming {(0,0), (1,0)} -> node mask 0x01.
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x40;
    in.awlen = 0;
    in.awsize = 2;
    in.awburst = 1;  // INCR
    in.awuser = (uint64_t{0x1000} << 10) | (uint64_t{1} << 8);
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    ASSERT_TRUE(out.awready) << "cycle 1: AWVALID observed -> awready pulses";

    adapter.set_inputs(in);  // valid held; prev ready -> AW handshake tick
    adapter.tick();
    adapter.get_outputs(out);

    bool saw_aw_flit = false;
    in = NmuInputs{};
    for (int i = 0; i < 32 && !saw_aw_flit; ++i) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.tx_dat_valid) {
            auto flit = flit_from_bytes(out.tx_dat_flit);
            if (flit.get_header_field("axi_ch") == ni::AXI_CH_DataAw) {
                EXPECT_EQ(flit.get_header_field("collective_op"), ni::COLLECTIVE_OP_MULTICAST);
                EXPECT_EQ(flit.get_header_field("collective_mask"), 0x01u);
                EXPECT_EQ(flit.get_header_field("dst_id"), 0x00u) << "the address names dst (0,0)";
                saw_aw_flit = true;
            }
        }
    }
    ASSERT_TRUE(saw_aw_flit) << "NmuWrap never emitted the collective DataAw flit -- the "
                                "awuser plumb (NmuInputs.awuser -> AwBeat.user) is broken";
}

// A missing topology is an error, not a fabricated mesh. init() used to fall
// back to a 16x16 / 4 GB-per-tile map when config_path was absent, so a
// testbench that forgot the YAML ran green against an address map nothing in
// the tree ships.
TEST(NmuWrap, init_without_topology_throws) {
    NmuWrap adapter;
    EXPECT_THROW(adapter.init(nullptr), std::invalid_argument);
    EXPECT_THROW(adapter.init(""), std::invalid_argument);
}

// port_id is a 2-bit header field, so 3 is encodable but names no endpoint --
// a testbench that passes it must be told, not silently given a fourth port.
TEST(NmuWrap, init_with_reserved_port_id_throws) {
    NmuWrap adapter;
    EXPECT_THROW(adapter.init(kTopologyYaml, /*src_id=*/0, /*port_id=*/3), std::invalid_argument);
}

// An ENCODABLE port_id that this topology declares no endpoint at. Nothing
// downstream catches it: route_compute resolves an ejection port from the flit
// header without asking whether an NI sits behind it, so the responses to this
// NI's requests route to an empty boundary port and the packets are lost with
// no error anywhere. The fault injections below are the three ways to get it
// wrong on a topology that DOES declare peripherals.
TEST(NmuWrap, init_with_a_port_id_no_endpoint_declares_throws) {
    constexpr const char* periph = CONFIG_DIR "/mesh_2x2_periph.yml";
    // mesh_2x2_vc1_periph declares an x-face peripheral (port 1) at (0,0) and
    // at (0,1), and no y-face peripheral anywhere.
    constexpr uint8_t kNode00 = 0;  // (x=0, y=0)
    constexpr uint8_t kNode10 = 1;  // (x=1, y=0)
    NmuWrap adapter;

    // The declared endpoints: the tile on LOCAL, and the peripheral on port 1.
    EXPECT_NO_THROW(adapter.init(periph, kNode00, /*port_id=*/0));
    EXPECT_NO_THROW(adapter.init(periph, kNode00, /*port_id=*/1));

    // Fault 1: the y face at a coordinate whose peripheral is on the x face.
    EXPECT_THROW(adapter.init(periph, kNode00, /*port_id=*/2), std::invalid_argument);
    // Fault 2: the right face, the wrong coordinate -- (1,0) declares no peripheral.
    EXPECT_THROW(adapter.init(periph, kNode10, /*port_id=*/1), std::invalid_argument);
    // Fault 3: a peripheral port on a topology with no peripherals block at all.
    EXPECT_THROW(adapter.init(kTopologyYaml, kNode00, /*port_id=*/1), std::invalid_argument);
    EXPECT_THROW(adapter.init(kTopologyYaml, kNode00, /*port_id=*/2), std::invalid_argument);

    // And a coordinate outside the mesh has no endpoint on any port.
    EXPECT_THROW(adapter.init(kTopologyYaml, /*src_id=*/0x77, /*port_id=*/0),
                 std::invalid_argument);
}

// Port 2 ACCEPTED. Every other port_id 2 in this file is an EXPECT_THROW, because
// mesh_2x2_vc1_periph declares x-face peripherals only -- so narrowing the range
// check above to `port_id > 1` would pass the whole suite: the endpoint-declaration
// check throws the same exception type and would stand in for it everywhere. The
// four-face topology is the one that puts a y-face peripheral behind port 2.
TEST(NmuWrap, init_accepts_a_y_face_peripheral_port) {
    constexpr const char* periph4 = CONFIG_DIR "/mesh_4x4_periph4.yml";
    // dst_id = (y << 4) | x. The y-face peripherals are at (1,0) south and (2,3) north.
    constexpr uint8_t kSouth = 0x01;  // (x=1, y=0)
    constexpr uint8_t kNorth = 0x32;  // (x=2, y=3)
    NmuWrap adapter;

    EXPECT_NO_THROW(adapter.init(periph4, kSouth, /*port_id=*/2));
    EXPECT_NO_THROW(adapter.init(periph4, kNorth, /*port_id=*/2));

    // The same coordinates on the OTHER face are undeclared, so this stays a
    // statement about the port and not about the coordinate.
    EXPECT_THROW(adapter.init(periph4, kSouth, /*port_id=*/1), std::invalid_argument);
    EXPECT_THROW(adapter.init(periph4, kNorth, /*port_id=*/1), std::invalid_argument);
}

// Note: the wrap-level "odd num_vc rejected" death test was removed in S3a
// T5. REQ/RSP are fixed single-VC now (S1 Q2), and the S3b VC collapse
// retired the read/write virtual-network split that owned the even-num_vc
// rule, so no oddness constraint exists at this boundary anymore: the
// allocator's candidate set is every VC in [0, dat_num_vc).
