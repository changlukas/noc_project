#include "common/flit_link_probe.hpp"
#include "flit.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include <deque>
#include <gtest/gtest.h>
#include <optional>

using ni::cmodel::Flit;
using ni::cmodel::testing::FlitLog;
using ni::cmodel::testing::ReqOutProbe;
using ni::cmodel::testing::RspInProbe;

namespace {

// Fake request sink: accepts a flit unless `block_` is set, recording the
// raw flit so the test can confirm byte-for-byte forwarding.
struct FakeReqOut : ni::cmodel::noc::NocReqOut {
    bool block_ = false;
    std::deque<Flit> got_;
    bool push_flit(const Flit& f) override {
        if (block_) return false;
        got_.push_back(f);
        return true;
    }
};

struct FakeRspIn : ni::cmodel::noc::NocRspIn {
    std::deque<Flit> q_;
    std::optional<Flit> pop_flit() override {
        if (q_.empty()) return std::nullopt;
        Flit f = q_.front();
        q_.pop_front();
        return f;
    }
};

Flit make_flit(uint8_t axi_ch, uint8_t vc, uint8_t src, uint8_t dst) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("vc_id", vc);
    f.set_header_field("src_id", src);
    f.set_header_field("dst_id", dst);
    return f;
}

}  // namespace

TEST(FlitLinkProbe, PushProbeForwardsAndRecordsCrossing) {
    uint64_t now = 0;
    FakeReqOut sink;
    FlitLog log("NMU0.req_out");
    ReqOutProbe probe(sink, log, now);

    now = 7;
    Flit f = make_flit(/*axi_ch=*/0, /*vc=*/1, /*src=*/3, /*dst=*/5);
    EXPECT_TRUE(probe.push_flit(f));
    ASSERT_EQ(sink.got_.size(), 1u);  // forwarded
    ASSERT_EQ(log.crossings().size(), 1u);
    EXPECT_EQ(log.crossings()[0].cycle, 7u);
    EXPECT_EQ(log.crossings()[0].axi_ch, 0u);
    EXPECT_EQ(log.crossings()[0].vc_id, 1u);
    EXPECT_EQ(log.crossings()[0].src_id, 3u);
    EXPECT_EQ(log.crossings()[0].dst_id, 5u);
    EXPECT_EQ(log.boundary_id(), "NMU0.req_out");
}

TEST(FlitLinkProbe, BackpressuredPushRecordsNothing) {
    uint64_t now = 3;
    FakeReqOut sink;
    sink.block_ = true;
    FlitLog log("NMU0.req_out");
    ReqOutProbe probe(sink, log, now);
    EXPECT_FALSE(probe.push_flit(make_flit(0, 0, 0, 0)));
    EXPECT_TRUE(log.crossings().empty());
}

TEST(FlitLinkProbe, PopProbeRecordsOnlyWhenFlitReturned) {
    uint64_t now = 0;
    FakeRspIn src;
    FlitLog log("NMU0.rsp_in");
    RspInProbe probe(src, log, now);

    now = 2;
    EXPECT_FALSE(probe.pop_flit().has_value());  // empty -> no record
    EXPECT_TRUE(log.crossings().empty());

    src.q_.push_back(make_flit(/*axi_ch=*/4, /*vc=*/0, /*src=*/9, /*dst=*/1));
    now = 8;
    auto got = probe.pop_flit();
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(log.crossings().size(), 1u);
    EXPECT_EQ(log.crossings()[0].cycle, 8u);
    EXPECT_EQ(log.crossings()[0].axi_ch, 4u);
    EXPECT_EQ(log.crossings()[0].src_id, 9u);
}
