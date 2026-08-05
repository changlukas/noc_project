// DatMergeWrap unit tests (S3a T5, controller ruling): the NI-level merge
// point for DAT's shared LOCAL port. Covers the properties the design
// depends on: egress worm-atomicity survives the merge (AW+W from NMU stay
// contiguous even with NSU contending), independent per-producer credit
// return (no cross-attribution), and ingress demux by axi_ch.
#include "wrap/dat_merge_wrap.hpp"
#include "wrap/flit_byte_conv.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <algorithm>
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
    in.nsu_tx_dat_flit = flit_to_bytes(make_data_r(0x09, /*vc=*/0));
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
