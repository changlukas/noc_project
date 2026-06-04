#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::Depacketize;
using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::testing::LoopbackNoc;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_aw_flit(uint8_t awid, uint64_t addr, uint8_t src_id = 0x10,
                              uint8_t rob_req = 0, uint8_t rob_idx = 0) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AW);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", addr);
    f.set_payload_field("AW", "awsize", 5);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}
ni::cmodel::Flit make_w_flit(uint32_t strb, bool last) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_W);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("last", 1);
    f.set_payload_field("W", "wlast", last ? 1u : 0u);
    f.set_payload_field("W", "wstrb", strb);
    return f;
}
ni::cmodel::Flit make_ar_flit(uint8_t arid, uint64_t addr, uint8_t src_id = 0x10) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AR);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("last", 1);
    f.set_payload_field("AR", "arid", arid);
    f.set_payload_field("AR", "araddr", addr);
    f.set_payload_field("AR", "arsize", 5);
    f.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}
}  // namespace

TEST(NsuDepacketize, AwFlitSnapshotsMetadataAndPopsBeat) {
    SCENARIO(
        "NSU Depacketize: AW flit snapshots src_id/rob_req/rob_idx into MetaBuffer + emits AW "
        "beat");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*aw*/ 16, /*w*/ 16, /*ar*/ 16);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000,
                                                     /*src*/ 0x12, /*rob_req*/ 1, /*rob_idx*/ 3)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->id, 0x05);
    EXPECT_EQ(aw->addr, 0x1000u);
    // MetaBuffer snapshot
    auto m = mb.peek_write(0x05);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->src_id, 0x12);
    EXPECT_EQ(m->rob_req, 1);
    EXPECT_EQ(m->rob_idx, 3);
}

TEST(NsuDepacketize, ArFlitSnapshotsReadMeta) {
    SCENARIO("NSU Depacketize: AR flit snapshots read-side meta into MetaBuffer + emits AR beat");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x07, 0x2000, 0x12)));
    depkt.tick();
    EXPECT_TRUE(depkt.pop_ar().has_value());
    EXPECT_TRUE(mb.peek_read(0x07).has_value());
    EXPECT_EQ(mb.peek_read(0x07)->src_id, 0x12);
}

TEST(NsuDepacketize, WFlitNoMetaSideEffect) {
    SCENARIO(
        "NSU Depacketize: W flit emits W beat but does NOT touch MetaBuffer (write-meta belongs to "
        "AW)");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFFFF, true)));
    depkt.tick();
    EXPECT_TRUE(depkt.pop_w().has_value());
    // MetaBuffer untouched
    EXPECT_FALSE(mb.peek_write(0).has_value());
}

TEST(NsuDepacketize, DemuxMixedAwWAr) {
    SCENARIO(
        "NSU Depacketize: interleaved AW/W/AR flits demux to per-channel queues by axi_ch header");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x01, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFF, true)));
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x02, 0x1000)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 0x01);
    EXPECT_EQ(depkt.pop_w()->strb, 0xFFu);
    EXPECT_EQ(depkt.pop_ar()->id, 0x02);
}

TEST(NsuDepacketize, PendingHolBlockingWFullBlocksAwBehind) {
    SCENARIO(
        "NSU Depacketize: HoL W queue full holds pending W; AW behind blocked until W drained");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*aw*/ 16, /*w cap*/ 1, /*ar*/ 16);
    // Order: W, W, AW
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAA, true)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xBB, true)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x07, 0x0)));
    depkt.tick();
    EXPECT_TRUE(depkt.pop_w().has_value());    // first W (0xAA) demuxed
    EXPECT_FALSE(depkt.pop_aw().has_value());  // AW blocked behind pending W
    depkt.tick();
    EXPECT_TRUE(depkt.pop_w().has_value());  // pending W (0xBB)
    depkt.tick();
    EXPECT_TRUE(depkt.pop_aw().has_value());  // AW now demuxed
}

TEST(NsuDepacketize, PopBAssertFalse) {
    SCENARIO("NSU Depacketize: pop_b asserts false (B is response-direction only, not request)");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
    EXPECT_DEATH(depkt.pop_b(), ".*");
}

TEST(NsuDepacketize, FifoOrderPreservedAcrossChannels) {
    SCENARIO("NSU Depacketize: AW queue preserves NoC arrival order across 3 sequential AW flits");
    LoopbackNoc noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(1, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(2, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(3, 0x0)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 1);
    EXPECT_EQ(depkt.pop_aw()->id, 2);
    EXPECT_EQ(depkt.pop_aw()->id, 3);
}
