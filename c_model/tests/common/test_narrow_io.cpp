#include "common/loopback_request_io.hpp"
#include "common/loopback_response_io.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

TEST(NarrowInterface, RequestPacketizerAcceptsRequestBeats) {
    ni::cmodel::testing::RequestChannelSet req;
    ni::cmodel::testing::LoopbackRequestPacketizer p(req);
    axi::AwBeat beat{};
    EXPECT_TRUE(p.push_aw(beat));
    EXPECT_EQ(req.aw.size(), 1u);
}

TEST(NarrowInterface, ResponseDepacketizerDefaultMetaForwards) {
    ni::cmodel::testing::ResponseChannelSet rsp;
    rsp.b_capacity = 4;
    ni::cmodel::testing::LoopbackResponseDepacketizer d(rsp);
    axi::BBeat beat{};
    beat.id = 7;
    rsp.b.push_back(beat);
    auto out = d.pop_b_with_meta();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->first.id, 7);
    EXPECT_EQ(out->second.rob_idx, 0);
    EXPECT_EQ(out->second.rob_req, 0);
}

TEST(NarrowInterface, RequestDepacketizerPopsRequestBeats) {
    ni::cmodel::testing::RequestChannelSet req;
    axi::AwBeat aw{};
    aw.id = 42;
    req.aw.push_back(aw);
    ni::cmodel::testing::LoopbackRequestDepacketizer d(req);
    auto out = d.pop_aw();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, 42);
    EXPECT_FALSE(d.pop_aw().has_value());  // queue empty
}

TEST(NarrowInterface, ResponsePacketizerPushesResponseBeats) {
    ni::cmodel::testing::ResponseChannelSet rsp;
    ni::cmodel::testing::LoopbackResponsePacketizer p(rsp);
    axi::RBeat rb{};
    EXPECT_TRUE(p.push_r(rb));
    EXPECT_EQ(rsp.r.size(), 1u);
}

TEST(NarrowInterface, ResponseDepacketizerPopRWithMetaForwards) {
    ni::cmodel::testing::ResponseChannelSet rsp;
    ni::cmodel::testing::LoopbackResponseDepacketizer d(rsp);
    axi::RBeat rb{};
    rb.id = 9;
    rsp.r.push_back(rb);
    auto out = d.pop_r_with_meta();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->first.id, 9);
    EXPECT_EQ(out->second.rob_idx, 0);
    EXPECT_EQ(out->second.rob_req, 0);
}
