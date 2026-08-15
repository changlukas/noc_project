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

constexpr uint64_t kTile = 0x1000;

// SamTable does not infer mesh dimensions, so a hand-built table states its own
// coordinate ranges the way sam_yaml's loader does for a shipped topology.
// Returns false when the entries contradict the declaration, which leaves the
// space a unicast-only target (spec §5.1).
// No peripheral in any fixture here, so the tile region is the full span --
// x_first/y_first = 0, x_last/y_last = count - 1.
bool declare(addr_trans::SamTable& t, axi::AxiClass cls, unsigned offset, unsigned x_count,
             unsigned y_count) {
    const unsigned x_bits = addr_trans::clog2(x_count);
    return t.declare_space_coords(cls, {{offset, x_bits},
                                        {offset + x_bits, addr_trans::clog2(y_count)},
                                        x_count,
                                        y_count,
                                        0,
                                        x_count - 1,
                                        0,
                                        y_count - 1});
}

// 4x4 mesh, one 4 KB tile per node, packed row-major. dst_id = (y << 4) | x and
// tile index = y*4 + x, so AWADDR bit 12 selects x[0], 13 x[1], 14 y[0], 15
// y[1] -- the equal-region map the collective bit-select reads.
addr_trans::SamTable mesh_sam() {
    auto t = addr_trans::SamTable::uniform(4, 4, kTile);
    declare(t, axi::AxiClass::Data, 12, 4, 4);
    return t;
}

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
    unsigned addr_x, addr_y;
    uint64_t addr_mask;
    uint8_t expect_node_mask;
};
}  // namespace

// === Translate (design §2.2, legal rows) ===

class NmuCollectiveMaskP : public ::testing::TestWithParam<MaskCase> {};

TEST_P(NmuCollectiveMaskP, AddressMaskTranslatesToNodeMask) {
    const MaskCase& c = GetParam();
    CollectiveTestbench t;
    const uint64_t addr = tile_addr(c.addr_x, c.addr_y);
    ASSERT_TRUE(
        t.rob.push_aw(make_aw(0x05, addr, awuser(axi::COLLECTIVE_OP_MULTICAST, c.addr_mask))));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_MULTICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), c.expect_node_mask);
    EXPECT_EQ(f->get_header_field("dst_id"), (c.addr_y << 4) | c.addr_x);
    // The REQUEST's address rides the flit, unrewritten, to every replica. What
    // the replicas share is the offset inside their own node's slot, which each
    // endpoint recovers by masking the node index off (space_windows'
    // node_addr_w) -- the same shape upstream uses.
    EXPECT_EQ(f->get_payload_field("AW", "awaddr"), addr);
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

// spec §1.3: memory and config base addresses stay apart because the map places them apart;
// translate() forwards addresses untouched.
TEST(NmuCollective, TwoSpaceTableKeepsTheSpacesApart) {
    // 2x2 mesh, 4 KB memory tiles then 4 KB config tiles, packed in list order.
    addr_trans::SamTable sam({{0x0000, kTile, 0x00, axi::AxiClass::Data},
                              {0x1000, kTile, 0x01, axi::AxiClass::Data},
                              {0x2000, kTile, 0x10, axi::AxiClass::Data},
                              {0x3000, kTile, 0x11, axi::AxiClass::Data},
                              {0x4000, kTile, 0x00, axi::AxiClass::Narrow},
                              {0x5000, kTile, 0x01, axi::AxiClass::Narrow},
                              {0x6000, kTile, 0x10, axi::AxiClass::Narrow},
                              {0x7000, kTile, 0x11, axi::AxiClass::Narrow}});
    ASSERT_TRUE(declare(sam, axi::AxiClass::Data, 12, 2, 2));
    ASSERT_TRUE(declare(sam, axi::AxiClass::Narrow, 12, 2, 2));
    CollectiveTestbench t(std::move(sam));

    // Memory-class pair (nodes 0x00, 0x01), tile offset 0x40.
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, 0x0040, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000))));
    auto data_flit = t.aw_cap.pop();
    ASSERT_TRUE(data_flit.has_value());
    EXPECT_EQ(data_flit->get_header_field("collective_mask"), 0x01u);
    // The request address, unchanged.
    EXPECT_EQ(data_flit->get_payload_field("AW", "awaddr"), 0x0040u);

    // Same offset in the config space of the same two nodes: same node mask,
    // different address. That separation is what the tile decoder reads.
    auto config_aw = make_aw(0x06, 0x4040, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    config_aw.size = 3;  // narrow rides the 8 B lane
    ASSERT_TRUE(t.rob.push_aw(config_aw));
    auto config_flit = t.aw_cap.pop();
    ASSERT_TRUE(config_flit.has_value());
    EXPECT_EQ(config_flit->get_header_field("collective_mask"), 0x01u);
    EXPECT_EQ(config_flit->get_payload_field("AW", "awaddr"), 0x4040u);
}

TEST(NmuCollective, NarrowClassCollectiveTranslates) {
    addr_trans::SamTable sam({{0x0000, kTile, 0x00, axi::AxiClass::Narrow},
                              {0x1000, kTile, 0x01, axi::AxiClass::Narrow}});
    ASSERT_TRUE(declare(sam, axi::AxiClass::Narrow, 12, 2, 1));
    CollectiveTestbench t(std::move(sam));
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
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(1, 1), /*user=*/0x5A)));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_op"), axi::COLLECTIVE_OP_UNICAST);
    EXPECT_EQ(f->get_header_field("collective_mask"), 0u);
    EXPECT_EQ(f->get_payload_field("AW", "awuser"), 0x5Au);
}

TEST(NmuCollective, WBeatsInheritTheAwMask) {
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
    CollectiveTestbench t;
    const uint64_t coll_user = awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000);
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0))));
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1), coll_user)));
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x06, tile_addr(0, 1), coll_user)));  // other id, free
    retire_b(t, 0x05);
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1), coll_user)));
}

TEST(NmuCollective, NothingStreamsPastAnInFlightCollective) {
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    EXPECT_EQ(t.rob.write_txns(), 1u);  // collective allocates the shared pool like any AW
    // Same dest as the collective's dst_id: exactly the same-destination bypass case.
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1))));
    EXPECT_FALSE(t.rob.push_aw(make_aw(0x05, tile_addr(3, 3))));
    retire_b(t, 0x05);
    EXPECT_EQ(t.rob.write_txns(), 0u);
    EXPECT_TRUE(t.rob.push_aw(make_aw(0x05, tile_addr(0, 1))));
}

// === Merged-B ingress (design §2.3a, release side) ===

TEST(NmuCollective, MergedBWithCollectiveBitsRetiresLikeAUnicastB) {
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
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    feed_b(t, 0x05, axi::COLLECTIVE_OP_MULTICAST, 0x03);
    ASSERT_TRUE(t.rob.pop_b().has_value());
    feed_b(t, 0x05, axi::COLLECTIVE_OP_MULTICAST, 0x03);
    EXPECT_DEATH(t.rob.pop_b(), "does not match the head");
}

// design §2.3a: the write_txns_ underflow assert is the single-merged-B invariant, injected
// directly here since the order-list head check shadows it on pop_b.
TEST(NmuCollectiveDeath, RetireWithoutAnOutstandingWriteAborts) {
    CollectiveTestbench t;
    ASSERT_TRUE(t.rob.push_aw(
        make_aw(0x05, tile_addr(0, 1), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))));
    retire_b(t, 0x05);
    EXPECT_DEATH(t.rob.retire_b(/*ordering_req=*/false, /*ordering_tag=*/0, 0x05),
                 "no outstanding write transaction");
}

// === Reject set (design §2.3) -- one death test per row ===

// spec §2.2 matrix: collective_op=UNICAST with nonzero mask is a stimulus contradiction, rejected
// rather than normalized.
TEST(NmuCollectiveDeath, MaskWithoutOp) {
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_UNICAST, 0x1000))),
        "UNICAST requires a zero mask");
}

// spec §2.2 matrix: collective_op=MULTICAST with a zero mask names an empty destination set,
// rejected rather than downgraded to unicast.
TEST(NmuCollectiveDeath, OpWithoutMask) {
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0))),
        "empty destination set");
}

// spec §2.2 / §6 header table: collective_op 2-3 are reserved.
TEST(NmuCollectiveDeath, ReservedOp) {
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(2, 0x1000))),
                 "reserved AWUSER collective_op");
}

// spec §2.3/§6.1: AxLOCK is unicast-only; a collective is never an exclusive access.
TEST(NmuCollectiveDeath, AwLockOnCollective) {
    CollectiveTestbench t;
    auto aw = make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    aw.lock = 1;
    EXPECT_DEATH(t.rob.push_aw(aw), "AWLOCK set on a collective AW");
}

// spec §2.3: a collective's destination set cannot straddle the Narrow/Data classes even though
// both are multicast-eligible; the class picks the network.
TEST(NmuCollectiveDeath, ReplicasStraddleTheClasses) {
    addr_trans::SamTable sam({{0x0000, kTile, 0x00, axi::AxiClass::Narrow},
                              {0x1000, kTile, 0x01, axi::AxiClass::Narrow},
                              {0x2000, kTile, 0x00, axi::AxiClass::Data},
                              {0x3000, kTile, 0x01, axi::AxiClass::Data}});
    ASSERT_TRUE(declare(sam, axi::AxiClass::Narrow, 12, 2, 1));
    ASSERT_TRUE(declare(sam, axi::AxiClass::Data, 12, 2, 1));
    CollectiveTestbench t(std::move(sam));
    auto aw = make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x2000));
    aw.size = 3;  // narrow request address: AWSIZE <= 3 (8 B), orthogonal to the class rule
    EXPECT_DEATH(t.rob.push_aw(aw), "coordinate ranges of the request address");
}

// spec §2.2 check 1: a mask wider than the node-id field names more destinations than the mesh has
// nodes.
TEST(NmuCollectiveDeath, MoreMaskBitsThanNodeIdBits) {
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, tile_addr(0, 0),
                                       awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1FF000))),
                 "coordinate ranges of the request address");
}

// spec §2.2 check 2 (:456-462): a mask bit below the node-index field would wildcard an address bit
// inside one region.
TEST(NmuCollectiveDeath, MaskBitInsideTheTileOffset) {
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x20))),
        "coordinate ranges of the request address");
}

// spec §2.2 check 3: a mask whose named addresses collapse onto the same node is rejected at
// declaration, not on every request.
TEST(NmuCollectiveDeath, DuplicateNodeInTheDestinationSet) {
    auto sam = addr_trans::SamTable::packed(
        {{0, 0, kTile}, {1, 0, kTile}, {2, 0, kTile}, {2, 0, kTile}}, /*x_span=*/4, /*y_span=*/1,
        /*block_size=*/kTile);
    EXPECT_FALSE(declare(sam, axi::AxiClass::Data, 12, 4, 1));
    CollectiveTestbench t(std::move(sam));
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))),
                 "not a collective target");
}

// spec §2.2 check 4: a mask bit past the node-index field must be caught here, never surface as a
// route_mask_fork out-of-mesh abort.
TEST(NmuCollectiveDeath, MaskBitOutsideTheMesh) {
    CollectiveTestbench t;
    EXPECT_DEATH(t.rob.push_aw(
                     make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x10000))),
                 "coordinate ranges of the request address");
}

// spec §2.2 check 4: on a non-power-of-two dimension, clamping a wildcard bound can land on a
// coordinate the raw set never named (mask 0x2 at x=1 -> raw {1,3}, clip lands on 2) -- rejected,
// unlike the design's worked full-range example 0b11={0,1,2,3}.
TEST(NmuCollective, MaskReachingAPaddingCoordinateClipsToTheTileRegion) {
    auto sam = addr_trans::SamTable::packed({{0, 0, kTile}, {1, 0, kTile}, {2, 0, kTile}},
                                            /*x_span=*/3, /*y_span=*/1, /*block_size=*/kTile);
    ASSERT_TRUE(declare(sam, axi::AxiClass::Data, 12, 3, 1));
    CollectiveTestbench t(std::move(sam));
    // Based at x = 0 the mask names {0, 2}, both real nodes -- no clipping.
    ASSERT_TRUE(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x2000))));
    auto f = t.aw_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("collective_mask"), 0x02u);
    // Based at x = 1 the raw set is {1, 3}; clip_max_x clamps to 2, which is
    // not a member of {1, 3} -- rejected rather than silently forwarded as {1}.
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x06, kTile, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x2000))),
                 "member of the wildcard set");
}

// spec §2.2 check 3: the nodes a mask names must form an aligned wildcard over dst_id, not just a
// same-count set.
TEST(NmuCollectiveDeath, NodeSetIsNotAnAlignedWildcard) {
    auto sam = addr_trans::SamTable::packed(
        {{0, 0, kTile}, {1, 0, kTile}, {2, 1, kTile}, {3, 1, kTile}}, /*x_span=*/4, /*y_span=*/2,
        /*block_size=*/kTile);
    EXPECT_FALSE(declare(sam, axi::AxiClass::Data, 12, 2, 2));
    CollectiveTestbench t(std::move(sam));
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x3000))),
                 "not a collective target");
}

// spec §2.1: push_aw_with_meta only rechecks meta-vs-AWUSER agreement; a mismatch here is a model
// bug in the translating layer, not a stimulus error.
TEST(NmuCollectiveDeath, MetaDisagreesWithAwuser) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, mesh_sam());
    auto aw = make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000));
    ni::cmodel::nmu::AwHeaderMeta meta{0x00, 0, 0, 0};  // collective fields left at UNICAST / 0
    EXPECT_DEATH(pkt.push_aw_with_meta(aw, meta), "disagrees with AWUSER");
}

// spec §2.1: the op/mask agreement guard also catches a translate that returns MULTICAST paired
// with an empty node mask.
TEST(NmuCollectiveDeath, MetaOpAndMaskDisagree) {
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
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, mesh_sam());
    auto aw = make_aw(0x05, tile_addr(0, 0));
    aw.user = uint64_t{1} << 60;
    EXPECT_DEATH(pkt.push_aw_with_meta(aw, ni::cmodel::nmu::AwHeaderMeta{0x00, 0, 0, 0}),
                 "above the field width");
}

// spec §2.2 check 5 (:461-462): every replica's region must cover the full burst footprint; testing
// the request address's footprint suffices since region size is uniform across a
// collective-eligible space.
TEST(NmuCollectiveDeath, BurstOverrunsTheRegion) {
    CollectiveTestbench t;
    EXPECT_DEATH(
        t.rob.push_aw(make_aw(0x05, tile_addr(0, 0), awuser(axi::COLLECTIVE_OP_MULTICAST, 0x1000),
                              /*len=*/191)),
        "burst footprint crosses a tile");
}

// spec §2.2 check 5: a space whose replica regions differ in size cannot share one footprint check,
// so it is rejected at declaration as a unicast-only target.
TEST(NmuCollectiveDeath, NonUniformRegionSizeIsNotACollectiveTarget) {
    addr_trans::SamTable sam(
        {{0x0000, 2 * kTile, 0x00}, {0x2000, kTile, 0x01}, {0x3000, kTile, 0x02}});
    EXPECT_FALSE(declare(sam, axi::AxiClass::Data, 12, 3, 1));
    CollectiveTestbench t(std::move(sam));
    EXPECT_DEATH(t.rob.push_aw(make_aw(0x05, 0x0000, awuser(axi::COLLECTIVE_OP_MULTICAST, 0x2000))),
                 "not a collective target");
}
