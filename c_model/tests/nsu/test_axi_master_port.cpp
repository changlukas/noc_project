// Unit tests for nsu/axi_master_port.hpp — the NSU downstream-facing AXI4
// manager transparent transport port, peer of nmu/axi_slave_port.hpp.
//
// Test strategy mirror of the NMU side:
//   - LoopbackChannelSet stands in for the future NoC fabric. The port
//     pulls AW/W/AR from the depacketizer (NoC -> AXI request) and pushes
//     B/R to the packetizer (AXI -> NoC response).
//   - The "downstream" face is what an external AxiSlave would talk to;
//     the test drives that face with pop_aw / pop_w / pop_ar / push_b /
//     push_r directly.
//   - PortParams come from c_model/config/port_params.yaml.
//   - Port contract: per-channel FIFO order for all beats regardless of
//     AXI ID. Cross-ID completion ordering / per-ID response reordering
//     is the ROB stage's responsibility (see plan §3.1), NOT this port's.
#include "common/loopback_depacketizer.hpp"
#include "common/loopback_packetizer.hpp"
#include "common/scenario.hpp"
#include "ni/port_params.hpp"
#include "nsu/axi_master_port.hpp"
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace cmod = ni::cmodel;
namespace nsu = ni::cmodel::nsu;
namespace axi = ni::cmodel::axi;
namespace test = ni::cmodel::testing;

namespace {

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0,
                    uint8_t size = 5, axi::Burst burst = axi::Burst::INCR,
                    uint8_t cache = 0xA, uint8_t lock = 0,
                    uint8_t prot = 0x3, uint8_t region = 0xF,
                    uint8_t user = 0x55, uint8_t qos = 0xC) {
  return axi::AwBeat{id, addr, len, size, burst, cache, lock, prot,
                      region, user, qos};
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr, uint8_t len = 0,
                    uint8_t size = 5, axi::Burst burst = axi::Burst::INCR,
                    uint8_t cache = 0x5, uint8_t lock = 0,
                    uint8_t prot = 0x2, uint8_t region = 0xE,
                    uint8_t user = 0xAA, uint8_t qos = 0x9) {
  return axi::ArBeat{id, addr, len, size, burst, cache, lock, prot,
                      region, user, qos};
}
axi::WBeat make_w(uint8_t fill, uint32_t strb, bool last, uint8_t user) {
  axi::WBeat w{};
  w.data.fill(fill); w.strb = strb; w.last = last; w.user = user;
  return w;
}
axi::BBeat make_b(uint8_t id, axi::Resp resp, uint8_t user) {
  return axi::BBeat{id, resp, user};
}
axi::RBeat make_r(uint8_t id, uint8_t fill, axi::Resp resp, bool last,
                  uint8_t user) {
  axi::RBeat r{}; r.id = id; r.data.fill(fill); r.resp = resp;
  r.last = last; r.user = user; return r;
}

struct PortFixture {
  test::LoopbackChannelSet ch{};
  test::LoopbackPacketizer pkt{ch};
  test::LoopbackDepacketizer depkt{ch};
  cmod::PortParams params;
  nsu::AxiMasterPort port;

  PortFixture()
      : params(cmod::load_port_params_yaml("config/port_params.yaml", "nsu")),
        port(depkt, pkt, params) {}

  void set_loopback_caps(std::size_t aw, std::size_t w, std::size_t ar,
                          std::size_t b, std::size_t r) {
    ch.aw_capacity = aw; ch.w_capacity  = w; ch.ar_capacity = ar;
    ch.b_capacity  = b;  ch.r_capacity  = r;
  }
};

}  // namespace

// -------------------------------------------------------------------------
// 1. Basic per-channel handshake: NoC delivers N AW beats, port surfaces N.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, AwBasicHandshake_PushPopNoLoss) {
  SCENARIO("NSU AxiMasterPort: 8 AW beats arrive from depacketizer, all surface via pop_aw in order");
  PortFixture fx;
  for (uint8_t i = 0; i < 8; ++i) fx.ch.aw.push_back(make_aw(i, 0x1000 + i*32));
  fx.port.tick();
  for (uint8_t i = 0; i < 8; ++i) {
    auto out = fx.port.pop_aw();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, i);
  }
}

TEST(NsuAxiMasterPort, ArBasicHandshake_PushPopNoLoss) {
  SCENARIO("NSU AxiMasterPort: 8 AR beats arrive from depacketizer, all surface via pop_ar in order");
  PortFixture fx;
  for (uint8_t i = 0; i < 8; ++i) fx.ch.ar.push_back(make_ar(i, 0x2000 + i*32));
  fx.port.tick();
  for (uint8_t i = 0; i < 8; ++i) {
    auto out = fx.port.pop_ar();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, i);
  }
}

TEST(NsuAxiMasterPort, WBasicHandshake_PushPopNoLoss) {
  SCENARIO("NSU AxiMasterPort: 8 W beats arrive from depacketizer, all surface via pop_w with payload");
  PortFixture fx;
  for (uint8_t i = 0; i < 8; ++i)
    fx.ch.w.push_back(make_w(0x10 + i, 0xFFFFFFFFu, i == 7, i));
  fx.port.tick();
  for (uint8_t i = 0; i < 8; ++i) {
    auto out = fx.port.pop_w();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->data[0], 0x10 + i);
  }
}

// -------------------------------------------------------------------------
// 2. Per-channel backpressure: B internal queue fills, push_b returns false;
//    draining via packetizer frees room.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, BBackpressure_FullThenDrainThenAcceptOne) {
  SCENARIO("NSU AxiMasterPort: B queue full rejects push; drain one to packetizer reopens slot");
  PortFixture fx;
  fx.set_loopback_caps(32, 32, 32, /*b*/ 0, /*r*/ 32);
  for (std::size_t i = 0; i < fx.params.b_queue_depth; ++i)
    ASSERT_TRUE(fx.port.push_b(make_b(static_cast<uint8_t>(i & 0xFF),
                                        axi::Resp::OKAY, 0)));
  EXPECT_FALSE(fx.port.push_b(make_b(0xFF, axi::Resp::OKAY, 0)));
  fx.ch.b_capacity = 1;
  fx.port.tick();
  EXPECT_TRUE(fx.port.push_b(make_b(0xFF, axi::Resp::OKAY, 0)));
}

// -------------------------------------------------------------------------
// 3. Queue boundary: failed push_b retried 3 times must not duplicate.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, BBoundary_FailedPushDoesNotDuplicateOnRetry) {
  SCENARIO("NSU AxiMasterPort: failed push_b retries do not silently land; retry counts as one");
  PortFixture fx;
  fx.set_loopback_caps(32, 32, 32, 0, 32);
  for (std::size_t i = 0; i < fx.params.b_queue_depth; ++i)
    ASSERT_TRUE(fx.port.push_b(make_b(static_cast<uint8_t>(i & 0xFF),
                                        axi::Resp::OKAY, 0)));
  axi::BBeat retry = make_b(0xCD, axi::Resp::SLVERR, 0xAB);
  EXPECT_FALSE(fx.port.push_b(retry));
  EXPECT_FALSE(fx.port.push_b(retry));
  EXPECT_FALSE(fx.port.push_b(retry));
  EXPECT_EQ(fx.port.b_q_size(), fx.params.b_queue_depth);
  fx.ch.b_capacity = 1;
  fx.port.tick();
  EXPECT_TRUE(fx.port.push_b(retry));
  fx.ch.b_capacity = 64;
  fx.port.tick();
  std::size_t retry_id_count = 0;
  for (const auto& b : fx.ch.b) if (b.id == 0xCD) ++retry_id_count;
  EXPECT_EQ(retry_id_count, 1u);
}

// -------------------------------------------------------------------------
// 4. AW channel FIFO order preserved end-to-end (mixed ids).
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, AwFifoOrder_PreservedAcrossMixedIds) {
  SCENARIO("NSU AxiMasterPort: AW channel FIFO order preserved across mixed AXI ids (no per-id reorder)");
  PortFixture fx;
  // Port contract: per-channel FIFO order for all beats regardless of AXI ID.
  // Cross-ID completion ordering / per-ID response reordering is the ROB
  // stage's responsibility (see plan §3.1), NOT this port's.
  const std::vector<uint8_t> ids{4, 7, 4, 1, 7, 2, 1, 4};
  for (auto id : ids) fx.ch.aw.push_back(make_aw(id, 0x1000));
  fx.port.tick();
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto out = fx.port.pop_aw();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, ids[i]);
  }
}

// -------------------------------------------------------------------------
// 5. AR channel FIFO order preserved end-to-end (mixed ids).
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, ArFifoOrder_PreservedAcrossMixedIds) {
  SCENARIO("NSU AxiMasterPort: AR channel FIFO order preserved across mixed AXI ids (no per-id reorder)");
  PortFixture fx;
  // Port contract: per-channel FIFO order for all beats regardless of AXI ID.
  // Cross-ID completion ordering / per-ID response reordering is the ROB
  // stage's responsibility (see plan §3.1), NOT this port's.
  const std::vector<uint8_t> ids{6, 8, 6, 3, 8, 0, 3, 6};
  for (auto id : ids) fx.ch.ar.push_back(make_ar(id, 0x2000));
  fx.port.tick();
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto out = fx.port.pop_ar();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, ids[i]);
  }
}

// -------------------------------------------------------------------------
// 6. W beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, WPassthroughBitForBit) {
  SCENARIO("NSU AxiMasterPort: W beat passes through with data/strb/last/user bit-for-bit preserved");
  PortFixture fx;
  axi::WBeat in{};
  for (std::size_t i = 0; i < axi::DATA_BYTES; ++i)
    in.data[i] = static_cast<uint8_t>(0xC0 + i);
  in.strb = 0xDEAD'BEEFu;
  in.last = true;
  in.user = 0x77;
  fx.ch.w.push_back(in);
  fx.port.tick();
  auto out = fx.port.pop_w();
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->data, in.data);
  EXPECT_EQ(out->strb, in.strb);
  EXPECT_EQ(out->last, in.last);
  EXPECT_EQ(out->user, in.user);
}

// -------------------------------------------------------------------------
// 7. B beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, BPassthroughBitForBit) {
  SCENARIO("NSU AxiMasterPort: B beat passes through to packetizer with id/resp/user preserved");
  PortFixture fx;
  axi::BBeat in = make_b(0x42, axi::Resp::EXOKAY, 0x91);
  ASSERT_TRUE(fx.port.push_b(in));
  fx.port.tick();
  ASSERT_EQ(fx.ch.b.size(), 1u);
  const auto& out = fx.ch.b.front();
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.resp, in.resp);
  EXPECT_EQ(out.user, in.user);
}

// -------------------------------------------------------------------------
// 8. R beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, RPassthroughBitForBit) {
  SCENARIO("NSU AxiMasterPort: R beat passes through to packetizer with all fields preserved");
  PortFixture fx;
  axi::RBeat in{};
  in.id = 0x37;
  for (std::size_t i = 0; i < axi::DATA_BYTES; ++i)
    in.data[i] = static_cast<uint8_t>(0xE0 + i);
  in.resp = axi::Resp::SLVERR;
  in.last = true;
  in.user = 0x12;
  ASSERT_TRUE(fx.port.push_r(in));
  fx.port.tick();
  ASSERT_EQ(fx.ch.r.size(), 1u);
  const auto& out = fx.ch.r.front();
  EXPECT_EQ(out.id,   in.id);
  EXPECT_EQ(out.data, in.data);
  EXPECT_EQ(out.resp, in.resp);
  EXPECT_EQ(out.last, in.last);
  EXPECT_EQ(out.user, in.user);
}

// -------------------------------------------------------------------------
// 9. AW + AR beats pass-through bit-for-bit unchanged (all fields).
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, AwArPassthroughBitForBit) {
  SCENARIO("NSU AxiMasterPort: AW and AR pass through with every field (id/addr/len/.../qos) preserved");
  PortFixture fx;
  axi::AwBeat aw_in = make_aw(0x5A, 0xCAFE'BABE'DEAD'BEEFull, 7, 5,
                                axi::Burst::WRAP, 0xC, 1, 0x7, 0xF, 0xAB, 0xE);
  axi::ArBeat ar_in = make_ar(0x6B, 0x1234'5678'9ABC'DEF0ull, 15, 3,
                                axi::Burst::FIXED, 0x9, 0, 0x4, 0xD, 0xCD, 0x5);
  fx.ch.aw.push_back(aw_in);
  fx.ch.ar.push_back(ar_in);
  fx.port.tick();
  auto aw = fx.port.pop_aw();
  ASSERT_TRUE(aw.has_value());
  EXPECT_EQ(aw->id, aw_in.id);     EXPECT_EQ(aw->addr, aw_in.addr);
  EXPECT_EQ(aw->len, aw_in.len);   EXPECT_EQ(aw->size, aw_in.size);
  EXPECT_EQ(aw->burst, aw_in.burst);
  EXPECT_EQ(aw->cache, aw_in.cache); EXPECT_EQ(aw->lock, aw_in.lock);
  EXPECT_EQ(aw->prot, aw_in.prot);   EXPECT_EQ(aw->region, aw_in.region);
  EXPECT_EQ(aw->user, aw_in.user);   EXPECT_EQ(aw->qos, aw_in.qos);
  auto ar = fx.port.pop_ar();
  ASSERT_TRUE(ar.has_value());
  EXPECT_EQ(ar->id, ar_in.id);     EXPECT_EQ(ar->addr, ar_in.addr);
  EXPECT_EQ(ar->len, ar_in.len);   EXPECT_EQ(ar->size, ar_in.size);
  EXPECT_EQ(ar->burst, ar_in.burst);
  EXPECT_EQ(ar->cache, ar_in.cache); EXPECT_EQ(ar->lock, ar_in.lock);
  EXPECT_EQ(ar->prot, ar_in.prot);   EXPECT_EQ(ar->region, ar_in.region);
  EXPECT_EQ(ar->user, ar_in.user);   EXPECT_EQ(ar->qos, ar_in.qos);
}

// -------------------------------------------------------------------------
// 10. Independent B and R backpressure on the response side: B packetizer
//     full while R packetizer still draining — R progresses, B holds.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, IndependentBRBackpressure_RProgressesWhileBHolds) {
  SCENARIO("NSU AxiMasterPort: B-side packetizer full does not block R-side draining (channels independent)");
  PortFixture fx;
  fx.set_loopback_caps(32, 32, 32, /*b*/ 0, /*r*/ 32);
  for (uint8_t i = 0; i < 4; ++i)
    ASSERT_TRUE(fx.port.push_b(make_b(i, axi::Resp::OKAY, 0)));
  for (uint8_t i = 0; i < 4; ++i)
    ASSERT_TRUE(fx.port.push_r(make_r(i, 0x40 + i, axi::Resp::OKAY,
                                        i == 3, 0)));
  fx.port.tick();
  EXPECT_EQ(fx.ch.r.size(), 4u);  // R drained to packetizer
  EXPECT_EQ(fx.ch.b.size(), 0u);  // B held: packetizer rejected all
  EXPECT_EQ(fx.port.b_q_size(), 4u);
  EXPECT_EQ(fx.port.r_q_size(), 0u);
  // Open the B packetizer one slot at a time; each tick should drain one.
  for (std::size_t i = 0; i < 4; ++i) {
    fx.ch.b_capacity = i + 1;
    fx.port.tick();
    EXPECT_EQ(fx.ch.b.size(), i + 1);
  }
  EXPECT_EQ(fx.port.b_q_size(), 0u);
}

// -------------------------------------------------------------------------
// 11. Simultaneous AW + W + AR availability: in one tick the depacketizer
//     hands off all three eligible request channels into the port queues.
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, SimultaneousAwWArForwardProgressInOneTick) {
  SCENARIO("NSU AxiMasterPort: AW, W, AR all ingested from depacketizer in same tick");
  PortFixture fx;
  fx.ch.aw.push_back(make_aw(0x1, 0x1000));
  fx.ch.w.push_back (make_w (0xAA, 0xFFFFFFFFu, true, 0x3C));
  fx.ch.ar.push_back(make_ar(0x2, 0x2000));
  fx.port.tick();
  EXPECT_EQ(fx.port.aw_q_size(), 1u);
  EXPECT_EQ(fx.port.w_q_size(),  1u);
  EXPECT_EQ(fx.port.ar_q_size(), 1u);
  EXPECT_EQ(fx.ch.aw.size(), 0u);
  EXPECT_EQ(fx.ch.w.size(),  0u);
  EXPECT_EQ(fx.ch.ar.size(), 0u);
}

// -------------------------------------------------------------------------
// 12. AW backpressure independent from AR. Saturate AW internal queue;
//     AR must still drain from the depacketizer (5 channels independent).
// -------------------------------------------------------------------------
TEST(NsuAxiMasterPort, AwBackpressure_DoesNotBlockAr) {
  SCENARIO("NSU AxiMasterPort: AW queue full does not block AR channel ingestion (independent per AXI4)");
  PortFixture fx;
  // Fill the port's AW internal queue but leave AR slack.
  for (std::size_t i = 0; i < fx.params.aw_queue_depth + 4; ++i)
    fx.ch.aw.push_back(make_aw(static_cast<uint8_t>(i & 0xFF), 0x1000));
  for (uint8_t i = 0; i < 3; ++i) fx.ch.ar.push_back(make_ar(i, 0x2000));
  fx.port.tick();
  EXPECT_EQ(fx.port.aw_q_size(), fx.params.aw_queue_depth);
  EXPECT_EQ(fx.port.ar_q_size(), 3u);  // AR drained fully despite AW jam
  EXPECT_EQ(fx.ch.ar.size(),     0u);
}
