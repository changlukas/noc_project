#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <array>
#include <gtest/gtest.h>

using ni::cmodel::nsu::AxiClass;
using ni::cmodel::nsu::Depacketize;
using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_aw_flit(uint8_t awid, uint64_t addr, uint8_t src_id = 0x10,
                              uint8_t ordering_req = 0, uint8_t ordering_tag = 0,
                              uint8_t axi_ch = ni::AXI_CH_NarrowAw) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", ordering_req);
    f.set_header_field("ordering_tag", ordering_tag);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", addr);
    f.set_payload_field("AW", "awsize", 5);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}
ni::cmodel::Flit make_w_flit(uint32_t strb, bool last, uint8_t axi_ch = ni::AXI_CH_NarrowW) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("flit_tail", 1);
    const char* ch = (axi_ch == ni::AXI_CH_DataW) ? "DATA_W" : "NARROW_W";
    f.set_payload_field(ch, "wlast", last ? 1u : 0u);
    f.set_payload_field(ch, "wstrb", strb);
    return f;
}
ni::cmodel::Flit make_ar_flit(uint8_t arid, uint64_t addr, uint8_t src_id = 0x10) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowAr);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("AR", "arid", arid);
    f.set_payload_field("AR", "araddr", addr);
    f.set_payload_field("AR", "arsize", 5);
    f.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}
}  // namespace

TEST(NsuDepacketize, AwFlitSnapshotsMetadataAndPopsBeat) {
    SCENARIO(
        "NSU Depacketize: AW flit allocates src_id/ordering_req/ordering_tag into MetaBuffer + "
        "emits AW "
        "beat");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000,
                                                     /*src*/ 0x12, /*ordering_req*/ 1,
                                                     /*ordering_tag*/ 3)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->id, 0x05);
    EXPECT_EQ(aw->addr, 0x1000u);
    // MetaBuffer entry
    auto m = mb.peek_write(0x05);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->src_id, 0x12);
    EXPECT_EQ(m->ordering_req, 1);
    EXPECT_EQ(m->ordering_tag, 3);
}

TEST(NsuDepacketize, DataAwFlitAcceptedAndRecordsDataClass) {
    SCENARIO(
        "NSU Depacketize: a DataAw flit (axi_ch=AXI_CH_DataAw) is accepted into the data-class "
        "s1 AW register (no abort) and the MetaBuffer entry records cls=Data, so the "
        "eventual B response is stamped in the same class");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000, /*src*/ 0x12,
                                                     /*ordering_req*/ 0, /*ordering_tag*/ 0,
                                                     ni::AXI_CH_DataAw)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->id, 0x05);
    auto m = mb.peek_write(0x05);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->cls, AxiClass::Data);
}

TEST(NsuDepacketize, DataWFlitDecodesFromDataWChannel) {
    SCENARIO(
        "NSU Depacketize: a DataW flit (axi_ch=AXI_CH_DataW) decodes its wstrb/wlast from the "
        "DATA_W payload channel, not NARROW_W");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    // The W stream follows the AW stream (AXI Channel Assignment), so a beat is
    // only poppable once its AW has been admitted. One single-beat DataAw ahead
    // of it is the smallest setup that satisfies that; the decode under test is
    // unaffected by it.
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAB, true, ni::AXI_CH_DataW)));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    auto w = depkt.pop_w();
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w->strb, 0xABu);
    EXPECT_TRUE(w->last);
}

TEST(NsuDepacketize, AwqosRecoveredFromFlit) {
    SCENARIO(
        "NSU Depacketize: awqos=0xA in AW flit payload is recovered into AwBeat.qos "
        "(verifies awqos is not forced to 0)");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    auto flit = make_aw_flit(0x05, 0x1000, /*src*/ 0x12, /*ordering_req*/ 0, /*ordering_tag*/ 0);
    flit.set_payload_field("AW", "awqos", 0xA);
    ASSERT_TRUE(noc.req_out().push_flit(flit));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->qos, 0xAu);
}

TEST(NsuDepacketize, ArqosRecoveredFromFlit) {
    SCENARIO(
        "NSU Depacketize: arqos=0xA in AR flit payload is recovered into ArBeat.qos "
        "(verifies arqos is not forced to 0)");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    auto flit = make_ar_flit(0x07, 0x2000, /*src*/ 0x12);
    flit.set_payload_field("AR", "arqos", 0xA);
    ASSERT_TRUE(noc.req_out().push_flit(flit));
    depkt.tick();
    auto ar = depkt.pop_ar();
    ASSERT_TRUE(ar.has_value());
    EXPECT_EQ(ar->qos, 0xAu);
}

TEST(NsuDepacketize, ArFlitSnapshotsReadMeta) {
    SCENARIO("NSU Depacketize: AR flit allocates read-side meta into MetaBuffer + emits AR beat");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    // The AW ahead of it only satisfies the W-follows-AW order; it allocates
    // under its own id (0x05, identity remap at max_unique_ids 256), so key 0
    // stays the untouched-by-W witness this test is about.
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFFFF, true, ni::AXI_CH_DataW)));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_TRUE(depkt.pop_w().has_value());
    // MetaBuffer untouched
    EXPECT_FALSE(mb.peek_write(0).has_value());
}

TEST(NsuDepacketize, NarrowWAfterDataWReadsOwnAwNotStaleFifoEntry) {
    SCENARIO(
        "NSU Depacketize: a data-class AW+W fully drained, then a narrow-class AW+W at byte_lane "
        "32 -- the narrow W must decode its lane from its OWN AW, not a leaked/stale data-class "
        "w_addr_fifo_ entry (site 2's FIFO must be narrow-only, mirroring nmu::Rob's "
        "ar_lane_meta_: decode_w's data branch never pops, so an unconditional push on every AW "
        "either leaks -- data class -- or, worse, hands the next narrow AW's W beats someone "
        "else's address basis)");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);

    // 1. Data-class AW+W, fully drained through pop_aw/pop_w (real usage).
    ASSERT_TRUE(
        noc.req_out().push_flit(make_aw_flit(0x01, 0x1000, /*src*/ 0x10, 0, 0, ni::AXI_CH_DataAw)));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFFFFFFFF, /*last=*/true, ni::AXI_CH_DataW)));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_w().has_value());

    // 2. Narrow-class AW at byte_lane 32 (addr & 63 == 32), size=2 (4 B/beat), + its W beat.
    constexpr uint64_t kAddr = 0x20;      // 0x20 & 63 == 32
    auto aw = make_aw_flit(0x02, kAddr);  // axi_ch defaults to AXI_CH_NarrowAw
    aw.set_payload_field("AW", "awsize", 2);
    ASSERT_TRUE(noc.req_out().push_flit(aw));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());

    ni::cmodel::Flit w = make_w_flit(0xF, /*last=*/true);  // beat-relative 4-bit strb, all lanes
    std::array<uint8_t, axi::NARROW_DATA_BYTES> lane_bytes{};
    for (int i = 0; i < 4; ++i) lane_bytes[i] = static_cast<uint8_t>(0xC0 + i);
    w.set_payload_bytes("NARROW_W", "wdata", lane_bytes.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    ASSERT_TRUE(noc.req_out().push_flit(w));
    depkt.tick();
    auto wb = depkt.pop_w();
    ASSERT_TRUE(wb.has_value());
    EXPECT_EQ(wb->strb, 0xFull << 32) << "lane must come from this beat's own AW (addr 0x20), "
                                         "not the leaked data-class AW (addr 0x1000, lane 0)";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(wb->data[32 + i], static_cast<uint8_t>(0xC0 + i));
}

TEST(NsuDepacketize, NarrowWUnalignedAddrInsertsAtCorrectLane) {
    SCENARIO(
        "NSU Depacketize: narrow class decode_w re-inserts the addressed 8B lane from a "
        "genuinely unaligned AW local_addr (not a multiple of the beat size) -- site 2 of the S2 "
        "design doc's lane re-anchor table");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);

    constexpr uint64_t kUnalignedAddr = 0x1B;      // 27, not a multiple of 4 (the beat size)
    auto aw = make_aw_flit(0x02, kUnalignedAddr);  // axi_ch defaults to AXI_CH_NarrowAw
    aw.set_payload_field("AW", "awsize", 2);       // 4 B/beat -- legal narrow (<=3)
    ASSERT_TRUE(noc.req_out().push_flit(aw));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());

    ni::cmodel::Flit w = make_w_flit(0xF, /*last=*/true);  // beat-relative 4-bit strb, all lanes
    std::array<uint8_t, axi::NARROW_DATA_BYTES> lane_bytes{};
    for (int i = 0; i < 4; ++i) lane_bytes[i] = static_cast<uint8_t>(0xD0 + i);
    w.set_payload_bytes("NARROW_W", "wdata", lane_bytes.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    ASSERT_TRUE(noc.req_out().push_flit(w));
    depkt.tick();
    auto wb = depkt.pop_w();
    ASSERT_TRUE(wb.has_value());

    // narrow_lane(0x1B) = (0x1B >> 3) & 7 = 3 -> byte offset 24: neither the
    // beat's own address (27) nor a size-aligned/rounded value.
    constexpr unsigned kByteOffset = 24;
    EXPECT_EQ(wb->strb, 0xFull << kByteOffset);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(wb->data[kByteOffset + i], static_cast<uint8_t>(0xD0 + i));
}

TEST(NsuDepacketize, DemuxMixedAwWAr) {
    SCENARIO(
        "NSU Depacketize: interleaved AW/W/AR flits demux to per-channel queues by axi_ch header");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x01, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFF, true)));
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x02, 0x1000)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 0x01);
    EXPECT_EQ(depkt.pop_w()->strb, 0xFFu);
    EXPECT_EQ(depkt.pop_ar()->id, 0x02);
}

TEST(NsuDepacketize, PendingHolBlockingS1WFullBlocksAwBehind) {
    SCENARIO(
        "NSU Depacketize: HoL S1 W register full holds pending W; AW behind blocked until W "
        "drained");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    // Order: AW(2 beats), W, W, AW -- data class throughout. The leading AW owns
    // both W beats, which is what makes them poppable at all now; the mechanic
    // under test is unchanged, the second W still stalls in the ingress stash
    // and the AW behind it is still blocked.
    auto aw_owner = make_aw_flit(0x06, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw);
    aw_owner.set_payload_field("AW", "awlen", 1);  // 2 beats
    ASSERT_TRUE(noc.req_out().push_flit(aw_owner));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAA, false, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xBB, true, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x07, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());   // owning AW admitted
    EXPECT_TRUE(depkt.pop_w().has_value());    // first W (0xAA) demuxed
    EXPECT_FALSE(depkt.pop_aw().has_value());  // AW blocked behind pending W
    depkt.tick();
    EXPECT_TRUE(depkt.pop_w().has_value());  // pending W (0xBB)
    depkt.tick();
    EXPECT_TRUE(depkt.pop_aw().has_value());  // AW now demuxed
}

// NsuDepacketize::PopBAssertFalse was a runtime wrong_side_() test.
// The method no longer exists on nsu::Depacketize; wrong-side
// calls are now caught at compile time. Test removed.

// Tick-cardinality: S1 register holds <=1 AW per tick.
// For 3 sequential AW flits on the same channel, tick 3 times — one flit
// per tick — to drain all 3. FIFO order coverage is unchanged.
TEST(NsuDepacketize, FifoOrderPreservedAcrossChannels) {
    SCENARIO(
        "NSU Depacketize: AW S1 register preserves NoC arrival order across 3 sequential AW flits "
        "(one per tick per staged S1 contract)");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(1, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(2, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(3, 0x0)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 1);  // flit 1 decoded into S1 this tick
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 2);  // flit 2 decoded into S1 next tick
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 3);  // flit 3 decoded into S1 third tick
}

TEST(NsuDepacketize, CtorRejectsIntermediateMaxUniqueIds) {
    SCENARIO(
        "NSU Depacketize: max_unique_ids must be 1 (collapse) or AXI_ID_SPACE (passthrough); an "
        "intermediate value throws (config trust boundary, fail-loud even under NDEBUG)");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // FlooNoC provides only collapse-or-passthrough; an intermediate N is unsupported.
    EXPECT_THROW(Depacketize(noc.req_in(), mb, /*max_unique_ids*/ 5), std::invalid_argument);
    EXPECT_THROW(Depacketize(noc.req_in(), mb, /*max_unique_ids*/ 0), std::invalid_argument);
    // Both legal endpoints construct without throwing.
    EXPECT_NO_THROW(Depacketize(noc.req_in(), mb, /*collapse*/ 1));
    EXPECT_NO_THROW(Depacketize(noc.req_in(), mb, axi::AXI_ID_SPACE));
}

// --- Node-coordinate rebase (Stage 2b) -------------------------------------
// A collective replica arrives carrying the ANCHOR's address, because one
// masked AW reaches N nodes unchanged. The NSU overwrites the coordinate field
// with its own so the tile behind it decodes an address that names itself.

namespace {
// A 4x4 memory space of 0x100000-byte tiles: node index in addr[23:20], X in
// [21:20] and Y in [23:22] (raster order, X fastest).
ni::cmodel::address_map::SpaceCoords mem_coords_4x4() {
    ni::cmodel::address_map::SpaceCoords c;
    c.x_count = 4;
    c.y_count = 4;
    c.x_range = {20, 2};
    c.y_range = {22, 2};
    return c;
}
std::array<ni::cmodel::address_map::SpaceCoords, 2> coords_for_narrow() {
    std::array<ni::cmodel::address_map::SpaceCoords, 2> a{};
    a[static_cast<unsigned>(axi::AxiClass::Narrow)] = mem_coords_4x4();
    return a;
}
}  // namespace

TEST(NsuDepacketize, RebasesAReplicaAddressOntoThisNode) {
    SCENARIO("NSU Depacketize: an AW carrying another node's address is rewritten to this node");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // src_id 0x21 = (y=2 << X_WIDTH) | x=1.
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256, ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21, coords_for_narrow());
    // Anchor address names node (0,0); the offset inside the tile is 0x3c0.
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x0003c0)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    // x=1 at bit 20, y=2 at bit 22 -> 0x9003c0.
    EXPECT_EQ(aw->addr, 0x9003c0u);
}

TEST(NsuDepacketize, RebaseIsTheIdentityForAUnicastAlreadyNamingThisNode) {
    SCENARIO("NSU Depacketize: an address already naming this node survives the rebase unchanged");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256, ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21, coords_for_narrow());
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x07, 0x9003c0)));
    depkt.tick();
    auto ar = depkt.pop_ar();
    ASSERT_TRUE(ar.has_value());
    EXPECT_EQ(ar->addr, 0x9003c0u);
}

TEST(NsuDepacketize, UndeclaredCoordsLeaveTheAddressAlone) {
    SCENARIO("NSU Depacketize: with no coordinate field declared the address is forwarded as-is");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ 256, ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x0003c0)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->addr, 0x0003c0u);
}
