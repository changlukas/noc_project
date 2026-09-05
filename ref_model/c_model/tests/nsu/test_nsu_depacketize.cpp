#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/channel_model.hpp"
#include "axi/types.hpp"
#include "ni/channel_mode.hpp"
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
                              uint8_t axi_ch = ni::AXI_CH_NarrowAw, uint8_t vc = 0) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", ordering_req);
    f.set_header_field("ordering_tag", ordering_tag);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", addr);
    f.set_payload_field("AW", "awsize", 5);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}
ni::cmodel::Flit make_w_flit(uint32_t strb, bool last, uint8_t axi_ch = ni::AXI_CH_NarrowW,
                             uint8_t vc = 0) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("vc_id", vc);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    auto flit = make_aw_flit(0x05, 0x1000, /*src*/ 0x12, /*ordering_req*/ 0, /*ordering_tag*/ 0);
    flit.set_payload_field("AW", "awqos", 0xA);
    ASSERT_TRUE(noc.req_out().push_flit(flit));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->qos, 0xAu);
}

TEST(NsuDepacketize, ArqosRecoveredFromFlit) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    auto flit = make_ar_flit(0x07, 0x2000, /*src*/ 0x12);
    flit.set_payload_field("AR", "arqos", 0xA);
    ASSERT_TRUE(noc.req_out().push_flit(flit));
    depkt.tick();
    auto ar = depkt.pop_ar();
    ASSERT_TRUE(ar.has_value());
    EXPECT_EQ(ar->qos, 0xAu);
}

TEST(NsuDepacketize, ArFlitSnapshotsReadMeta) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    auto f = make_ar_flit(0x07, 0x2000, 0x12);
    f.set_header_field("src_port_id", 1);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    depkt.tick();
    EXPECT_TRUE(depkt.pop_ar().has_value());
    EXPECT_TRUE(mb.peek_read(0x07).has_value());
    EXPECT_EQ(mb.peek_read(0x07)->src_id, 0x12);
    EXPECT_EQ(mb.peek_read(0x07)->src_port, 1u);
}

TEST(NsuDepacketize, RecordsTheRequestersPortInTheMetaEntry) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    auto f = make_aw_flit(0x05, 0x1000, /*src_id=*/0x12);
    f.set_header_field("src_port_id", 1);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_EQ(mb.peek_write(0x05)->src_port, 1u);
}

// This NSU sits on port 1, so a request naming port 1 is its own. Twin of the
// death test below: same drive, only the flit's dst_port_id differs.
TEST(NsuDepacketize, AcceptsARequestAddressedToItsOwnPort) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // port_id is the seventh constructor argument, after space_coords.
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE,
                      ni::cmodel::router::null_req_in(), /*src_id=*/0x02, /*space_coords=*/{},
                      /*port_id=*/1);
    auto f = make_aw_flit(0x05, 0x1000, /*src_id=*/0x12);
    f.set_header_field("dst_port_id", 1);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    depkt.tick();
    EXPECT_TRUE(depkt.pop_aw().has_value());
}

// Fault injection: a request naming a port that is not this NSU's must abort.
// The NSU is on port 1 and the flit names port 0, so a port_id_ hardwired to 0
// cannot satisfy this.
TEST(NsuDepacketizeDeath, RejectsARequestAddressedToAnotherPort) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE,
                      ni::cmodel::router::null_req_in(), /*src_id=*/0x02, /*space_coords=*/{},
                      /*port_id=*/1);
    auto f = make_aw_flit(0x05, 0x1000, /*src_id=*/0x12);
    f.set_header_field("dst_port_id", 0);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    EXPECT_DEATH(depkt.tick(), "dst_port_id");
}

TEST(NsuDepacketize, WFlitNoMetaSideEffect) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    // The AW ahead of it only satisfies the W-follows-AW order; it allocates
    // under its own id (0x05, identity remap at max_unique_ids NOC_ID_SPACE), so key 0
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);

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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);

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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x01, 0x0)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFF, true)));
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x02, 0x1000)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_aw()->id, 0x01);
    EXPECT_EQ(depkt.pop_w()->strb, 0xFFu);
    EXPECT_EQ(depkt.pop_ar()->id, 0x02);
}

// Two data-class worms on different VCs arrive flit-interleaved, which is
// what the per-(output, VC) router lock produces. Each VC reassembles its
// own burst; the AXI side sees A's beats, then B's, never mixed.
TEST(NsuDepacketize, InterleavedDataWormsReassemblePerVc) {
    ChannelModel noc(16, 16);
    ChannelModel dat_noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE, dat_noc.req_in(), 0, {}, 0,
                      /*dat_num_vc=*/2);
    auto aw_a = make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw, /*vc=*/0);
    auto aw_b = make_aw_flit(0x02, 0x0, 0x11, 0, 0, ni::AXI_CH_DataAw, /*vc=*/1);
    aw_a.set_payload_field("AW", "awlen", 1);
    aw_b.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(dat_noc.req_out().push_flit(aw_a));
    ASSERT_TRUE(dat_noc.req_out().push_flit(aw_b));
    ASSERT_TRUE(dat_noc.req_out().push_flit(make_w_flit(0xA0, false, ni::AXI_CH_DataW, 0)));
    ASSERT_TRUE(dat_noc.req_out().push_flit(make_w_flit(0xB0, false, ni::AXI_CH_DataW, 1)));
    ASSERT_TRUE(dat_noc.req_out().push_flit(make_w_flit(0xA1, true, ni::AXI_CH_DataW, 0)));
    ASSERT_TRUE(dat_noc.req_out().push_flit(make_w_flit(0xB1, true, ni::AXI_CH_DataW, 1)));
    for (int i = 0; i < 6; ++i) depkt.tick();
    auto a = depkt.pop_aw();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, 0x01);
    EXPECT_EQ(depkt.pop_w()->strb, 0xA0u);
    EXPECT_EQ(depkt.pop_w()->strb, 0xA1u);
    auto b = depkt.pop_aw();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x02);
    EXPECT_EQ(depkt.pop_w()->strb, 0xB0u);
    EXPECT_EQ(depkt.pop_w()->strb, 0xB1u);
    EXPECT_FALSE(depkt.pop_w().has_value());
}

// Credit is the VC queue slot, returned when the flit leaves the queue
// (pop_aw / pop_w), not when it arrives. One pulse per consumed flit.
TEST(NsuDepacketize, DatCreditPulsesOnConsumption) {
    ChannelModel noc(16, 16);
    ChannelModel dat_noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE, dat_noc.req_in(), 0, {}, 0,
                      /*dat_num_vc=*/2);
    ASSERT_TRUE(
        dat_noc.req_out().push_flit(make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw, 1)));
    ASSERT_TRUE(dat_noc.req_out().push_flit(make_w_flit(0xFF, true, ni::AXI_CH_DataW, 1)));
    depkt.tick();
    depkt.tick();
    EXPECT_FALSE(depkt.take_dat_credit(1));  // arrived, not consumed
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_TRUE(depkt.take_dat_credit(1));
    EXPECT_FALSE(depkt.take_dat_credit(1));
    ASSERT_TRUE(depkt.pop_w().has_value());
    EXPECT_TRUE(depkt.take_dat_credit(1));
    EXPECT_FALSE(depkt.take_dat_credit(0));
}

namespace {

void expect_normalized_data_w_reanchor(::ni::cmodel::ni::ChannelMode mode,
                                       bool use_dat_ingress) {
    ChannelModel noc(16, 16);
    ChannelModel dat_noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE, dat_noc.req_in(), 0, {},
                      0, /*dat_num_vc=*/1);
    depkt.set_channel_mode(mode);
    auto& ingress = use_dat_ingress ? dat_noc.req_out() : noc.req_out();

    auto aw = make_aw_flit(0x01, 0x1000, 0x10, 0, 0, ::ni::AXI_CH_DataAw);
    aw.set_payload_field("AW", "awlen", 1);
    aw.set_payload_field("AW", "awsize", 3);
    ASSERT_TRUE(ingress.push_flit(aw));
    for (uint8_t beat = 0; beat < 2; ++beat) {
        auto w = make_w_flit(0, beat == 1, ::ni::AXI_CH_DataW);
        w.set_payload_field("NARROW_W", "wlast", beat == 1 ? 1u : 0u);
        w.set_payload_field("NARROW_W", "wstrb", 0xFF);
        std::array<uint8_t, axi::NARROW_DATA_BYTES> lane{};
        for (std::size_t i = 0; i < lane.size(); ++i)
            lane[i] = static_cast<uint8_t>((beat == 0 ? 0xD0 : 0xE0) + i);
        w.set_payload_bytes("NARROW_W", "wdata", lane.data(),
                            ::ni::width::NOC_NARROW_DATA_WIDTH);
        ASSERT_TRUE(ingress.push_flit(w));
    }
    for (int i = 0; i < 3; ++i) depkt.tick();

    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_EQ(depkt.take_dat_credit(0), use_dat_ingress);
    for (uint8_t beat = 0; beat < 2; ++beat) {
        auto out = depkt.pop_w();
        ASSERT_TRUE(out.has_value());
        const std::size_t offset = beat * axi::NARROW_DATA_BYTES;
        EXPECT_EQ(out->strb, 0xFFull << offset);
        for (std::size_t i = 0; i < out->data.size(); ++i) {
            const uint8_t expected = i >= offset && i < offset + axi::NARROW_DATA_BYTES
                                         ? static_cast<uint8_t>((beat == 0 ? 0xD0 : 0xE0) + i - offset)
                                         : 0;
            EXPECT_EQ(out->data[i], expected) << "beat=" << unsigned(beat) << " byte=" << i;
        }
        EXPECT_EQ(depkt.take_dat_credit(0), use_dat_ingress);
    }
    EXPECT_FALSE(depkt.take_dat_credit(0));
}

}  // namespace

TEST(NsuDepacketize, TwoChannel64ReanchorsConsecutiveDataWBeatsToAddressedAxiLanes) {
    expect_normalized_data_w_reanchor(::ni::cmodel::ni::ChannelMode::TwoChannel64,
                                      /*use_dat_ingress=*/false);
}

TEST(NsuDepacketize, TwoChannel64ReqIngressWaitsWhenDataQueueIsFull) {
    ChannelModel noc(32, 32);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE);
    depkt.set_channel_mode(::ni::cmodel::ni::ChannelMode::TwoChannel64);

    auto aw = make_aw_flit(0x01, 0x1000, 0x10, 0, 0, ::ni::AXI_CH_DataAw);
    aw.set_payload_field("AW", "awlen", ::ni::NOC_NI_DAT_RX_VC_DEPTH - 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw));
    for (std::size_t beat = 0; beat < ::ni::NOC_NI_DAT_RX_VC_DEPTH; ++beat) {
        ASSERT_TRUE(noc.req_out().push_flit(
            make_w_flit(static_cast<uint32_t>(beat),
                        beat + 1 == ::ni::NOC_NI_DAT_RX_VC_DEPTH, ::ni::AXI_CH_DataW)));
    }

    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    ASSERT_TRUE(depkt.pop_w().has_value());
    depkt.tick();
    for (std::size_t beat = 1; beat < ::ni::NOC_NI_DAT_RX_VC_DEPTH; ++beat) {
        auto w = depkt.pop_w();
        ASSERT_TRUE(w.has_value()) << "beat=" << beat;
        EXPECT_EQ(w->last, beat + 1 == ::ni::NOC_NI_DAT_RX_VC_DEPTH);
    }
}

TEST(NsuDepacketize, ThreeChannel64ReanchorsConsecutiveDataWBeatsToAddressedAxiLanes) {
    expect_normalized_data_w_reanchor(::ni::cmodel::ni::ChannelMode::ThreeChannel64,
                                      /*use_dat_ingress=*/true);
}

// A second worm on the SAME VC queues behind the first worm's beats: a VC's
// W stream is one worm at a time. Same order semantics as the deleted
// PendingHolBlockingS1WFullBlocksAwBehind, without the ingress stash.
TEST(NsuDepacketize, SecondWormOnSameVcWaitsBehindFirstWormsBeats) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE);
    auto aw_owner = make_aw_flit(0x06, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw);
    aw_owner.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw_owner));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAA, false, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xBB, true, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x07, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    for (int i = 0; i < 4; ++i) depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_FALSE(depkt.pop_aw().has_value());  // vc0 front is W, not AW
    EXPECT_EQ(depkt.pop_w()->strb, 0xAAu);
    EXPECT_EQ(depkt.pop_w()->strb, 0xBBu);
    EXPECT_TRUE(depkt.pop_aw().has_value());
}

// Fault injection: a VC's W stream interrupted by another head means the
// fabric broke per-VC contiguity. Fail loud, never mis-pair.
TEST(NsuDepacketizeDeath, WStreamInterruptedOnItsVcAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, axi::NOC_ID_SPACE);
    auto aw_a = make_aw_flit(0x01, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw);
    aw_a.set_payload_field("AW", "awlen", 1);
    ASSERT_TRUE(noc.req_out().push_flit(aw_a));
    ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xA0, false, ni::AXI_CH_DataW)));
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x02, 0x0, 0x10, 0, 0, ni::AXI_CH_DataAw)));
    for (int i = 0; i < 3; ++i) depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    ASSERT_TRUE(depkt.pop_w().has_value());
    EXPECT_DEATH(depkt.pop_w(), "contiguity");
}

// NsuDepacketize::PopBAssertFalse was a runtime wrong_side_() test.
// The method no longer exists on nsu::Depacketize; wrong-side
// calls are now caught at compile time. Test removed.

// Tick-cardinality: S1 register holds <=1 AW per tick.
// For 3 sequential AW flits on the same channel, tick 3 times — one flit
// per tick — to drain all 3. FIFO order coverage is unchanged.
TEST(NsuDepacketize, FifoOrderPreservedAcrossChannels) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE);
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // FlooNoC provides only collapse-or-passthrough; an intermediate N is unsupported.
    EXPECT_THROW(Depacketize(noc.req_in(), mb, /*max_unique_ids*/ 5), std::invalid_argument);
    EXPECT_THROW(Depacketize(noc.req_in(), mb, /*max_unique_ids*/ 0), std::invalid_argument);
    // Both legal endpoints construct without throwing.
    EXPECT_NO_THROW(Depacketize(noc.req_in(), mb, /*collapse*/ 1));
    EXPECT_NO_THROW(Depacketize(noc.req_in(), mb, axi::NOC_ID_SPACE));
}

// --- Node-coordinate rebase (Stage 2b) -------------------------------------
// A collective replica arrives carrying the REQUEST's own address, because one
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
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // src_id 0x21 = (y=2 << X_WIDTH) | x=1.
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE,
                      ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21, coords_for_narrow());
    // The request address names node (0,0); the offset inside the tile is 0x3c0.
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x0003c0)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    // x=1 at bit 20, y=2 at bit 22 -> 0x9003c0.
    EXPECT_EQ(aw->addr, 0x9003c0u);
}

TEST(NsuDepacketize, RebaseIsTheIdentityForAUnicastAlreadyNamingThisNode) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE,
                      ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21, coords_for_narrow());
    ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x07, 0x9003c0)));
    depkt.tick();
    auto ar = depkt.pop_ar();
    ASSERT_TRUE(ar.has_value());
    EXPECT_EQ(ar->addr, 0x9003c0u);
}

TEST(NsuDepacketize, UndeclaredCoordsLeaveTheAddressAlone) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::NOC_ID_SPACE,
                      ni::cmodel::router::null_req_in(),
                      /*src_id*/ 0x21);
    ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x0003c0)));
    depkt.tick();
    auto aw = depkt.pop_aw();
    ASSERT_TRUE(aw.has_value());
    EXPECT_EQ(aw->addr, 0x0003c0u);
}
