#include "nmu/packetize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <array>
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
    SCENARIO(
        "NMU Packetize: push_aw stamps src_id/axi_ch=AW/vc=0/flit_tail=0/awid/awaddr on emitted "
        "flit "
        "(AW starts wormhole packet)");
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

TEST(NmuPacketize, WMetaFifoInheritsAwDst) {
    SCENARIO("NMU Packetize: W flit inherits dst_id from preceding AW via W-meta FIFO");
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
    SCENARIO("NMU Packetize: 2 outstanding AWs (different dst), each W inherits its own AW's dst");
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

TEST(NmuPacketize, WHeaderFlitTailMatchesWlast) {
    SCENARIO(
        "NMU Packetize: header.flit_tail on W flits matches payload.wlast — "
        "intermediate W beats stamp 0, terminal beat stamps 1 "
        "(FlooNoC wormhole packet boundary semantic; "
        "fixes pre-existing bug where every W flit stamped header.flit_tail=1)");
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
    SCENARIO(
        "NMU Packetize: push_aw returns false when NoC req channel is full; succeeds after drain");
    ChannelModel noc(/*req*/ 1, /*rsp*/ 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, legacy_sam());
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
    SCENARIO(
        "NMU Packetize: AWUSER[57:8] (collective_op/collective_mask) is stripped at packetize "
        "and never reaches the AW payload; zero collective bits pass through unaffected, only "
        "AWUSER[7:0] lands in the payload's awuser field");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());

    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = 0x5Au;  // no collective bits set
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_payload_field("AW", "awuser"), 0x5Au);  // AWUSER[57:8] never reaches payload
}

TEST(NmuPacketizeDeath, NonzeroCollectiveOpAborts) {
    SCENARIO(
        "NMU Packetize: the direct push_aw interface bypasses nmu::Rob, which owns the collective "
        "validate and translate, so a collective AWUSER here would be silently truncated to the "
        "8 b payload field. Permanent illegal input, not backpressure — fails loud (assert+abort) "
        "instead of return-false, which can't wedge the S1 stage as congestion");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = (uint64_t{1} << 8) | 0x5Au;  // collective_op=1, user-defined=0x5A
    EXPECT_DEATH(pkt.push_aw(aw), "collective_op/collective_mask");
}

TEST(NmuPacketizeDeath, NonzeroCollectiveMaskAborts) {
    SCENARIO(
        "NMU Packetize: nonzero AWUSER collective_mask alone (collective_op=0) also aborts on the "
        "direct path — the reject checks AWUSER[57:8] as a whole, not collective_op in isolation");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto aw = make_aw(/*id*/ 0x03, /*addr*/ 0x340000);
    aw.user = (uint64_t{0xFFFFFFFFFFFFull} << 10) | 0x5Au;  // nonzero collective_mask only
    EXPECT_DEATH(pkt.push_aw(aw), "collective_op/collective_mask");
}

TEST(NmuPacketize, AwqosRoundTrip) {
    SCENARIO(
        "NMU Packetize: awqos=0xA set on AwBeat packs into the AW payload field "
        "(AWQOS_LSB=81, AWQOS_WIDTH=4); flit get_payload_field recovers same value");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
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
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    auto ar = make_ar(/*id*/ 0x02, /*addr*/ 0x990000);
    ar.qos = 0xA;
    ASSERT_TRUE(pkt.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_payload_field("AR", "arqos"), 0xAu);
}

TEST(NmuPacketize, WPayloadBitPerfect) {
    SCENARIO(
        "NMU Packetize: W payload (wdata/wstrb/wlast/wuser) round-trips bit-perfect through flit "
        "(legacy_sam() is memory space -> data class -> DATA_W)");
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
    SCENARIO(
        "NMU Packetize: narrow class push_w extracts the addressed 8B lane from a genuinely "
        "unaligned local_addr (not a multiple of the beat size) -- site 1 of the S2 design doc's "
        "lane re-anchor table, bypassing AxiMaster's own aligned-down AW/first-beat-mask "
        "machinery entirely (push_aw_with_meta takes local_addr directly)");
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
    SCENARIO(
        "NMU Packetize: AR flit stamps axi_ch=AR, dst from addr_trans, ordering_req/ordering_tag "
        "defaults to "
        "0");
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
    SCENARIO("NMU Packetize: rsvd/disabled header fields all zero");
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, legacy_sam());
    ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
    auto f = *aw_cap.pop();
    EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(NmuPacketize, PushAwWithMeta_OverrideDefault) {
    SCENARIO(
        "NMU Packetize: push_aw_with_meta overrides dst_id/local_addr/ordering_req/ordering_tag "
        "from meta");
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
    SCENARIO(
        "NMU Packetize: direct-path push_aw runs SamTable::translate (16x16 uniform) to fill "
        "dst_id "
        "(from the table) while forwarding the address unchanged");
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
    SCENARIO(
        "NMU Packetize: push_aw runs SamTable::translate; dst_id comes from the table and "
        "awaddr is the request address, unchanged");
    ReqCapture aw_cap, w_cap, ar_cap;
    // Single packed tile at (2,1) -> dst_id 0x12, base 0 (only entry in the list).
    auto sam = addr_trans::SamTable::packed({{2, 1, 0x100000000ull}});
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, /*src_id=*/0, sam);
    axi::AwBeat aw{};
    aw.addr = 0x40ull;
    aw.id = 5;
    aw.len = 0;
    aw.size = 3;
    aw.burst = axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("dst_id"), 0x12u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x40ull);
}
