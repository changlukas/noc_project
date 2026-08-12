// DatMergeWrap unit tests (S3a T5, controller ruling): the NI-level merge
// point for DAT's shared LOCAL port. Covers the properties the design
// depends on: egress worm-atomicity survives the merge (AW+W from NMU stay
// contiguous even with NSU contending), independent per-producer credit
// return (no cross-attribution), and ingress demux by axi_ch.
#include "wrap/dat_merge_wrap.hpp"
#include "wrap/flit_byte_conv.hpp"
#include "wrap/nmu_wrap.hpp"
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <vector>

using namespace ni::cmodel::wrap;
using ni::cmodel::Flit;

namespace {

Flit make_data_aw(uint8_t awid, uint8_t vc) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataAw);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", 0);
    f.set_payload_field("AW", "awid", awid);
    return f;
}

Flit make_data_w(uint8_t vc) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataW);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("DATA_W", "wlast", 1);
    return f;
}

Flit make_data_r(uint8_t rid, uint8_t vc) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("DATA_R", "rid", rid);
    return f;
}

}  // namespace

// Egress worm-atomicity: NMU pushes a DataAw+DataW pair (its own
// dat_wormhole_arbiter_ already made them contiguous); NSU contends with a
// DataR in the SAME cycle window. The merge's wormhole arbiter must not
// interleave NSU's R between NMU's AW and W.
TEST(DatMergeWrap, EgressPreservesNmuAwWAtomicityUnderNsuContention) {
    DatMergeWrap m;
    m.init(/*dat_num_vc=*/1);

    // push_flit() lands directly in the arbiter's pending queue (no staged
    // register like SimpleRouter/Router), so tick() can grant+drain a
    // just-pushed flit in the SAME call -- observe get_outputs() every tick,
    // including these two setup ticks.
    DatMergeOutputs out{};
    std::vector<uint64_t> drained_ch;
    auto observe = [&] {
        m.get_outputs(out);
        if (out.tx_dat_valid) {
            drained_ch.push_back(flit_from_bytes(out.tx_dat_flit).get_header_field("axi_ch"));
        }
    };

    DatMergeInputs in{};
    in.nmu_tx_dat_valid = true;
    in.nmu_tx_dat_flit = flit_to_bytes(make_data_aw(0x05, /*vc=*/0));
    in.nsu_tx_dat_valid = true;
    in.nsu_tx_dat_flit = flit_to_bytes(make_data_r(0x01, /*vc=*/0));
    m.set_inputs(in);
    m.tick();  // both pushed; the arbiter may already grant one this tick
    observe();

    in = DatMergeInputs{};
    in.nmu_tx_dat_valid = true;
    in.nmu_tx_dat_flit = flit_to_bytes(make_data_w(/*vc=*/0));
    m.set_inputs(in);
    m.tick();
    observe();

    in = DatMergeInputs{};
    for (int t = 0; t < 8 && drained_ch.size() < 3; ++t) {
        m.set_inputs(in);
        m.tick();
        observe();
    }
    ASSERT_EQ(drained_ch.size(), 3u) << "expected AW, W, R (in some AW-W-contiguous order)";
    // Find where DataAw landed; DataW must be the very next drain.
    auto aw_it =
        std::find(drained_ch.begin(), drained_ch.end(), static_cast<uint64_t>(ni::AXI_CH_DataAw));
    ASSERT_NE(aw_it, drained_ch.end());
    ASSERT_LT(std::distance(drained_ch.begin(), aw_it) + 1, static_cast<long>(drained_ch.size()))
        << "DataAw drained but no flit followed it";
    EXPECT_EQ(*(aw_it + 1), static_cast<uint64_t>(ni::AXI_CH_DataW))
        << "NSU's DataR interleaved between NMU's DataAw and DataW -- worm atomicity broken";
}

// Independent credit-return: NMU's push must generate a credit-return pulse
// tagged for NMU, never leak onto NSU's credit-return output.
TEST(DatMergeWrap, CreditReturnDoesNotCrossProducers) {
    DatMergeWrap m;
    m.init(/*dat_num_vc=*/1);

    DatMergeInputs in{};
    in.nmu_tx_dat_valid = true;
    in.nmu_tx_dat_flit = flit_to_bytes(make_data_r(0x01, /*vc=*/0));  // single-flit worm (tail=1)
    m.set_inputs(in);
    DatMergeOutputs out{};
    m.tick();  // push_flit() + tick()'s own grant can land in the same call
    m.get_outputs(out);
    bool nmu_pulse = out.nmu_tx_dat_crdvalid[0];
    EXPECT_FALSE(out.nsu_tx_dat_crdvalid[0]) << "NMU's send must not credit-return to NSU";

    in = DatMergeInputs{};
    m.set_inputs(in);
    for (int t = 0; t < 8 && !nmu_pulse; ++t) {
        m.tick();
        m.get_outputs(out);
        if (out.nmu_tx_dat_crdvalid[0]) nmu_pulse = true;
        EXPECT_FALSE(out.nsu_tx_dat_crdvalid[0]) << "NMU's send must not credit-return to NSU";
    }
    EXPECT_TRUE(nmu_pulse) << "NMU never got its credit-return pulse";
}

// Ingress demux: DataR from the router goes to NMU; DataAw/DataW go to NSU.
TEST(DatMergeWrap, IngressDemuxesByAxiCh) {
    DatMergeWrap m;
    m.init(/*dat_num_vc=*/1);

    DatMergeInputs in{};
    in.rx_dat_valid = true;
    in.rx_dat_flit = flit_to_bytes(make_data_r(0x03, /*vc=*/0));
    m.set_inputs(in);
    m.tick();
    DatMergeOutputs out{};
    m.get_outputs(out);
    EXPECT_TRUE(out.nmu_rx_dat_valid) << "DataR must demux to NMU";
    EXPECT_FALSE(out.nsu_rx_dat_valid) << "DataR must not also reach NSU";
    EXPECT_EQ(flit_from_bytes(out.nmu_rx_dat_flit).get_payload_field("DATA_R", "rid"), 0x03u);
    EXPECT_TRUE(out.rx_dat_crdvalid[0]) << "ingress accept must credit-return to the router";

    in.rx_dat_flit = flit_to_bytes(make_data_aw(0x07, /*vc=*/0));
    m.set_inputs(in);
    m.tick();
    m.get_outputs(out);
    EXPECT_TRUE(out.nsu_rx_dat_valid) << "DataAw must demux to NSU";
    EXPECT_FALSE(out.nmu_rx_dat_valid) << "DataAw must not also reach NMU";
}

// S3a T6: the two tests above prove the merge's OWN wormhole lock is
// contention-safe using hand-crafted mock flits pushed directly into the
// arbiter -- they say nothing about whether Packetize's real steering
// decision (S3a T6) actually delivers a Data-class AW+W pair to that lock as
// one contiguous worm. Drive a real NmuWrap through a real AXI write instead:
// if steering ever regressed to splitting AW/W across networks (the S3a
// stage design §1 hazard this task fixes), this test would see NSU's
// contending DataR land between them.
TEST(DatMergeWrap, RealNmuSteeringKeepsAwWContiguousUnderNsuContention) {
    NmuWrap nmu;
    nmu.init(TOPOLOGY_DIR "/mesh_2x2_vc1.yaml", /*src_id=*/0x01, /*dat_num_vc=*/1);
    DatMergeWrap merge;
    merge.init(/*dat_num_vc=*/1);

    std::array<uint8_t, 64> wdata{};
    for (int b = 0; b < 64; ++b) wdata[b] = static_cast<uint8_t>(0xA0 + b);

    NmuInputs nmu_in{};
    NmuOutputs nmu_out{};
    DatMergeInputs merge_in{};
    DatMergeOutputs merge_out{};

    // aw_phase / w_phase mirror NmuWrap's own registered handshake
    // (nmu_wrap.hpp): a channel's push registers on the tick AFTER its ready
    // first pulses ("in_.awvalid && prev_awready_" / "in_.wvalid &&
    // prev_wready_"), so each VALID must stay held one extra tick past the
    // ready pulse -- dropping it the same tick it's observed (as a
    // same-cycle valid-implies-done check would) drops the transfer.
    int aw_phase = 0;  // 0=offering, 1=holding one extra tick, 2=done
    int w_phase = -1;  // -1=not yet offering (waits for AW to open the W window)
    std::vector<uint64_t> drained_ch;

    for (int t = 0; t < 40; ++t) {
        nmu_in.awvalid = (aw_phase < 2);
        nmu_in.awid = 0x05;
        nmu_in.awaddr = 0x100000ull;  // mesh_2x2_vc1 memory tile 1: data class
        nmu_in.awlen = 0;
        nmu_in.awsize = 5;
        nmu_in.awburst = 1;
        nmu_in.wvalid = (w_phase >= 0 && w_phase < 2);
        nmu_in.wdata = wdata;
        nmu_in.wstrb = ~0ull;
        nmu_in.wlast = true;
        nmu_in.bready = true;

        // NSU-side contention: offer one DataR right as NMU's write starts (a
        // real NSU's own sender credit would gate repeat offers -- this mock
        // has none, so a single push is the honest amount of contention to
        // model). Racing at t=0 is the worst case for the arbiter's lock: NSU's
        // R and NMU's AW both arrive before either is granted.
        merge_in.nsu_tx_dat_valid = (t == 0);
        if (t == 0) merge_in.nsu_tx_dat_flit = flit_to_bytes(make_data_r(0x01, /*vc=*/0));

        nmu.set_inputs(nmu_in);
        nmu.tick();
        nmu.get_outputs(nmu_out);
        if (aw_phase == 0 && nmu_out.awready) {
            aw_phase = 1;
        } else if (aw_phase == 1) {
            aw_phase = 2;
        }
        if (w_phase == -1 && nmu_out.wready) {
            w_phase = 0;
        } else if (w_phase == 0) {
            w_phase = 1;
        } else if (w_phase == 1) {
            w_phase = 2;
        }

        merge_in.nmu_tx_dat_valid = nmu_out.tx_dat_valid;
        merge_in.nmu_tx_dat_flit = nmu_out.tx_dat_flit;
        merge.set_inputs(merge_in);
        merge.tick();
        merge.get_outputs(merge_out);
        if (merge_out.tx_dat_valid) {
            drained_ch.push_back(flit_from_bytes(merge_out.tx_dat_flit).get_header_field("axi_ch"));
        }
    }

    ASSERT_EQ(aw_phase, 2) << "NmuWrap never accepted the AW beat";
    ASSERT_EQ(w_phase, 2) << "NmuWrap never accepted the W beat";
    auto aw_it =
        std::find(drained_ch.begin(), drained_ch.end(), static_cast<uint64_t>(ni::AXI_CH_DataAw));
    ASSERT_NE(aw_it, drained_ch.end()) << "DataAw never reached the merge's DAT egress";
    ASSERT_LT(std::distance(drained_ch.begin(), aw_it) + 1, static_cast<long>(drained_ch.size()))
        << "DataAw drained but no flit followed it";
    EXPECT_EQ(*(aw_it + 1), static_cast<uint64_t>(ni::AXI_CH_DataW))
        << "real Packetize-steered DataAw+DataW worm was split by NSU contention at the merge";
}
