#include "nmu/packetize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::Packetize;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x12;

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
    SCENARIO(
        "NMU Packetize: push_aw stamps src_id/axi_ch=AW/vc=0/last=0/awid/awaddr on emitted flit "
        "(AW starts wormhole packet)");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    // Legacy test: only verifies packetize stamps src + axi_ch + last + awid +
    // awaddr. dst_id derivation is covered by WMetaFifoInheritsAwDst below.
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0xDEADBEEFCAFEBABEull)));

    auto flit_opt = aw_cap.pop();
    ASSERT_TRUE(flit_opt.has_value());
    const auto& f = *flit_opt;
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f.get_header_field("src_id"), kSrcId);
    EXPECT_EQ(f.get_header_field("vc_id"), 0u);
    EXPECT_EQ(f.get_header_field("last"), 0u);  // AW starts wormhole packet (FlooNoC)
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0x05u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xDEADBEEFCAFEBABEull);
}

TEST(NmuPacketize, WMetaFifoInheritsAwDst) {
    SCENARIO("NMU Packetize: W flit inherits dst_id from preceding AW via W-meta FIFO");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    // addr 0x340000 → dst = (0x340000 >> 16) & 0xFF = 0x34
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x340000)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ true)));

    aw_cap.pop();  // discard AW
    auto w_flit_opt = w_cap.pop();
    ASSERT_TRUE(w_flit_opt.has_value());
    EXPECT_EQ(w_flit_opt->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(w_flit_opt->get_header_field("axi_ch"), ni::AXI_CH_W);
}

TEST(NmuPacketize, MultiOutstandingAwInterleavedW) {
    SCENARIO("NMU Packetize: 2 outstanding AWs (different dst), each W inherits its own AW's dst");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    // addr 0x340000 → dst=0x34;  addr 0x560000 → dst=0x56.
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x340000)));
    ASSERT_TRUE(pkt.push_aw(make_aw(0x06, 0x560000)));
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

TEST(NmuPacketize, WHeaderLastMatchesWlast) {
    SCENARIO(
        "NMU Packetize: header.last on W flits matches payload.wlast — "
        "intermediate W beats stamp 0, terminal beat stamps 1 "
        "(FlooNoC wormhole packet boundary semantic; "
        "fixes pre-existing bug where every W flit stamped header.last=1)");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    ASSERT_TRUE(pkt.push_aw(make_aw(0x07, 0x340000, /*len*/ 2)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ false)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ false)));
    ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/ true)));

    aw_cap.pop();  // discard AW
    for (int i = 0; i < 3; ++i) {
        auto f = w_cap.pop();
        ASSERT_TRUE(f.has_value());
        uint64_t expected_last = (i == 2) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("last"), expected_last)
            << "W beat " << i << ": header.last expected " << expected_last;
        EXPECT_EQ(f->get_payload_field("W", "wlast"), expected_last);
    }
}

TEST(NmuPacketize, PushAwFailsOnNocFull) {
    SCENARIO(
        "NMU Packetize: push_aw returns false when NoC req channel is full; succeeds after drain");
    ChannelModel noc(/*req*/ 1, /*rsp*/ 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    EXPECT_FALSE(pkt.push_aw(make_aw(1, 0)));
    noc.req_in().pop_flit();
    EXPECT_TRUE(pkt.push_aw(make_aw(1, 0)));
}

TEST(NmuPacketize, AwPayloadBitPerfect) {
    SCENARIO(
        "NMU Packetize: every AW payload field (id/addr/len/size/burst/cache/lock/prot/...) "
        "bit-perfect");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    auto aw = make_aw(/*id*/ 0xAB, /*addr*/ 0x123456789ABCDEF0ull, /*len*/ 0xFF);
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
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0xABu);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x123456789ABCDEF0ull);
    EXPECT_EQ(f.get_payload_field("AW", "awlen"), 0xFFu);
    EXPECT_EQ(f.get_payload_field("AW", "awsize"), 5u);
    EXPECT_EQ(f.get_payload_field("AW", "awburst"), static_cast<uint64_t>(axi::Burst::WRAP));
    EXPECT_EQ(f.get_payload_field("AW", "awcache"), 0xFu);
    EXPECT_EQ(f.get_payload_field("AW", "awlock"), 1u);
    EXPECT_EQ(f.get_payload_field("AW", "awprot"), 0x7u);
    EXPECT_EQ(f.get_payload_field("AW", "awregion"), 0xFu);
    EXPECT_EQ(f.get_payload_field("AW", "awuser"), 0xFFu);
}

TEST(NmuPacketize, AwqosRoundTrip) {
    SCENARIO(
        "NMU Packetize: awqos=0xA set on AwBeat packs into the AW payload field "
        "(AWQOS_LSB=97, AWQOS_WIDTH=4); flit get_payload_field recovers same value");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    auto aw = make_aw(/*id*/ 0x01, /*addr*/ 0x340000);
    aw.qos = 0xA;
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_payload_field("AW", "awqos"), 0xAu);
}

TEST(NmuPacketize, ArqosRoundTrip) {
    SCENARIO(
        "NMU Packetize: arqos=0xA set on ArBeat packs into the AR payload field "
        "(ARQOS_LSB=97, ARQOS_WIDTH=4); flit get_payload_field recovers same value");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    auto ar = make_ar(/*id*/ 0x02, /*addr*/ 0x990000);
    ar.qos = 0xA;
    ASSERT_TRUE(pkt.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_payload_field("AR", "arqos"), 0xAu);
}

TEST(NmuPacketize, WPayloadBitPerfect) {
    SCENARIO(
        "NMU Packetize: W payload (wdata/wstrb/wlast/wuser) round-trips bit-perfect through flit");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    auto w = make_w(0xDEADBEEF, /*last*/ true);
    w.user = 0xAB;
    ASSERT_TRUE(pkt.push_w(w));
    aw_cap.pop();  // discard AW
    auto f = *w_cap.pop();
    EXPECT_EQ(f.get_payload_field("W", "wlast"), 1u);
    EXPECT_EQ(f.get_payload_field("W", "wstrb"), 0xDEADBEEFu);
    EXPECT_EQ(f.get_payload_field("W", "wuser"), 0xABu);
    std::array<uint8_t, 32> wdata_out{};
    f.get_payload_bytes("W", "wdata", wdata_out.data(), 256);
    for (int i = 0; i < 32; ++i) EXPECT_EQ(wdata_out[i], static_cast<uint8_t>(i));
}

TEST(NmuPacketize, ArEncodesAxiChAndRobIdx) {
    SCENARIO(
        "NMU Packetize: AR flit stamps axi_ch=AR, dst from addr_trans, rob_req/rob_idx defaults to "
        "0");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    // addr 0x990000 → dst = (0x990000 >> 16) & 0xFF = 0x99.
    // Frozen interface auto-fills rob_req/rob_idx = 0; Rob-driven path uses
    // push_ar_with_meta (covered by PushAwWithMeta_OverrideDefault).
    ASSERT_TRUE(pkt.push_ar(make_ar(0x07, 0x994000)));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("dst_id"), 0x99u);
    EXPECT_EQ(f.get_header_field("rob_req"), 0u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);
    EXPECT_EQ(f.get_payload_field("AR", "arid"), 0x07u);
    EXPECT_EQ(f.get_payload_field("AR", "araddr"), 0x994000u);
}

TEST(NmuPacketize, RsvdAndDisabledFieldsZero) {
    SCENARIO("NMU Packetize: rsvd/disabled header fields all zero");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, kSrcId);
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    auto f = *aw_cap.pop();
    EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(NmuPacketize, PushAwWithMeta_OverrideDefault) {
    SCENARIO(
        "NMU Packetize: push_aw_with_meta overrides dst_id/local_addr/rob_req/rob_idx from meta");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, /*src=*/0x01);
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x100);  // addr → dst=0 by default
    ni::cmodel::nmu::AwHeaderMeta meta{/*dst_id=*/0x42,
                                       /*local_addr=*/0x9999,
                                       /*rob_req=*/1,
                                       /*rob_idx=*/0x07};
    ASSERT_TRUE(pkt.push_aw_with_meta(b, meta));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x42u);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0x07u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x9999u);  // meta.local_addr, NOT b.addr
}

TEST(NmuPacketize, AddrTransIntegratedDstIdInHeader) {
    SCENARIO(
        "NMU Packetize: frozen push_aw runs addr_trans::xy_route to fill dst_id and local_addr");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, /*src=*/0x01);
    // addr 0x10100 → addr_trans gives dst=1
    axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x10100);
    ASSERT_TRUE(pkt.push_aw(b));  // frozen interface auto-computes
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);            // from addr_trans::xy_route
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x10100u);  // local_addr = addr
}
