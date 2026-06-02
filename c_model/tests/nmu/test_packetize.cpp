#include "nmu/packetize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nmu::Packetize;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x12;

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
  axi::AwBeat b{};
  b.id = id; b.addr = addr; b.len = len; b.size = 5;
  b.burst = axi::Burst::INCR; b.cache = 0xF; b.lock = 0;
  b.prot = 0; b.region = 0; b.user = 0; b.qos = 0;
  return b;
}
axi::WBeat make_w(uint32_t strb, bool last) {
  axi::WBeat b{};
  for (int i = 0; i < 32; ++i) b.data[i] = static_cast<uint8_t>(i);
  b.strb = strb; b.last = last; b.user = 0;
  return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
  axi::ArBeat b{};
  b.id = id; b.addr = addr; b.len = 0; b.size = 5;
  b.burst = axi::Burst::INCR; b.cache = 0; b.lock = 0;
  b.prot = 0; b.region = 0; b.user = 0; b.qos = 0;
  return b;
}
}

TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields) {
  LoopbackNoc noc(/*req*/16, /*rsp*/16);
  Packetize pkt(noc.req_out(), kSrcId);
  // Legacy test: only verifies packetize stamps src + axi_ch + last + awid +
  // awaddr. dst_id derivation is covered by WMetaFifoInheritsAwDst below.
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0xDEADBEEFCAFEBABEull)));

  auto flit_opt = noc.req_in().pop_flit();
  ASSERT_TRUE(flit_opt.has_value());
  const auto& f = *flit_opt;
  EXPECT_EQ(f.get_header_field("axi_ch"),   ni::AXI_CH_AW);
  EXPECT_EQ(f.get_header_field("src_id"),   kSrcId);
  EXPECT_EQ(f.get_header_field("vc_id"),    0u);
  EXPECT_EQ(f.get_header_field("last"),     1u);
  EXPECT_EQ(f.get_payload_field("AW", "awid"),   0x05u);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xDEADBEEFCAFEBABEull);
}

TEST(NmuPacketize, WMetaFifoInheritsAwDst) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  // addr 0x340000 → dst = (0x340000 >> 16) & 0xFF = 0x34
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x340000)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/true)));

  noc.req_in().pop_flit();  // discard AW
  auto w_flit_opt = noc.req_in().pop_flit();
  ASSERT_TRUE(w_flit_opt.has_value());
  EXPECT_EQ(w_flit_opt->get_header_field("dst_id"), 0x34u);
  EXPECT_EQ(w_flit_opt->get_header_field("axi_ch"), ni::AXI_CH_W);
}

TEST(NmuPacketize, MultiOutstandingAwInterleavedW) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  // addr 0x340000 → dst=0x34;  addr 0x560000 → dst=0x56.
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x340000)));
  ASSERT_TRUE(pkt.push_aw(make_aw(0x06, 0x560000)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/true)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/true)));

  ASSERT_EQ(noc.req_q_size(), 4u);
  noc.req_in().pop_flit();  // AW1
  noc.req_in().pop_flit();  // AW2
  auto w1 = noc.req_in().pop_flit();
  auto w2 = noc.req_in().pop_flit();
  EXPECT_EQ(w1->get_header_field("dst_id"), 0x34u);
  EXPECT_EQ(w2->get_header_field("dst_id"), 0x56u);
}

TEST(NmuPacketize, PushAwFailsOnNocFull) {
  LoopbackNoc noc(/*req*/1, /*rsp*/16);
  Packetize pkt(noc.req_out(), kSrcId);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
  EXPECT_FALSE(pkt.push_aw(make_aw(1, 0)));
  noc.req_in().pop_flit();
  EXPECT_TRUE(pkt.push_aw(make_aw(1, 0)));
}

TEST(NmuPacketize, AwPayloadBitPerfect) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  auto aw = make_aw(/*id*/0xAB, /*addr*/0x123456789ABCDEF0ull, /*len*/0xFF);
  aw.size = 5; aw.burst = axi::Burst::WRAP; aw.cache = 0xF; aw.lock = 1;
  aw.prot = 0x7; aw.region = 0xF; aw.user = 0xFF; aw.qos = 0xF;
  ASSERT_TRUE(pkt.push_aw(aw));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_payload_field("AW", "awid"),     0xABu);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"),   0x123456789ABCDEF0ull);
  EXPECT_EQ(f.get_payload_field("AW", "awlen"),    0xFFu);
  EXPECT_EQ(f.get_payload_field("AW", "awsize"),   5u);
  EXPECT_EQ(f.get_payload_field("AW", "awburst"), static_cast<uint64_t>(axi::Burst::WRAP));
  EXPECT_EQ(f.get_payload_field("AW", "awcache"),  0xFu);
  EXPECT_EQ(f.get_payload_field("AW", "awlock"),   1u);
  EXPECT_EQ(f.get_payload_field("AW", "awprot"),   0x7u);
  EXPECT_EQ(f.get_payload_field("AW", "awregion"), 0xFu);
  EXPECT_EQ(f.get_payload_field("AW", "awuser"),   0xFFu);
}

TEST(NmuPacketize, WPayloadBitPerfect) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
  auto w = make_w(0xDEADBEEF, /*last*/true);
  w.user = 0xAB;
  ASSERT_TRUE(pkt.push_w(w));
  noc.req_in().pop_flit();  // discard AW
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_payload_field("W", "wlast"),  1u);
  EXPECT_EQ(f.get_payload_field("W", "wstrb"),  0xDEADBEEFu);
  EXPECT_EQ(f.get_payload_field("W", "wuser"),  0xABu);
  std::array<uint8_t, 32> wdata_out{};
  f.get_payload_bytes("W", "wdata", wdata_out.data(), 256);
  for (int i = 0; i < 32; ++i) EXPECT_EQ(wdata_out[i], static_cast<uint8_t>(i));
}

TEST(NmuPacketize, ArEncodesAxiChAndRobIdx) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  // addr 0x990000 → dst = (0x990000 >> 16) & 0xFF = 0x99.
  // Frozen interface auto-fills rob_req/rob_idx = 0; Rob-driven path uses
  // push_ar_with_meta (covered by PushAwWithMeta_OverrideDefault).
  ASSERT_TRUE(pkt.push_ar(make_ar(0x07, 0x994000)));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_header_field("axi_ch"),   ni::AXI_CH_AR);
  EXPECT_EQ(f.get_header_field("dst_id"),   0x99u);
  EXPECT_EQ(f.get_header_field("rob_req"),  0u);
  EXPECT_EQ(f.get_header_field("rob_idx"),  0u);
  EXPECT_EQ(f.get_payload_field("AR", "arid"),   0x07u);
  EXPECT_EQ(f.get_payload_field("AR", "araddr"), 0x994000u);
}

TEST(NmuPacketize, RsvdAndDisabledFieldsZero) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_header_field("commtype"),  0u);
  EXPECT_EQ(f.get_header_field("multicast"), 0u);
  EXPECT_EQ(f.get_header_field("noc_qos"),   0u);
  EXPECT_EQ(f.get_header_field("route_par"), 0u);
  EXPECT_EQ(f.get_header_field("flit_ecc"),  0u);
  EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(NmuPacketize, PushAwWithMeta_OverrideDefault) {
  LoopbackNoc noc(/*req*/16, /*rsp*/16);
  Packetize pkt(noc.req_out(), /*src=*/0x01);
  axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x100);  // addr → dst=0 by default
  ni::cmodel::nmu::AwHeaderMeta meta{
      /*dst_id=*/0x42,
      /*local_addr=*/0x9999,
      /*rob_req=*/1,
      /*rob_idx=*/0x07
  };
  ASSERT_TRUE(pkt.push_aw_with_meta(b, meta));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_header_field("dst_id"),  0x42u);
  EXPECT_EQ(f.get_header_field("rob_req"), 1u);
  EXPECT_EQ(f.get_header_field("rob_idx"), 0x07u);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x9999u);  // meta.local_addr, NOT b.addr
}

TEST(NmuPacketize, AddrTransIntegratedDstIdInHeader) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), /*src=*/0x01);
  // addr 0x10100 → addr_trans gives dst=1
  axi::AwBeat b = make_aw(/*id=*/0x05, /*addr=*/0x10100);
  ASSERT_TRUE(pkt.push_aw(b));  // frozen interface auto-computes
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);  // from addr_trans::xy_route
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0x10100u);  // local_addr = addr
}
