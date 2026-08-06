// S4-T2: NMU collective admission -- AWUSER address-mask translate (design
// §2.2), the reject set (§2.3), and the R2 per-id interlock (§2.3a).
//
// Every reject row gets a death test: the standing fault-injection rule says a
// new checker is unproven until it has been made to fire.
#include "nmu/rob.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/nmu.hpp"
#include "nmu/packetize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>

using ni::cmodel::nmu::Depacketize;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::nmu::Rob;
using ni::cmodel::nmu::RobMode;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;
namespace addr_trans = ni::cmodel::nmu::addr_trans;

namespace {
constexpr uint8_t kSrcId = 0x00;

// 4x4 mesh, one 4 KB tile per node, packed row-major. dst_id = (y << 4) | x and
// tile index = y*4 + x, so AWADDR bit 12 selects x[0], 13 x[1], 14 y[0], 15
// y[1] -- the equal-aperture map on which the enumerate-and-check translate
// degenerates to upstream's bit-select.
addr_trans::SamTable mesh_sam() {
    return addr_trans::SamTable::uniform(4, 4, 0x1000);
}

constexpr uint64_t kTile = 0x1000;
uint64_t tile_addr(unsigned x, unsigned y) {
    return (y * 4 + x) * kTile;
}

// AWUSER: [7:0] user, [9:8] collective_op, [57:10] collective_mask (address mask).
uint64_t awuser(uint8_t op, uint64_t addr_mask, uint8_t user = 0) {
    return (addr_mask << 10) | (uint64_t{op} << 8) | user;
}

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint64_t user = 0, uint8_t len = 0) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = len;
    b.size = 5;  // 32 B/beat
    b.burst = axi::Burst::INCR;
    b.user = user;
    return b;
}

axi::WBeat make_w(bool last) {
    axi::WBeat b{};
    b.last = last;
    return b;
}

// Rob over Packetize + Depacketize, with the AW/W faces captured separately so
// a multicast worm's header fields can be read back beat by beat.
struct CollectiveTestbench {
    explicit CollectiveTestbench(addr_trans::SamTable sam = mesh_sam())
        : rob(pkt, depkt, RobMode::Disabled, std::move(sam)) {}
    ChannelModel noc{16, 16};
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt{aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, {}};
    Depacketize depkt{noc.rsp_in(), 16, 16};
    Rob rob;
};

// Deliver a bypassed B into the response ingress. The collective header bits
// default to a plain unicast B; the merged B of a collective carries the
// echoed pair the NSU stamped (design §3.1).
void feed_b(CollectiveTestbench& t, uint8_t id, uint8_t collective_op = axi::COLLECTIVE_OP_UNICAST,
            uint8_t collective_mask = 0, axi::Resp resp = axi::Resp::OKAY) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("collective_op", collective_op);
    f.set_header_field("collective_mask", collective_mask);
    f.set_payload_field("B", "bid", id);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(resp));
    ASSERT_TRUE(t.noc.rsp_out().push_flit(f));
    t.depkt.tick();
}

// Feed a bypassed B back so the transaction retires and releases the id.
void retire_b(CollectiveTestbench& t, uint8_t id) {
    feed_b(t, id);
    ASSERT_TRUE(t.rob.pop_b().has_value());
}

struct MaskCase {
    const char* name;
    unsigned anchor_x, anchor_y;
    uint64_t addr_mask;
    uint8_t expect_node_mask;
};
}  // namespace

// === Translate (design §2.2, legal rows) ===

class NmuCollectiveMaskP : public ::testing::TestWithParam<MaskCase> {};

TEST_P(NmuCollectiveMaskP, AddressMaskTranslatesToNodeMask) {
    SCENARIO(
        "NMU collective: the 48 b AWUSER address mask translates to the 8 b flit collective_mask "
        "by enumerating the 2^n named addresses over the SAM; dst_id stays the anchor's");
    const MaskCase& c = GetParam();
    CollectiveTestbench t;
    const uint64_t anchor = tile_addr(c.anchor_x, c.anchor_y);
    ASSERT_TRUE(
        t.rob.push_aw(make_aw(0x05, anchor, awuser(axi::COLLECTIVE_OP_MULTICAST, c.addr_mask))));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), c.expect_node_mask);
    EXPECT_EQ(f->get_header_field("dst_id"), (c.anchor_y << 4) | c.anchor_x);
    EXPECT_EQ(f->get_payload_field("AW", "awaddr"), 0u);  // node-local offset, shared by replicas
}

INSTANTIATE_TEST_SUITE_P(AlignedMasks, NmuCollectiveMaskP,
                         ::testing::Values(
                             // Two nodes on one row: AWADDR bit 12 = x[0].
                             MaskCase{"pair", 0, 0, 0x1000, 0x01},
                             // Whole row y=1: bits 12,13 = x[1:0].
                             MaskCase{"row", 0, 1, 0x3000, 0x03},
                             // Whole column x=2: bits 14,15 = y[1:0].
                             MaskCase{"column", 2, 0, 0xC000, 0x30},
                             // 2x2 aligned submesh at the origin: x[0] and y[0].
                             MaskCase{"submesh_2x2", 0, 0, 0x5000, 0x11},
                             // Every node: all four node-index bits.
                             MaskCase{"full_mesh", 0, 0, 0xF000, 0x33}),
                         [](const ::testing::TestParamInfo<MaskCase>& i) { return i.param.name; });

TEST(NmuCollective, NarrowClassCollectiveTranslates) {
    SCENARIO(
        "NMU collective Q4 revision 2: multicast is legal on BOTH classes -- config-space message "
        "replication rides Narrow. The translate was always class-agnostic, so a narrow anchor "
        "yields the same node mask; the flit lands on NarrowAw and so forks on REQ, not DAT");
    CollectiveTestbench t(addr_trans::SamTable({{0x0000, kTile, 0x00, axi::AxiClass::Narrow},
                                                {0x1000, kTile, 0x01, axi::AxiClass::Narrow}}));
    auto aw = make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    aw.size = 3;  // narrow rides the 8 B lane: AWSIZE <= 3, as for a narrow unicast
    ASSERT_TRUE(t.rob.push_aw(aw));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_NarrowAw));
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0x01u);
}

TEST(NmuCollective, UnicastAwLeavesCollectiveFieldsClear) {
    SCENARIO(
        "NMU collective: an AWUSER with only the 8 b user field set is an ordinary unicast AW -- "
        "collective_op/collective_mask stay zero and AWUSER[7:0] still reaches the payload");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(1, 1), /*user=*/0x5A)));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_UNICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0u);
    EXPECT_EQ(f->get_payload_field("AW", "awuser"), 0x5Au);
}

TEST(NmuCollective, WBeatsInheritTheAwMask) {
    SCENARIO(
        "NMU collective: W beats carry no AWUSER of their own, so they latch the mask/op from "
        "their AW's write-meta entry -- every beat of the worm forks to the same node set");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000), /*len=*/1)));
    ASSERT_TRUE(t.rob.push_w(make_w(/*last=*/false)));
    ASSERT_TRUE(t.rob.push_w(make_w(/*last=*/true)));
    for (int beat = 0; beat < 2; ++beat) {
        auto f = t.w_cap.pop();
        ASSERT_TRUE(f.has_value()) << "W beat " << beat;
        EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
        EXPECT_EQ(f->get_header_field("collective_mask"), 0x03u);
    }
}

TEST(NmuCollective, S1StageCarriesTheCollectiveFields) {
    SCENARIO(
        "NMU collective: the S1 request stage between Rob and Packetize re-builds AwHeaderMeta "
        "from its staged copy, so the translated mask has to survive the stage. Dropping it would "
        "abort on Packetize's meta-vs-AWUSER check rather than mis-stamp the flit");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, {});
    ni::cmodel::nmu::NmuReqS1Bridge bridge;
    const auto aw = make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000));
    const ni::cmodel::nmu::AwHeaderMeta meta{
        /*dst_id=*/0x10,
        /*local_addr=*/0,        0, 0, axi::AxiClass::Data, axi::COLLECTIVE_OP_MULTICAST,
        /*collective_mask=*/0x03};
    ASSERT_TRUE(bridge.push_aw_with_meta(aw, meta));
    bridge.tick(pkt);
    auto f = aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0x03u);
}

// === R2 interlock (design §2.3a) -- retryable backpressure, not an error ===

TEST(NmuCollective, CollectiveWaitsForAnIdleId) {
    SCENARIO(
        "NMU collective R2: a collective is admitted only into an empty per-id order list, so it "
        "always takes the idle-ID bypass (no RoB slot). Refused while a unicast AW of the same id "
        "is in flight, admitted once that one's B retires; a different id is unaffected");
    CollectiveTestbench t;
    const uint64_t coll_user = awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000);
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0))));
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1), coll_user)));
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x06, tile_addr(0, 1), coll_user)));  // other id, free
    retire_b(t, 0x05);
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1), coll_user)));
}

TEST(NmuCollective, NothingStreamsPastAnInFlightCollective) {
    SCENARIO(
        "NMU collective R2: the front-entry collective flag closes the same-destination bypass -- "
        "a later same-id AW to the collective's own anchor would otherwise stream past without a "
        "RoB slot. Refused until the collective's merged B retires; the shared write pool holds "
        "one entry for it meanwhile");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    EXPECT_EQ(t.rob.write_txns(), 1u);  // collective allocates the shared pool like any AW
    // Same dest as the collective's anchor: exactly the same-destination bypass case.
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1))));
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(3, 3))));
    retire_b(t, 0x05);
    EXPECT_EQ(t.rob.write_txns(), 0u);
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1))));
}

// === Merged-B ingress (design §2.3a, release side) ===

TEST(NmuCollective, MergedBWithCollectiveBitsRetiresLikeAUnicastB) {
    SCENARIO(
        "NMU merged-B ingress: the B of a collective comes back carrying the echoed "
        "collective_op/collective_mask. Nothing on the response path reads them -- decode_b takes "
        "bid/bresp/buser from the payload and the meta is ordering_tag/ordering_req/class only -- "
        "so the merged B retires through the same bypassed path as any unicast B, with zero new "
        "response-path state");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    EXPECT_EQ(t.rob.write_txns(), 1u);
    feed_b(t, 0x05, axi::COLLECTIVE_OP_MULTICAST, /*collective_mask=*/0x03, axi::Resp::SLVERR);
    auto b = t.rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05);
    EXPECT_EQ(b->resp, axi::Resp::SLVERR);  // the merge's SLVERR precedence result, unaltered here
    EXPECT_EQ(t.rob.write_txns(), 0u);      // one B releases the shared pool entry
    // R2 released: the id accepts work again, including a second collective.
    EXPECT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(3, 3), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
}

TEST(NmuCollectiveDeath, SecondBForOneCollectiveAborts) {
    SCENARIO(
        "NMU merged-B ingress: one AW gets exactly one B, merged or not. A second B for the same "
        "collective would double-release the interlock and the shared write pool. The order-list "
        "head invariant catches it first -- the write_txns_ underflow assert below is the backstop "
        "for any path that reaches retirement without it");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    feed_b(t, 0x05, axi::COLLECTIVE_OP_MULTICAST, 0x03);
    ASSERT_TRUE(t.rob.pop_b().has_value());
    feed_b(t, 0x05, axi::COLLECTIVE_OP_MULTICAST, 0x03);
    EXPECT_DEATH(t.rob.pop_b(), "does not match the head");
}

TEST(NmuCollectiveDeath, RetireWithoutAnOutstandingWriteAborts) {
    SCENARIO(
        "NMU merged-B ingress: the write_txns_ underflow assert IS the single-merged-B invariant "
        "seen from the NMU side (design §2.3a). Injected directly at the retire entry point, since "
        "the order-list head check shadows it on the pop_b path");
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    retire_b(t, 0x05);
    EXPECT_DEATH(t.rob.retire_b(/*ordering_req=*/false, /*ordering_tag=*/0, 0x05),
                 "no outstanding write transaction");
}

// === Reject set (design §2.3) -- one death test per row ===

TEST(NmuCollectiveDeath, MaskWithoutOp) {
    SCENARIO(
        "NMU collective §2.2 matrix: collective_op=UNICAST with a nonzero mask is a stimulus "
        "contradiction. Upstream cannot express it (it derives the op FROM the mask); ours is an "
        "explicit field, so the mismatch rejects instead of being normalized away");
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_UNICAST, 0x1000))),
        "UNICAST requires a zero mask");
}

TEST(NmuCollectiveDeath, OpWithoutMask) {
    SCENARIO(
        "NMU collective §2.2 matrix: collective_op=MULTICAST with a zero mask names an empty "
        "destination set. NOT the upstream downgrade-to-unicast, which only exists because "
        "upstream has no explicit op to contradict");
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0))),
        "empty destination set");
}

TEST(NmuCollectiveDeath, ReservedOp) {
    SCENARIO("NMU collective §2.2 matrix: collective_op 2-3 are reserved (spec §6 header table)");
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(2, 0x1000))),
                 "reserved AWUSER collective_op");
}

TEST(NmuCollectiveDeath, AwLockOnCollective) {
    SCENARIO(
        "NMU collective §2.3: spec §6.1 -- AxLOCK is unicast only, a collective is not an "
        "exclusive access");
    CollectiveTestbench t;
    auto aw = make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    aw.lock = 1;
    EXPECT_DEATH(t.rob.push_aw(aw), "AWLOCK set on a collective AW");
}

TEST(NmuCollectiveDeath, ReplicasStraddleTheClasses) {
    SCENARIO(
        "NMU collective §2.3: both classes multicast (Q4 revision 2), but ONE destination set "
        "cannot straddle them -- the class picks the network the worm forks on (Narrow -> REQ, "
        "Data -> DAT) and a packet rides exactly one. Separate rule from the node-local offset "
        "check that shares its loop");
    CollectiveTestbench t(addr_trans::SamTable({{0x0000, kTile, 0x00, axi::AxiClass::Narrow},
                                                {0x1000, kTile, 0x01, axi::AxiClass::Data}}));
    auto aw = make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    aw.size = 3;  // narrow anchor: AWSIZE <= 3 (8 B), orthogonal to the class rule
    EXPECT_DEATH(t.rob.push_aw(aw), "narrow and data classes");
}

TEST(NmuCollectiveDeath, MoreMaskBitsThanNodeIdBits) {
    SCENARIO(
        "NMU collective §2.2 check 1: n set mask bits name 2^n addresses, so n above the node-id "
        "width names more destinations than the mesh has nodes. Caught before enumerating");
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0),
                                       awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1FF000))),
                 "than a node id has");
}

TEST(NmuCollectiveDeath, MaskBitInsideTheTileOffset) {
    SCENARIO(
        "NMU collective §2.2 check 2 (spec :456-462): mask bits are limited to the node-index "
        "field. A bit below it wildcards an address bit inside one aperture, so the named "
        "addresses no longer share a node-local offset");
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x20))),
        "collective replicas disagree on");
}

TEST(NmuCollectiveDeath, DuplicateNodeInTheDestinationSet) {
    SCENARIO(
        "NMU collective §2.2 check 3: the 2^n named addresses must reach 2^n DISTINCT nodes. "
        "popcount(node_mask) == n alone does not imply it -- {0x00,0x01,0x02,0x02} spans exactly "
        "2 bits yet covers 3 nodes, so distinctness is checked in its own right");
    CollectiveTestbench t(
        addr_trans::SamTable::packed({{0, 0, kTile}, {1, 0, kTile}, {2, 0, kTile}, {2, 0, kTile}}));
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))),
                 "names one node twice");
}

TEST(NmuCollectiveDeath, MaskBitOutsideTheMesh) {
    SCENARIO(
        "NMU collective §2.2 check 4: a mask bit above the node-index field names addresses past "
        "the last tile. This is the NI-boundary reject the T1 route-mask carry-in requires -- a "
        "bad mask must surface here, never as a route_mask_fork out-of-mesh abort");
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(
                     make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x10000))),
                 "collective replica address maps to no");
}

TEST(NmuCollectiveDeath, NodeSetIsNotAnAlignedWildcard) {
    SCENARIO(
        "NMU collective §2.2 check 3: 2^n distinct nodes at a shared offset are still illegal if "
        "they do not form an ALIGNED wildcard over dst_id -- popcount(node_mask) must equal n. "
        "Here 4 addresses reach nodes {0x00,0x01,0x12,0x13}, spanning 3 dst_id bits, not 2");
    CollectiveTestbench t(
        addr_trans::SamTable::packed({{0, 0, kTile}, {1, 0, kTile}, {2, 1, kTile}, {3, 1, kTile}}));
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))),
                 "node set is not an aligned");
}

TEST(NmuCollectiveDeath, MetaDisagreesWithAwuser) {
    SCENARIO(
        "NMU collective §2.1: push_aw_with_meta receives already-validated meta from Rob, so it "
        "only rechecks that the meta still agrees with the AWUSER it was derived from. A "
        "mismatch is a model bug in the translating layer, not a stimulus error");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, mesh_sam());
    auto aw = make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    ni::cmodel::nmu::AwHeaderMeta meta{0x00, 0, 0, 0};  // collective fields left at UNICAST / 0
    EXPECT_DEATH(pkt.push_aw_with_meta(aw, meta), "disagrees with AWUSER");
}

TEST(NmuCollectiveDeath, MetaOpAndMaskDisagree) {
    SCENARIO(
        "NMU collective §2.1: the op/mask agreement half of the same guard. meta.collective_op "
        "can match AWUSER and still be paired with an empty node mask -- a translate that "
        "returned MULTICAST with nothing to fan out to");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, mesh_sam());
    const auto aw = make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    ni::cmodel::nmu::AwHeaderMeta meta{0x00,
                                       0,
                                       0,
                                       0,
                                       axi::AxiClass::Data,
                                       axi::COLLECTIVE_OP_MULTICAST,
                                       /*collective_mask=*/0};  // agrees with AWUSER, mask empty
    EXPECT_DEATH(pkt.push_aw_with_meta(aw, meta), "collective_op and collective_mask disagree");
}

TEST(NmuCollectiveDeath, AwuserAboveTheFieldWidth) {
    SCENARIO(
        "NMU collective: AWUSER is 58 b. The accessors mask to 2 b / 48 b, so a bit above the "
        "field would be silently dropped on the Rob path -- reject it instead. The direct path "
        "already catches it by testing AWUSER[57:8] as a whole");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, mesh_sam());
    auto aw = make_aw(0x05, tile_addr(0, 0));
    aw.user = uint64_t{1} << 60;
    EXPECT_DEATH(pkt.push_aw_with_meta(aw, ni::cmodel::nmu::AwHeaderMeta{0x00, 0, 0, 0}),
                 "above the field width");
}

TEST(NmuCollectiveDeath, ReplicaBurstOverrunsItsAperture) {
    SCENARIO(
        "NMU collective §2.2 check 5 (spec :461-462): each replica's aperture must cover the full "
        "burst footprint. The anchor's 6 KB burst fits its 8 KB tile but overruns the 4 KB "
        "aperture of the replica it names");
    CollectiveTestbench t(addr_trans::SamTable(
        {{0x0000, 2 * kTile, 0x00}, {0x2000, kTile, 0x01}, {0x3000, kTile, 0x02}}));
    // 192 beats x 32 B = 6 KB, anchored at 0; the replica at 0x2000 has 4 KB.
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x2000),
                                       /*len=*/191)),
                 "collective replica burst footprint");
}
