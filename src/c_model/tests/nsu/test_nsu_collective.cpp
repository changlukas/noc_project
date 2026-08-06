// S4-T5: NSU collective echo -- the AW's collective identity rides the
// MetaBuffer to its B (design §3.1).
//
// Why exact matters: the RSP router's CollectB join is stateless. It recomputes
// the B's expected-input set from the B's own dst_id + collective_mask
// (route_mask_join), so a B carrying the wrong mask arrives at a router that is
// not expecting it and trips the join's fatal "arrived on a port outside its
// own expected-input set". These tests keep that abort off the legal path.
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::nsu::Depacketize;
using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::Packetize;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::RspCapture;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kNsuSrcId = 0x02;   // this NSU's node
constexpr uint8_t kCollector = 0x12;  // the requesting NMU, = the B's dst_id

ni::cmodel::Flit make_aw_flit(uint8_t awid, uint8_t axi_ch, uint8_t collective_op,
                              uint8_t collective_mask, uint8_t ordering_tag = 0) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("src_id", kCollector);
    f.set_header_field("dst_id", kNsuSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);  // a collective always takes the idle-ID bypass
    f.set_header_field("ordering_tag", ordering_tag);
    f.set_header_field("collective_op", collective_op);
    f.set_header_field("collective_mask", collective_mask);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", 0);
    f.set_payload_field("AW", "awsize", 5);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

axi::BBeat make_b(uint8_t id, axi::Resp resp = axi::Resp::OKAY) {
    axi::BBeat b{};
    b.id = id;
    b.resp = resp;
    return b;
}

// AW flit in on REQ -> MetaBuffer -> B beat in from the slave -> B flit out on RSP.
struct EchoTestbench {
    ChannelModel noc{16, 16};
    RspCapture b_cap, r_cap;
    MetaBuffer mb{4};
    Depacketize depkt{noc.req_in(), mb, /*max_unique_ids=*/256};
    Packetize pkt{b_cap, r_cap, r_cap, mb, kNsuSrcId};

    void accept_aw(const ni::cmodel::Flit& f) {
        ASSERT_TRUE(noc.req_out().push_flit(f));
        depkt.tick();
        ASSERT_TRUE(depkt.pop_aw().has_value());
    }
    void respond(uint8_t id) {
        ASSERT_TRUE(pkt.push_b(make_b(id)));
        pkt.tick();
    }
};
}  // namespace

TEST(NsuCollective, DataBEchoesTheAwCollectiveIdentity) {
    SCENARIO(
        "NSU echo: a collective DataAw's op/mask/ordering_tag come back on its DataB unmodified. "
        "dst_id is the requesting node (the collector) and src_id this responder -- the pair the "
        "RSP join turns back into an expected-input set");
    EchoTestbench t;
    t.accept_aw(make_aw_flit(0x05, ni::AXI_CH_DataAw, axi::COLLECTIVE_OP_MULTICAST, 0x03,
                             /*ordering_tag=*/7));
    t.respond(0x05);

    auto f = t.b_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_DataB));
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0x03u);
    EXPECT_EQ(f->get_header_field("ordering_tag"), 7u);
    EXPECT_EQ(f->get_header_field("dst_id"), kCollector);
    EXPECT_EQ(f->get_header_field("src_id"), kNsuSrcId);
    EXPECT_EQ(f->get_payload_field("B", "bid"), 0x05u);
}

TEST(NsuCollective, NarrowBEchoesTheAwCollectiveIdentity) {
    SCENARIO(
        "NSU echo, narrow mirror (Q4 revision 2 -- both classes multicast): the echo is one "
        "mechanism in the shared MetaEntry, so a NarrowAw collective returns a NarrowB carrying "
        "the same op/mask. Class only picks the axi_ch");
    EchoTestbench t;
    t.accept_aw(make_aw_flit(0x05, ni::AXI_CH_NarrowAw, axi::COLLECTIVE_OP_MULTICAST, 0x11));
    t.respond(0x05);

    auto f = t.b_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_NarrowB));
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0x11u);
}

TEST(NsuCollective, UnicastAwLeavesTheBFieldsClear) {
    SCENARIO(
        "NSU echo: a unicast AW must not stamp anything. op=UNICAST with a zero mask is what the "
        "RSP router's is_collect_b() reads as 'not a CollectB' -- an accidental stamp would send a "
        "plain B into the join and hang the write behind members that never arrive");
    EchoTestbench t;
    t.accept_aw(make_aw_flit(0x05, ni::AXI_CH_DataAw, axi::COLLECTIVE_OP_UNICAST, 0));
    t.respond(0x05);

    auto f = t.b_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_UNICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0u);
}

TEST(NsuCollective, ConcurrentCollectivesEchoTheirOwnMasks) {
    SCENARIO(
        "NSU echo: two collectives outstanding at once (different AXI ids, different destination "
        "sets). The identity is per-MetaEntry, not per-NSU state, so neither B picks up the "
        "other's mask -- cross-contamination would misroute one join and abort the other");
    EchoTestbench t;
    t.accept_aw(make_aw_flit(0x05, ni::AXI_CH_DataAw, axi::COLLECTIVE_OP_MULTICAST, 0x03));
    t.accept_aw(make_aw_flit(0x06, ni::AXI_CH_DataAw, axi::COLLECTIVE_OP_MULTICAST, 0x30));

    t.respond(0x06);  // out of allocation order: different ids are independent
    auto second = t.b_cap.pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->get_payload_field("B", "bid"), 0x06u);
    EXPECT_EQ(second->get_header_field("collective_mask"), 0x30u);

    t.respond(0x05);
    auto first = t.b_cap.pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->get_payload_field("B", "bid"), 0x05u);
    EXPECT_EQ(first->get_header_field("collective_mask"), 0x03u);
}
