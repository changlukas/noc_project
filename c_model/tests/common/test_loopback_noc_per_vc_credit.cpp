#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_flit_on_vc(uint8_t vc_id, uint8_t dst_id, uint8_t axi_ch) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id",  vc_id);
    f.set_header_field("last",   1);
    return f;
}

}  // namespace

TEST(LoopbackNocPerVcCredit, ConfiguredDepth16ExhaustsAfter16Pushes) {
    SCENARIO("LoopbackNoc: set_per_vc_depth(16) -> 16 successive pushes on VC=0 "
             "exhaust credit; 17th push_flit and credit_avail both return false");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(16);
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(noc.req_out().credit_avail(0));
        ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    }
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    EXPECT_FALSE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
}

TEST(LoopbackNocPerVcCredit, PerVcDepthEnforcedIndependently) {
    SCENARIO("LoopbackNoc: per_vc_depth=2 -> 2 pushes on VC=0 exhaust credit; "
             "VC=1 still has full credit (per-VC counters independent)");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    EXPECT_TRUE(noc.req_out().credit_avail(1));
}

TEST(LoopbackNocPerVcCredit, PopReleasesCredit) {
    SCENARIO("LoopbackNoc: pop_flit on NSU side decrements NMU per-VC counter, "
             "restoring credit_avail for the popped flit's VC");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    EXPECT_FALSE(noc.req_out().credit_avail(0));
    auto f = noc.req_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.req_out().credit_avail(0));
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);
}

TEST(LoopbackNocPerVcCredit, RspSidePerVcCreditMirrorsReq) {
    SCENARIO("LoopbackNoc: NSU rsp side has symmetric per-VC credit; "
             "credit_avail on NSU rsp_out queries response-direction counter");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    noc.set_per_vc_depth(2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    EXPECT_FALSE(noc.rsp_out().credit_avail(0));
    auto f = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(noc.rsp_out().credit_avail(0));
}

TEST(LoopbackNocPerVcCredit, CreditAvailMatchesPushFlitForPerNsuFull) {
    SCENARIO("LoopbackNoc: credit_avail must return false when per-NSU queue is "
             "full even if per-VC counter still has room (contract: credit_avail=true "
             "must imply push_flit will succeed; otherwise VcArbiter tick aborts on "
             "the 'lying downstream' guard)");
    // 1-NSU, req-queue depth 2, default per_vc_depth (unlimited)
    LoopbackNoc noc(/*req*/2, /*rsp*/32);
    // Fill the per-NSU queue to capacity on VC=0.
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_AW)));
    EXPECT_FALSE(noc.req_out().credit_avail(0))
        << "Per-NSU queue full; credit_avail must mirror push_flit";
    EXPECT_FALSE(noc.req_out().credit_avail(1))
        << "Per-NSU queue full means NO vc can push (conservative contract)";
}

TEST(LoopbackNocPerVcCredit, RspSideCreditAvailMatchesPushFlitForRspQueueFull) {
    SCENARIO("LoopbackNoc rsp side: credit_avail returns false when rsp_q is full "
             "even if per-VC counter has room (same contract as req side)");
    LoopbackNoc noc(/*req*/32, /*rsp*/2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_flit_on_vc(0, 0, ni::AXI_CH_B)));
    EXPECT_FALSE(noc.rsp_out().credit_avail(0));
    EXPECT_FALSE(noc.rsp_out().credit_avail(1));
}
