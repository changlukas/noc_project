#include "nmu/packetize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "axi/types.hpp"
#include <array>
#include <utility>
#include <gtest/gtest.h>

using ni::cmodel::nmu::Packetize;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;
namespace addr_trans = ni::cmodel::nmu::addr_trans;

namespace {
constexpr uint8_t kSrcId = 0x12;

// 16x16 uniform, 4 GB/tile: dst = addr[39:32] (256 tiles cover every dst_id,
// matching the retired xy_route decode); awaddr is the request address,
// forwarded unchanged. Legacy dst-byte expectations below still hold.
addr_trans::SamTable legacy_sam() {
    return addr_trans::SamTable::uniform(16, 16, 0x100000000ull);
}

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = len;
    b.size = 5;
    b.burst = axi::Burst::INCR;
    b.cache = 0xF;
    b.lock = 0;
    b.prot = 0;
    b.region = 0;
    b.user = 0;
    b.qos = 0;
    return b;
}
axi::WBeat make_w(uint32_t strb, bool last) {
    axi::WBeat b{};
    for (int i = 0; i < 32; ++i) b.data[i] = static_cast<uint8_t>(i);
    b.strb = strb;
    b.last = last;
    b.user = 0;
    return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = 0;
    b.size = 5;
    b.burst = axi::Burst::INCR;
    b.cache = 0;
    b.lock = 0;
    b.prot = 0;
    b.region = 0;
    b.user = 0;
    b.qos = 0;
    return b;
}
}  // namespace

TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    // Legacy test: only verifies packetize stamps src + axi_ch + flit_tail + awid +
    // awaddr. dst_id derivation is covered by WMetaFifoInheritsAwDst below.
    // Address is the low 40 bits of the original 0xDEADBEEFCAFEBABE pattern:
    // the legacy SAM covers addr < 2^40 (256 tiles x 4GB), unlike xy_route
    // which masked dst_id and tolerated any 64-bit address.
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0xEFCAFEBABEull)));

    auto flit_opt = aw_cap.pop();
    ASSERT_TRUE(flit_opt.has_value());
    const auto& f = *flit_opt;
    // legacy_sam() is memory space (no "space" annotation -> data class).
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataAw);
    EXPECT_EQ(f.get_header_field("src_id"), kSrcId);
    EXPECT_EQ(f.get_header_field("vc_id"), 0u);
    EXPECT_EQ(f.get_header_field("flit_tail"), 0u);  // AW starts wormhole packet (FlooNoC)
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0x05u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xEFCAFEBABEull);  // forwarded unchanged
}

TEST(NmuPacketize, StampsItsOwnPortAndTheSamEntrysPortIntoTheHeader) {
    // A SAM entry whose port is 1 is what a peripheral destination will look
    // like in round 3. No shipped topology declares one yet, so the value is
    // the fixture's.
    addr_trans::SamTable sam{
        {{/*base=*/0x0, /*size=*/0x1000, /*dst_id=*/0x11, axi::AxiClass::Data, /*port=*/1}}};
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, std::move(sam), /*port_id=*/2);
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x40)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ true)));

    auto flit_opt = aw_cap.pop();
    ASSERT_TRUE(flit_opt.has_value());
    EXPECT_EQ(flit_opt->get_header_field("dst_port_id"), 1u);
    EXPECT_EQ(flit_opt->get_header_field("src_port_id"), 2u);
    // The W beat inherits dst_port from its AW through w_meta_fifo_, the same
    // way it inherits dst_id -- both halves of the worm must reach one endpoint.
    auto w_flit_opt = w_cap.pop();
    ASSERT_TRUE(w_flit_opt.has_value());
    EXPECT_EQ(w_flit_opt->get_header_field("dst_port_id"), 1u);
    EXPECT_EQ(w_flit_opt->get_header_field("src_port_id"), 2u);
    // The read path reaches the SAM through its own push_ar, so the AW's
    // coverage says nothing about it.
    ASSERT_TRUE(pkt.push_ar(make_ar(0x06, 0x40)));
    auto ar_flit_opt = ar_cap.pop();
    ASSERT_TRUE(ar_flit_opt.has_value());
    EXPECT_EQ(ar_flit_opt->get_header_field("dst_port_id"), 1u);
    EXPECT_EQ(ar_flit_opt->get_header_field("src_port_id"), 2u);
}

TEST(NmuPacketize, WMetaFifoInheritsAwDst) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    // addr 0x3400000000 → dst = (0x3400000000 >> 32) & 0xFF = 0x34
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x3400000000)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ true)));

    aw_cap.pop();  // discard AW
    auto w_flit_opt = w_cap.pop();
    ASSERT_TRUE(w_flit_opt.has_value());
    EXPECT_EQ(w_flit_opt->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(w_flit_opt->get_header_field("axi_ch"), ni::AXI_CH_DataW);
}

TEST(NmuPacketize, MultiOutstandingAwInterleavedW) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    // addr 0x3400000000 → dst=0x34;  addr 0x5600000000 → dst=0x56.
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x3400000000)));
    ASSERT_TRUE(pkt.push_aw(make_aw(0x06, 0x5600000000)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/ true)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/ true)));

    ASSERT_EQ(aw_cap.size() + w_cap.size() + ar_cap.size(), 4u);
    aw_cap.pop();  // AW1
    aw_cap.pop();  // AW2
    auto w1 = w_cap.pop();
    auto w2 = w_cap.pop();
    EXPECT_EQ(w1->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(w2->get_header_field("dst_id"), 0x56u);
}

// Regression guard: fixes a pre-existing bug where every W flit stamped header.flit_tail=1 instead
// of only the terminal beat (FlooNoC wormhole packet-boundary semantic).
TEST(NmuPacketize, WHeaderFlitTailMatchesWlast) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    ASSERT_TRUE(pkt.push_aw(make_aw(0x07, 0x340000, /*len*/ 2)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ false)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ false)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ true)));

    aw_cap.pop();  // discard AW
    for (int i = 0; i < 3; ++i) {
        auto f = w_cap.pop();
        ASSERT_TRUE(f.has_value());
        uint64_t expected_flit_tail = (i == 2) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("flit_tail"), expected_flit_tail)
            << "W beat " << i << ": header.flit_tail expected " << expected_flit_tail;
        EXPECT_EQ(f->get_payload_field("NARROW_W", "wlast"), expected_flit_tail);
    }
}

TEST(NmuPacketize, PushAwFailsOnNocFull) {
    ChannelModel noc(/*req*/ 1, /*rsp*/ 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, legacy_sam());
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    EXPECT_FALSE(pkt.push_aw(make_aw(1, 0)));
    noc.req_in().pop_flit();
    EXPECT_TRUE(pkt.push_aw(make_aw(1, 0)));
}

TEST(NmuPacketize, AwPayloadBitPerfect) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    // Address is the low 40 bits of the original 0x123456789ABCDEF0 ascending
    // pattern: the legacy SAM covers addr < 2^40 (256 tiles x 4GB).
    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x789ABCDEF0ull, /*len*/ 0xFF);
    aw.size = 5;
    aw.burst = axi::Burst::WRAP;
    aw.cache = 0xF;
    aw.lock = 1;
    aw.prot = 0x7;
    aw.region = 0xF;
    aw.user = 0xFF;
    aw.qos = 0xF;
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0x03u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x789ABCDEF0ull);  // forwarded unchanged
    EXPECT_EQ(f.get_payload_field("AW", "awlen"), 0xFFu);
    EXPECT_EQ(f.get_payload_field("AW", "awsize"), 5u);
    EXPECT_EQ(f.get_payload_field("AW", "awburst"), static_cast<uint64_t>(axi::Burst::WRAP));
    EXPECT_EQ(f.get_payload_field("AW", "awcache"), 0xFu);
    EXPECT_EQ(f.get_payload_field("AW", "awlock"), 1u);
    EXPECT_EQ(f.get_payload_field("AW", "awprot"), 0x7u);
    EXPECT_EQ(f.get_payload_field("AW", "awregion"), 0xFu);
    EXPECT_EQ(f.get_payload_field("AW", "awuser"), 0xFFu);
}

TEST(NmuPacketize, AwuserStripsCollectiveBitsWhenZero) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());

    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = 0x5Au;  // no collective bits set
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_payload_field("AW", "awuser"), 0x5Au);  // AWUSER[57:8] never reaches payload
}

TEST(NmuPacketizeDeath, NonzeroCollectiveOpAborts) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = (uint64_t{1} << 8) | 0x5Au;  // collective_op=1, user-defined=0x5A
    EXPECT_DEATH(pkt.push_aw(aw), "collective_op/collective_mask");
}

TEST(NmuPacketizeDeath, NonzeroCollectiveMaskAborts) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = (uint64_t{0xFFFFFFFFFFFFull} << 10) | 0x5Au;  // nonzero collective_mask only
    EXPECT_DEATH(pkt.push_aw(aw), "collective_op/collective_mask");
}

TEST(NmuPacketize, AwqosRoundTrip) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto aw = make_aw(/*id*/ 0x01, /*addr*/ 0x340000);
    aw.qos = 0xA;
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_payload_field("AW", "awqos"), 0xAu);
}

TEST(NmuPacketize, ArqosRoundTrip) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto ar = make_ar(/*id*/ 0x02, /*addr*/ 0x990000);
    ar.qos = 0xA;
    ASSERT_TRUE(pkt.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_payload_field("AR", "arqos"), 0xAu);
}

TEST(NmuPacketize, WPayloadBitPerfect) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    auto w = make_w(0xDEADBEEF, /*last*/ true);
    w.user = 0xAB;
    ASSERT_TRUE(pkt.push_w(w));
    aw_cap.pop();  // discard AW
    auto f = *w_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataW);
    EXPECT_EQ(f.get_payload_field("DATA_W", "wlast"), 1u);
    EXPECT_EQ(f.get_payload_field("DATA_W", "wstrb"), 0xDEADBEEFu);
    EXPECT_EQ(f.get_payload_field("DATA_W", "wuser"), 0xABu);
    std::array<uint8_t, axi::DATA_BYTES> wdata_out{};
    f.get_payload_bytes("DATA_W", "wdata", wdata_out.data(), ni::width::NOC_DATA_WIDTH);
    for (int i = 0; i < 32; ++i) EXPECT_EQ(wdata_out[i], static_cast<uint8_t>(i));
}

TEST(NmuPacketize, NarrowWUnalignedAddrExtractsCorrectLane) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId,
                  addr_trans::SamTable{});  // sam_ unused by *_with_meta

    axi::AwBeat b = make_aw(/*id=*/0x01, /*addr=*/0);  // addr unused: meta.local_addr supplies it
    b.size = 2;                                        // 4 B/beat -- legal narrow (<=3)
    constexpr uint64_t kUnalignedAddr = 0x1B;          // 27, not a multiple of 4 (the beat size)
    ni::cmodel::nmu::AwHeaderMeta meta{/*dst_id=*/0x03, kUnalignedAddr, /*ordering_req=*/0,
                                       /*ordering_tag=*/0, axi::AxiClass::Narrow};
    ASSERT_TRUE(pkt.push_aw_with_meta(b, meta));
    aw_cap.pop();  // discard AW

    axi::WBeat w{};
    for (int i = 0; i < axi::DATA_BYTES; ++i) w.data[i] = static_cast<uint8_t>(i);
    w.strb = axi::kFullStrbMask;
    w.last = true;
    ASSERT_TRUE(pkt.push_w(w));
    auto f = *w_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_NarrowW);

    // narrow_lane(0x1B) = (0x1B >> 3) & 7 = 3 -> byte offset 3*8 = 24: neither the
    // beat's own address (27) nor a size-aligned/rounded value.
    constexpr unsigned kByteOffset = 24;
    std::array<uint8_t, axi::NARROW_DATA_BYTES> out{};
    f.get_payload_bytes("NARROW_W", "wdata", out.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    for (int i = 0; i < axi::NARROW_DATA_BYTES; ++i)
        EXPECT_EQ(out[i], static_cast<uint8_t>(kByteOffset + i));
    EXPECT_EQ(f.get_payload_field("NARROW_W", "wstrb"), 0xFFu);
}

TEST(NmuPacketize, ArEncodesAxiChAndOrderingTag) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    // addr 0x9900004000 -> dst = (0x9900004000 >> 32) & 0xFF = 0x99; araddr
    // stays 0x9900004000. Direct-path interface
    // auto-fills ordering_req/ordering_tag = 0; Rob-driven path uses push_ar_with_meta
    // (covered by PushAwWithMeta_OverrideDefault).
    ASSERT_TRUE(pkt.push_ar(make_ar(0x07, 0x9900004000)));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataAr);
    EXPECT_EQ(f.get_header_field("dst_id"), 0x99u);
    EXPECT_EQ(f.get_header_field("ordering_req"), 0u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);
    EXPECT_EQ(f.get_payload_field("AR", "arid"), 0x07u);
    EXPECT_EQ(f.get_payload_field("AR", "araddr"), 0x9900004000ull);  // forwarded unchanged
}

TEST(NmuPacketize, RsvdAndDisabledFieldsZero) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    auto f = *aw_cap.pop();
    EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(NmuPacketize, PushAwWithMeta_OverrideDefault) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, /*src=*/0x01, legacy_sam());
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x100);  // addr → dst=0 by default
    ni::cmodel::nmu::AwHeaderMeta meta{/*dst_id=*/0x42,
                                       /*local_addr=*/0x9999,
                                       /*ordering_req=*/1,
                                       /*ordering_tag=*/0x07};
    ASSERT_TRUE(pkt.push_aw_with_meta(b, meta));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x42u);
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0x07u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x9999u);  // meta.local_addr, NOT b.addr
}

TEST(NmuPacketize, AddrTransIntegratedDstIdInHeader) {
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, /*src=*/0x01, legacy_sam());
    // addr 0x100000100 -> tile 1 (0x100000100 / 4GB = 1); the address itself is not touched
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x100000100);
    ASSERT_TRUE(pkt.push_aw(b));  // direct-path interface auto-computes
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);                  // from SamTable::translate
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x100000100ull);  // forwarded unchanged
}

TEST(NmuPacketize, SamTranslateSetsDstFromTableAndKeepsTheAddress) {
    ReqCapture aw_cap, w_cap, ar_cap;
    // Single packed tile at (2,1) -> dst_id 0x12, base ((1<<2)|2) * 4 GB =
    // 0x600000000 (x_span = 3 -> x_bits = 2).
    auto sam = addr_trans::SamTable::packed({{2, 1, 0x100000000ull}}, /*x_span=*/3, /*y_span=*/2,
                                            /*block_size=*/0x100000000ull);
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, /*src_id=*/0, sam);
    axi::AwBeat aw{};
    aw.addr = 0x600000040ull;
    aw.id = 5;
    aw.len = 0;
    aw.size = 3;
    aw.burst = axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x12u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x600000040ull);
}
