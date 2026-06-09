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
