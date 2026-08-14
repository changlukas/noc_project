#include "common/channel_model.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::testing::ChannelModel;

namespace {

Flit make_flit_on_vc(uint8_t vc_id, uint8_t dst_id, uint8_t axi_ch) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", vc_id);
    f.set_header_field("flit_tail", 1);
    return f;
}

}  // namespace

TEST(ChannelModelPerVcCredit, ConfiguredDepth16ExhaustsAfter16Pushes) {
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    noc.set_per_vc_depth(16);
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(noc.req_out().credit_avail(0));
        ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    }
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    EXPECT_FALSE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
}

TEST(ChannelModelPerVcCredit, PerVcDepthEnforcedIndependently) {
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    EXPECT_TRUE(noc.req_out().credit_avail(1));
}

TEST(ChannelModelPerVcCredit, PopReleasesCredit) {
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    auto f = noc.req_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.req_out().credit_avail(0));
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);
}

TEST(ChannelModelPerVcCredit, RspSidePerVcCreditMirrorsReq) {
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowB)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowB)));
    EXPECT_FALSE(noc.rsp_out().credit_avail(0));
    auto f = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.rsp_out().credit_avail(0));
}

TEST(ChannelModelPerVcCredit, CreditAvailMatchesPushFlitForPerNsuFull) {
    // 1-NSU, req-queue depth 2, default per_vc_depth (unlimited)
    ChannelModel noc(/*req*/ 2, /*rsp*/ 32);
    // Fill the per-NSU queue to capacity on VC=0.
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowAw)));
    EXPECT_FALSE(noc.req_out().credit_avail(0))
        << "Per-NSU queue full; credit_avail must mirror push_flit";
    EXPECT_FALSE(noc.req_out().credit_avail(1))
        << "Per-NSU queue full means NO vc can push (conservative contract)";
}

TEST(ChannelModelPerVcCredit, RspSideCreditAvailMatchesPushFlitForRspQueueFull) {
    ChannelModel noc(/*req*/ 32, /*rsp*/ 2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowB)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_NarrowB)));
    EXPECT_FALSE(noc.rsp_out().credit_avail(0));
    EXPECT_FALSE(noc.rsp_out().credit_avail(1));
}
