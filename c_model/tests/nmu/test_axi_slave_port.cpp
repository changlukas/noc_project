// Unit tests for nmu/axi_slave_port.hpp — the NMU upstream-facing AXI4
// subordinate transparent transport port.
//
// Test strategy (per Stage 3 brief):
//   - LoopbackChannelSet stands in for the future ni/packetize.hpp +
//     ni/depacketize.hpp pair so the port can be exercised without standing
//     up the NoC fabric.
//   - PortParams come from c_model/config/port_params.yaml (loaded at
//     fixture setup; NO hardcoded queue-depth literals in the port header).
//   - Port contract: per-channel FIFO order for all beats regardless of
//     AXI ID. Cross-ID completion ordering / per-ID response reordering
//     is the ROB stage's responsibility (see plan §3.1), NOT this port's.
#include "common/loopback_request_io.hpp"
#include "common/loopback_response_io.hpp"
#include "common/scenario.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/port_params.hpp"
#include <cstdint>
#include <gtest/gtest.h>

namespace cmod = ni::cmodel;
namespace nmu = ni::cmodel::nmu;
namespace axi = ni::cmodel::axi;
namespace test = ni::cmodel::testing;

namespace {

// PortParams come from the project-level YAML via the shared
// ni::cmodel::load_port_params_yaml helper. Tests that need different depths
// construct cmod::PortParams directly via brace-init.

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0, uint8_t size = 5,
                    axi::Burst burst = axi::Burst::INCR, uint8_t cache = 0xA, uint8_t lock = 0,
                    uint8_t prot = 0x3, uint8_t region = 0xF, uint8_t user = 0x55,
                    uint8_t qos = 0xC) {
    return axi::AwBeat{id, addr, len, size, burst, cache, lock, prot, region, user, qos};
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr, uint8_t len = 0, uint8_t size = 5,
                    axi::Burst burst = axi::Burst::INCR, uint8_t cache = 0x5, uint8_t lock = 0,
                    uint8_t prot = 0x2, uint8_t region = 0xE, uint8_t user = 0xAA,
                    uint8_t qos = 0x9) {
    return axi::ArBeat{id, addr, len, size, burst, cache, lock, prot, region, user, qos};
}
axi::WBeat make_w(uint8_t fill, uint32_t strb, bool last, uint8_t user) {
    axi::WBeat w{};
    w.data.fill(fill);
    w.strb = strb;
    w.last = last;
    w.user = user;
    return w;
}
axi::BBeat make_b(uint8_t id, axi::Resp resp, uint8_t user) {
    return axi::BBeat{id, resp, user};
}
axi::RBeat make_r(uint8_t id, uint8_t fill, axi::Resp resp, bool last, uint8_t user) {
    axi::RBeat r{};
    r.id = id;
    r.data.fill(fill);
    r.resp = resp;
    r.last = last;
    r.user = user;
    return r;
}

struct PortFixture {
    test::LoopbackChannelSet ch{};
    test::LoopbackRequestPacketizer pkt{ch.request};
    test::LoopbackResponseDepacketizer depkt{ch.response};
    nmu::PortParams params;
    nmu::AxiSlavePort port;

    PortFixture()
        : params(nmu::load_nmu_port_params("config/port_params.yaml")), port(pkt, depkt, params) {}

    // Tighter loopback caps so backpressure tests can drive saturation
    // without exhausting the default 32-deep ports.
    void set_loopback_caps(std::size_t aw, std::size_t w, std::size_t ar, std::size_t b,
                           std::size_t r) {
        ch.request.aw_capacity = aw;
        ch.request.w_capacity = w;
        ch.request.ar_capacity = ar;
        ch.response.b_capacity = b;
        ch.response.r_capacity = r;
    }
};

}  // namespace

// -------------------------------------------------------------------------
// 1. Basic per-channel handshake: push N beats, tick, drain N beats out.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, AwBasicHandshake_PushPopNoLoss) {
    SCENARIO("NMU AxiSlavePort: 8 AW pushes forward to packetizer in original order, no loss");
    PortFixture fx;
    for (uint8_t i = 0; i < 8; ++i) ASSERT_TRUE(fx.port.push_aw(make_aw(i, 0x1000 + i * 32)));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.aw.size(), 8u);
    for (uint8_t i = 0; i < 8; ++i) EXPECT_EQ(fx.ch.request.aw[i].id, i);
}

TEST(NmuAxiSlavePort, ArBasicHandshake_PushPopNoLoss) {
    SCENARIO("NMU AxiSlavePort: 8 AR pushes forward to packetizer in original order, no loss");
    PortFixture fx;
    for (uint8_t i = 0; i < 8; ++i) ASSERT_TRUE(fx.port.push_ar(make_ar(i, 0x2000 + i * 32)));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.ar.size(), 8u);
    for (uint8_t i = 0; i < 8; ++i) EXPECT_EQ(fx.ch.request.ar[i].id, i);
}

TEST(NmuAxiSlavePort, WBasicHandshake_PushPopNoLoss) {
    SCENARIO("NMU AxiSlavePort: 8 W beat pushes forward to packetizer with payload preserved");
    PortFixture fx;
    for (uint8_t i = 0; i < 8; ++i)
        ASSERT_TRUE(fx.port.push_w(make_w(0x10 + i, 0xFFFFFFFFu, i == 7, i)));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.w.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(fx.ch.request.w[i].data[0], 0x10 + i);
}

// -------------------------------------------------------------------------
// 2. Per-channel backpressure: fill internal queue to capacity, push_*
//    must return false; draining one frees room and the next push succeeds.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, AwBackpressure_FullThenDrainThenAcceptOne) {
    SCENARIO(
        "NMU AxiSlavePort: AW queue full rejects push; drain one freeing slot lets next AW in");
    PortFixture fx;
    fx.set_loopback_caps(0, 0, 0, 0, 0);  // packetizer cannot accept anything
    for (std::size_t i = 0; i < fx.params.aw_queue_depth; ++i)
        ASSERT_TRUE(fx.port.push_aw(make_aw(static_cast<uint8_t>(i & 0xFF), 0x1000 + i * 32)));
    EXPECT_FALSE(fx.port.push_aw(make_aw(0xFF, 0x9000)));
    // Open packetizer one slot.
    fx.ch.request.aw_capacity = 1;
    fx.port.tick();  // forwards one to packetizer, freeing one port slot
    EXPECT_TRUE(fx.port.push_aw(make_aw(0xFF, 0x9000)));
}

// -------------------------------------------------------------------------
// 3. Queue boundary: exactly-full state correct; a failed push must NOT
//    duplicate-enqueue when retried later.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, AwBoundary_FailedPushDoesNotDuplicateOnRetry) {
    SCENARIO(
        "NMU AxiSlavePort: failed AW pushes do not silently land; retry counts as exactly one");
    PortFixture fx;
    fx.set_loopback_caps(0, 0, 0, 0, 0);
    for (std::size_t i = 0; i < fx.params.aw_queue_depth; ++i)
        ASSERT_TRUE(fx.port.push_aw(make_aw(static_cast<uint8_t>(i & 0xFF), 0x1000 + i * 32)));
    EXPECT_EQ(fx.port.aw_q_size(), fx.params.aw_queue_depth);
    // Multiple rejected retries of the SAME beat must not silently land.
    axi::AwBeat retry = make_aw(0xAB, 0xDEAD);
    EXPECT_FALSE(fx.port.push_aw(retry));
    EXPECT_FALSE(fx.port.push_aw(retry));
    EXPECT_FALSE(fx.port.push_aw(retry));
    EXPECT_EQ(fx.port.aw_q_size(), fx.params.aw_queue_depth);
    // Open packetizer one slot, tick to free one port slot, then accept retry.
    fx.ch.request.aw_capacity = 1;
    fx.port.tick();
    EXPECT_TRUE(fx.port.push_aw(retry));
    // Now there must be EXACTLY one retry-id (0xAB) in the system, not three.
    fx.ch.request.aw_capacity = 64;
    fx.port.tick();
    std::size_t retry_id_count = 0;
    for (const auto& b : fx.ch.request.aw)
        if (b.id == 0xAB) ++retry_id_count;
    EXPECT_EQ(retry_id_count, 1u);
}

// -------------------------------------------------------------------------
// 4. AW channel FIFO order preserved end-to-end (mixed ids).
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, AwFifoOrder_PreservedAcrossMixedIds) {
    SCENARIO(
        "NMU AxiSlavePort: AW channel FIFO order preserved across mixed AXI ids (no per-id "
        "reorder)");
    PortFixture fx;
    // Port contract: per-channel FIFO order for all beats regardless of AXI ID.
    // Cross-ID completion ordering / per-ID response reordering is the ROB
    // stage's responsibility (see plan §3.1), NOT this port's.
    const std::vector<uint8_t> ids{3, 5, 3, 7, 5, 0, 7, 3};
    for (auto id : ids) ASSERT_TRUE(fx.port.push_aw(make_aw(id, 0x1000)));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.aw.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(fx.ch.request.aw[i].id, ids[i]);
}

// -------------------------------------------------------------------------
// 5. AR channel FIFO order preserved end-to-end (mixed ids).
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, ArFifoOrder_PreservedAcrossMixedIds) {
    SCENARIO(
        "NMU AxiSlavePort: AR channel FIFO order preserved across mixed AXI ids (no per-id "
        "reorder)");
    PortFixture fx;
    // Port contract: per-channel FIFO order for all beats regardless of AXI ID.
    // Cross-ID completion ordering / per-ID response reordering is the ROB
    // stage's responsibility (see plan §3.1), NOT this port's.
    const std::vector<uint8_t> ids{1, 2, 1, 3, 2, 0, 3, 1};
    for (auto id : ids) ASSERT_TRUE(fx.port.push_ar(make_ar(id, 0x2000)));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.ar.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(fx.ch.request.ar[i].id, ids[i]);
}

// -------------------------------------------------------------------------
// 6. W beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, WPassthroughBitForBit) {
    SCENARIO(
        "NMU AxiSlavePort: W beat passes through with data/strb/last/user bit-for-bit preserved");
    PortFixture fx;
    axi::WBeat in{};
    for (std::size_t i = 0; i < axi::DATA_BYTES; ++i) in.data[i] = static_cast<uint8_t>(0xC0 + i);
    in.strb = 0xDEAD'BEEFu;
    in.last = true;
    in.user = 0x77;
    ASSERT_TRUE(fx.port.push_w(in));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.w.size(), 1u);
    const auto& out = fx.ch.request.w.front();
    EXPECT_EQ(out.data, in.data);
    EXPECT_EQ(out.strb, in.strb);
    EXPECT_EQ(out.last, in.last);
    EXPECT_EQ(out.user, in.user);
}

// -------------------------------------------------------------------------
// 7. B beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, BPassthroughBitForBit) {
    SCENARIO("NMU AxiSlavePort: B beat passes through with id/resp/user bit-for-bit preserved");
    PortFixture fx;
    axi::BBeat in = make_b(0x42, axi::Resp::EXOKAY, 0x91);
    fx.ch.response.b.push_back(in);
    fx.port.tick();
    auto out = fx.port.pop_b();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, in.id);
    EXPECT_EQ(out->resp, in.resp);
    EXPECT_EQ(out->user, in.user);
}

// -------------------------------------------------------------------------
// 8. R beats pass-through bit-for-bit unchanged.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, RPassthroughBitForBit) {
    SCENARIO(
        "NMU AxiSlavePort: R beat passes through with all fields (id/data/resp/last/user) "
        "preserved");
    PortFixture fx;
    axi::RBeat in{};
    in.id = 0x37;
    for (std::size_t i = 0; i < axi::DATA_BYTES; ++i) in.data[i] = static_cast<uint8_t>(0xE0 + i);
    in.resp = axi::Resp::SLVERR;
    in.last = true;
    in.user = 0x12;
    fx.ch.response.r.push_back(in);
    fx.port.tick();
    auto out = fx.port.pop_r();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, in.id);
    EXPECT_EQ(out->data, in.data);
    EXPECT_EQ(out->resp, in.resp);
    EXPECT_EQ(out->last, in.last);
    EXPECT_EQ(out->user, in.user);
}

// -------------------------------------------------------------------------
// 9. AW + AR beats pass-through bit-for-bit unchanged (all fields).
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, AwArPassthroughBitForBit) {
    SCENARIO(
        "NMU AxiSlavePort: AW and AR pass through with every field (id/addr/len/.../qos) "
        "preserved");
    PortFixture fx;
    axi::AwBeat aw_in = make_aw(0x5A, 0xCAFE'BABE'DEAD'BEEFull, 7, 5, axi::Burst::WRAP, 0xC, 1, 0x7,
                                0xF, 0xAB, 0xE);
    axi::ArBeat ar_in = make_ar(0x6B, 0x1234'5678'9ABC'DEF0ull, 15, 3, axi::Burst::FIXED, 0x9, 0,
                                0x4, 0xD, 0xCD, 0x5);
    ASSERT_TRUE(fx.port.push_aw(aw_in));
    ASSERT_TRUE(fx.port.push_ar(ar_in));
    fx.port.tick();
    ASSERT_EQ(fx.ch.request.aw.size(), 1u);
    ASSERT_EQ(fx.ch.request.ar.size(), 1u);
    const auto& aw = fx.ch.request.aw.front();
    EXPECT_EQ(aw.id, aw_in.id);
    EXPECT_EQ(aw.addr, aw_in.addr);
    EXPECT_EQ(aw.len, aw_in.len);
    EXPECT_EQ(aw.size, aw_in.size);
    EXPECT_EQ(aw.burst, aw_in.burst);
    EXPECT_EQ(aw.cache, aw_in.cache);
    EXPECT_EQ(aw.lock, aw_in.lock);
    EXPECT_EQ(aw.prot, aw_in.prot);
    EXPECT_EQ(aw.region, aw_in.region);
    EXPECT_EQ(aw.user, aw_in.user);
    EXPECT_EQ(aw.qos, aw_in.qos);
    const auto& ar = fx.ch.request.ar.front();
    EXPECT_EQ(ar.id, ar_in.id);
    EXPECT_EQ(ar.addr, ar_in.addr);
    EXPECT_EQ(ar.len, ar_in.len);
    EXPECT_EQ(ar.size, ar_in.size);
    EXPECT_EQ(ar.burst, ar_in.burst);
    EXPECT_EQ(ar.cache, ar_in.cache);
    EXPECT_EQ(ar.lock, ar_in.lock);
    EXPECT_EQ(ar.prot, ar_in.prot);
    EXPECT_EQ(ar.region, ar_in.region);
    EXPECT_EQ(ar.user, ar_in.user);
    EXPECT_EQ(ar.qos, ar_in.qos);
}

// -------------------------------------------------------------------------
// 10. Independent B and R backpressure: B internal queue full while R is
//     still draining — R progresses, B holds, no cross-channel contamination.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, IndependentBRBackpressure_RProgressesWhileBHolds) {
    SCENARIO("NMU AxiSlavePort: B queue full does not block R ingestion (independent channels)");
    // Custom params: tiny B queue (1), normal R queue.
    test::LoopbackChannelSet ch{};
    test::LoopbackRequestPacketizer pkt{ch.request};
    test::LoopbackResponseDepacketizer depkt{ch.response};
    nmu::PortParams p{32, 32, 32, /*b*/ 1, /*r*/ 32, /*depkt_b*/ 32, /*depkt_r*/ 32};
    nmu::AxiSlavePort port(pkt, depkt, p);

    // Pre-fill the depkt-side queues with several beats on each channel.
    for (uint8_t i = 0; i < 4; ++i) ch.response.b.push_back(make_b(i, axi::Resp::OKAY, 0));
    for (uint8_t i = 0; i < 4; ++i)
        ch.response.r.push_back(make_r(i, 0x40 + i, axi::Resp::OKAY, i == 3, 0));

    port.tick();
    // R should fully ingest (cap 32, 4 beats); B should ingest only 1
    // because its cap is 1 and the test does not pop_b yet.
    EXPECT_EQ(port.r_q_size(), 4u);
    EXPECT_EQ(port.b_q_size(), 1u);
    EXPECT_EQ(ch.response.b.size(), 3u);  // 3 still pending on the depkt side
    EXPECT_EQ(ch.response.r.size(), 0u);

    // Now consume B one at a time — each tick after pop_b frees a slot.
    for (int i = 0; i < 4; ++i) {
        auto out = port.pop_b();
        EXPECT_TRUE(out.has_value());
        port.tick();
    }
    EXPECT_EQ(port.b_q_size(), 0u);
    EXPECT_EQ(ch.response.b.size(), 0u);
}

// -------------------------------------------------------------------------
// 11. Simultaneous AW + W + AR availability: in one tick all three eligible
//     request channels make forward progress.
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, SimultaneousAwWArForwardProgressInOneTick) {
    SCENARIO("NMU AxiSlavePort: AW, W, AR all forward in same tick when packetizer has room");
    PortFixture fx;
    ASSERT_TRUE(fx.port.push_aw(make_aw(0x1, 0x1000)));
    ASSERT_TRUE(fx.port.push_w(make_w(0xAA, 0xFFFFFFFFu, true, 0x3C)));
    ASSERT_TRUE(fx.port.push_ar(make_ar(0x2, 0x2000)));
    fx.port.tick();
    EXPECT_EQ(fx.ch.request.aw.size(), 1u);
    EXPECT_EQ(fx.ch.request.w.size(), 1u);
    EXPECT_EQ(fx.ch.request.ar.size(), 1u);
    EXPECT_EQ(fx.port.aw_q_size(), 0u);
    EXPECT_EQ(fx.port.w_q_size(), 0u);
    EXPECT_EQ(fx.port.ar_q_size(), 0u);
}

// -------------------------------------------------------------------------
// 12. W backpressure independent from AW. Fill W internal queue, AW still
//     accepts new beats (per AXI4 IHI 0022: 5 channels are independent).
// -------------------------------------------------------------------------
TEST(NmuAxiSlavePort, WBackpressure_DoesNotBlockAw) {
    SCENARIO(
        "NMU AxiSlavePort: W queue full does not block AW channel (independent per AXI4 §A3.4)");
    PortFixture fx;
    fx.set_loopback_caps(/*aw*/ 32, /*w*/ 0, /*ar*/ 32, /*b*/ 32, /*r*/ 32);
    for (std::size_t i = 0; i < fx.params.w_queue_depth; ++i)
        ASSERT_TRUE(fx.port.push_w(
            make_w(0x10 + (i & 0xF), 0xFFFFFFFFu, i + 1 == fx.params.w_queue_depth, 0)));
    EXPECT_FALSE(fx.port.push_w(make_w(0xFF, 0, true, 0)));
    // AW must still flow despite W jam.
    ASSERT_TRUE(fx.port.push_aw(make_aw(0x9, 0x1000)));
    fx.port.tick();
    EXPECT_EQ(fx.ch.request.aw.size(), 1u);
}
