// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/axi_slave.hpp"
#include "common/scenario.hpp"
#include "mock_memory_port.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;
namespace test = ni::cmodel::axi::testing;

TEST(AxiSlave, ConstructsAndAcceptsEmptyTick) {
    SCENARIO("AxiSlave: empty tick produces no captured writes/reads (no spurious activity)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    EXPECT_EQ(slave.aw_q_size(), 0u);
    slave.tick();
    EXPECT_EQ(slave.b_q_size(), 0u);
}

TEST(AxiSlave, WriteBurstSingleBeatInBoundsOkay) {
    SCENARIO("AxiSlave: 1-beat in-bounds write forwards to memory port and emits OKAY B");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);

    axi::AwBeat aw{};
    aw.id = 7;
    aw.addr = 0x1000;
    aw.len = 0;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);

    axi::WBeat w{};
    w.data.fill(0xCD);
    w.strb = 0xFFFF'FFFFu;
    w.last = true;
    slave.push_w(w);

    slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(mem.captured_writes.front().id, 7);
    EXPECT_EQ(mem.captured_writes.front().addr, 0x1000u);
    EXPECT_EQ(mem.captured_writes.front().last, true);

    mem.queued_write_resps.push_back(axi::MemWriteResp{
        mem.captured_writes.front().id, axi::Resp::OKAY, mem.captured_writes.front().tag});

    slave.tick();
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 7);
    EXPECT_EQ(b->resp, axi::Resp::OKAY);
}

TEST(AxiSlave, WriteBurstIncr8Beat_InBounds) {
    SCENARIO("AxiSlave: 8-beat INCR forwards each beat with per-beat addr increment of 32B");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);

    axi::AwBeat aw{};
    aw.id = 3;
    aw.addr = 0x2000;
    aw.len = 7;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);

    for (uint8_t i = 0; i < 8; ++i) {
        axi::WBeat w{};
        w.data.fill(0x10 + i);
        w.strb = 0xFFFF'FFFFu;
        w.last = (i == 7);
        slave.push_w(w);
    }

    for (int t = 0; t < 16; ++t) slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(mem.captured_writes[i].addr, 0x2000u + i * 32u);
        EXPECT_EQ(mem.captured_writes[i].data[0], 0x10 + static_cast<uint8_t>(i));
        EXPECT_EQ(mem.captured_writes[i].last, i == 7);
    }

    for (std::size_t i = 0; i < 8; ++i) {
        mem.queued_write_resps.push_back(
            axi::MemWriteResp{3, axi::Resp::OKAY, mem.captured_writes[i].tag});
    }
    for (int t = 0; t < 8; ++t) slave.tick();
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 3);
}

TEST(AxiSlave, AwWIndependence_WBeforeAw) {
    SCENARIO("AxiSlave: W beats queued before AW are held, then forwarded once AW arrives");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);

    for (uint8_t i = 0; i < 2; ++i) {
        axi::WBeat w{};
        w.data.fill(0xAA + i);
        w.strb = 0xFFFF'FFFFu;
        w.last = (i == 1);
        slave.push_w(w);
    }
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 0u);

    axi::AwBeat aw{};
    aw.id = 5;
    aw.addr = 0x3000;
    aw.len = 1;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);

    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 2u);
    EXPECT_EQ(mem.captured_writes[0].data[0], 0xAA);
    EXPECT_EQ(mem.captured_writes[1].data[0], 0xAB);
}

TEST(AxiSlave, WriteBurstAtomicOob_PushesDecerrSkipsMemory) {
    SCENARIO("AxiSlave: atomic write fully OOB → emits DECERR B without touching memory");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x100);  // 256 bytes

    axi::AwBeat aw{};
    aw.id = 9;
    aw.addr = 0x10E0;
    aw.len = 3;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    // 4 beats * 32 bytes = 128 -> 0x10E0 + 128 = 0x1160 > 0x1100 -> OOB
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 4; ++i) {
        axi::WBeat w{};
        w.data.fill(0);
        w.strb = 0xFFFF'FFFFu;
        w.last = (i == 3);
        slave.push_w(w);
    }
    slave.tick();

    EXPECT_EQ(mem.captured_writes.size(), 0u);
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::DECERR);
    EXPECT_EQ(slave.w_q_size(), 0u);
}

TEST(AxiSlave, ReadBurstSingleBeatInBoundsOkay) {
    SCENARIO("AxiSlave: 1-beat in-bounds read forwards AR to memory, returns OKAY R with payload");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::ArBeat ar{};
    ar.id = 2;
    ar.addr = 0x1080;
    ar.len = 0;
    ar.size = 5;
    ar.burst = axi::Burst::INCR;
    slave.push_ar(ar);
    slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 1u);
    EXPECT_EQ(mem.captured_reads.front().addr, 0x1080u);

    axi::MemReadResp rresp{};
    rresp.id = 2;
    rresp.data.fill(0x77);
    rresp.resp = axi::Resp::OKAY;
    rresp.last = true;
    rresp.tag = mem.captured_reads.front().tag;
    mem.queued_read_resps.push_back(rresp);
    slave.tick();

    auto r = slave.pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 2);
    EXPECT_EQ(r->resp, axi::Resp::OKAY);
    EXPECT_EQ(r->last, true);
    EXPECT_EQ(r->data[0], 0x77);
}

TEST(AxiSlave, ReadBurstIncr4Beat_InBounds) {
    SCENARIO("AxiSlave: 4-beat INCR read emits 4 R beats in order with RLAST on beat 3");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::ArBeat ar{};
    ar.id = 6;
    ar.addr = 0x1000;
    ar.len = 3;
    ar.size = 5;
    ar.burst = axi::Burst::INCR;
    slave.push_ar(ar);
    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 4u);

    for (uint8_t i = 0; i < 4; ++i) {
        axi::MemReadResp rresp{};
        rresp.id = 6;
        rresp.data.fill(0xB0 + i);
        rresp.resp = axi::Resp::OKAY;
        rresp.last = (i == 3);
        rresp.tag = mem.captured_reads[i].tag;
        mem.queued_read_resps.push_back(rresp);
    }
    for (int t = 0; t < 4; ++t) slave.tick();

    for (uint8_t i = 0; i < 4; ++i) {
        auto r = slave.pop_r();
        ASSERT_TRUE(r.has_value()) << "beat " << int(i);
        EXPECT_EQ(r->data[0], 0xB0 + i);
        EXPECT_EQ(r->last, i == 3);
    }
}

TEST(AxiSlave, ReadBurstAtomicOob_AllBeatsDecerr) {
    SCENARIO("AxiSlave: atomic OOB read → every R beat returns DECERR, none submitted to memory");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x100);
    axi::ArBeat ar{};
    ar.id = 4;
    ar.addr = 0x10F0;
    ar.len = 1;
    ar.size = 5;
    ar.burst = axi::Burst::INCR;
    slave.push_ar(ar);
    slave.tick();
    EXPECT_EQ(mem.captured_reads.size(), 0u);
    for (uint8_t i = 0; i < 2; ++i) {
        auto r = slave.pop_r();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->resp, axi::Resp::DECERR);
        EXPECT_EQ(r->last, i == 1);
    }
}

TEST(AxiSlave, BackpressureRetry_NoBeatDropped) {
    SCENARIO(
        "AxiSlave: memory port at capacity=1 → slave retries next beat on each tick, no drops");
    test::MockMemoryPort mem;
    mem.write_capacity = 1;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AwBeat aw{};
    aw.id = 1;
    aw.addr = 0x1000;
    aw.len = 2;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 3; ++i) {
        axi::WBeat w{};
        w.data.fill(0x40 + i);
        w.strb = 0xFFFF'FFFFu;
        w.last = (i == 2);
        slave.push_w(w);
    }
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(slave.w_q_size(), 2u);

    mem.queued_write_resps.push_back(
        axi::MemWriteResp{1, axi::Resp::OKAY, mem.captured_writes[0].tag});
    mem.captured_writes.pop_front();
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 1u);

    mem.queued_write_resps.push_back(
        axi::MemWriteResp{1, axi::Resp::OKAY, mem.captured_writes[0].tag});
    mem.captured_writes.pop_front();
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 1u);

    EXPECT_EQ(slave.w_q_size(), 0u);
}

TEST(AxiSlave, WriteBurstWorstRespAccumulatedAcrossBeats) {
    SCENARIO(
        "AxiSlave: write B picks worst per-beat resp (arithmetic max, DECERR > SLVERR > OKAY)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);

    axi::AwBeat aw{};
    aw.id = 11;
    aw.addr = 0x4000;
    aw.len = 2;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 3; ++i) {
        axi::WBeat w{};
        w.data.fill(0);
        w.strb = 0xFFFF'FFFFu;
        w.last = (i == 2);
        slave.push_w(w);
    }
    // 3 ticks to submit all 3 W beats
    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 3u);

    // Mock returns: beat 0 OKAY, beat 1 DECERR, beat 2 SLVERR
    // worst should be DECERR (3) > SLVERR (2) > OKAY (0)
    mem.queued_write_resps.push_back(
        axi::MemWriteResp{11, axi::Resp::OKAY, mem.captured_writes[0].tag});
    mem.queued_write_resps.push_back(
        axi::MemWriteResp{11, axi::Resp::DECERR, mem.captured_writes[1].tag});
    mem.queued_write_resps.push_back(
        axi::MemWriteResp{11, axi::Resp::SLVERR, mem.captured_writes[2].tag});

    for (int t = 0; t < 4; ++t) slave.tick();
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::DECERR)
        << "worst_resp should pick arithmetic max (DECERR=3), not last (SLVERR=2)";
}

TEST(AxiSlave, SequentialBurstsDifferentIds) {
    SCENARIO(
        "AxiSlave: sequential bursts with distinct ids each complete cleanly without interference");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AwBeat aw1{};
    aw1.id = 1;
    aw1.addr = 0x1000;
    aw1.len = 0;
    aw1.size = 5;
    aw1.burst = axi::Burst::INCR;
    slave.push_aw(aw1);
    axi::WBeat w1{};
    w1.data.fill(0x11);
    w1.strb = 0xFFFF'FFFFu;
    w1.last = true;
    slave.push_w(w1);
    slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(mem.captured_writes[0].id, 1);
    mem.queued_write_resps.push_back(
        axi::MemWriteResp{1, axi::Resp::OKAY, mem.captured_writes[0].tag});
    slave.tick();
    EXPECT_TRUE(slave.pop_b().has_value());

    axi::AwBeat aw2 = aw1;
    aw2.id = 2;
    aw2.addr = 0x1100;
    axi::WBeat w2 = w1;
    w2.data.fill(0x22);
    slave.push_aw(aw2);
    slave.push_w(w2);
    mem.captured_writes.pop_front();
    slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(mem.captured_writes[0].id, 2);
    EXPECT_EQ(mem.captured_writes[0].data[0], 0x22);
}

// Phase B-3b: narrow burst (size=2, bpb=4) addr+strb forwarded per-beat
// without slave-side reinterpretation. AxiSlave just propagates the W beats
// to the memory port; addr increments by bpb per INCR beat.
TEST(AxiSlave, NarrowTransferForwardedToMemory) {
    SCENARIO(
        "AxiSlave: narrow burst (size=2, bpb=4) propagates per-beat addr+strb to memory unchanged");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AwBeat aw{};
    aw.id = 9;
    aw.addr = 0x1004;
    aw.len = 1;
    aw.size = 2;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);

    // Beat 0: addr 0x1004, byte_lane=4, strb=0xF<<4=0xF0.
    axi::WBeat w0{};
    w0.data.fill(0);
    w0.strb = 0x000000F0u;
    w0.last = false;
    slave.push_w(w0);
    // Beat 1: addr 0x1008, byte_lane=8, strb=0xF<<8=0xF00.
    axi::WBeat w1{};
    w1.data.fill(0);
    w1.strb = 0x00000F00u;
    w1.last = true;
    slave.push_w(w1);

    for (int t = 0; t < 4; ++t) slave.tick();

    ASSERT_EQ(mem.captured_writes.size(), 2u);
    EXPECT_EQ(mem.captured_writes[0].addr, 0x1004u);
    EXPECT_EQ(mem.captured_writes[0].strb, 0x000000F0u);
    EXPECT_EQ(mem.captured_writes[0].last, false);
    EXPECT_EQ(mem.captured_writes[1].addr, 0x1008u);
    EXPECT_EQ(mem.captured_writes[1].strb, 0x00000F00u);
    EXPECT_EQ(mem.captured_writes[1].last, true);
}

// Phase B-2.3: WSTRB on the W channel passes through to MemWriteReq.strb
// unchanged. AxiSlave does not interpret WSTRB; the memory port records the
// exact mask the master emitted so per-byte enable semantics survive.
TEST(AxiSlave, SparseStrbForwardedToMemory) {
    SCENARIO(
        "AxiSlave: WSTRB on W passes through verbatim to MemWriteReq.strb (no reinterpretation)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AwBeat aw{};
    aw.id = 8;
    aw.addr = 0x1000;
    aw.len = 0;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    slave.push_aw(aw);

    axi::WBeat w{};
    w.data.fill(0xCC);
    w.strb = 0xFFFFFFF8u;  // lanes 0..2 cleared, lanes 3..31 set
    w.last = true;
    slave.push_w(w);

    slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(mem.captured_writes[0].strb, 0xFFFFFFF8u);
}

// Regression: when several AWs are queued before their B responses come back,
// the W router must advance to the next AW once the current burst's W beats
// are fully forwarded — not wait for the B drain. Otherwise the second burst's
// W beats are mis-routed to (or overwrite) the first burst's address space.
TEST(AxiSlave, ConcurrentBurstsDifferentIds_WRoutingAdvances) {
    SCENARIO(
        "AxiSlave: 3 queued AWs+Ws all reach memory with correct id/addr routing before any B "
        "drains");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AwBeat aw1{};
    aw1.id = 1;
    aw1.addr = 0x1000;
    aw1.len = 0;
    aw1.size = 5;
    aw1.burst = axi::Burst::INCR;
    axi::AwBeat aw2 = aw1;
    aw2.id = 2;
    aw2.addr = 0x1020;
    axi::AwBeat aw3 = aw1;
    aw3.id = 3;
    aw3.addr = 0x1040;

    axi::WBeat w1{};
    w1.data.fill(0x11);
    w1.strb = 0xFFFF'FFFFu;
    w1.last = true;
    axi::WBeat w2 = w1;
    w2.data.fill(0x22);
    axi::WBeat w3 = w1;
    w3.data.fill(0x33);

    // Push 3 AWs + 3 W beats all in one tick, no B responses yet.
    slave.push_aw(aw1);
    slave.push_w(w1);
    slave.push_aw(aw2);
    slave.push_w(w2);
    slave.push_aw(aw3);
    slave.push_w(w3);
    slave.tick();

    // All 3 W beats must reach memory with the correct (id, addr, data) routing.
    ASSERT_EQ(mem.captured_writes.size(), 3u);
    EXPECT_EQ(mem.captured_writes[0].id, 1);
    EXPECT_EQ(mem.captured_writes[0].addr, 0x1000u);
    EXPECT_EQ(mem.captured_writes[0].data[0], 0x11);
    EXPECT_EQ(mem.captured_writes[1].id, 2);
    EXPECT_EQ(mem.captured_writes[1].addr, 0x1020u);
    EXPECT_EQ(mem.captured_writes[1].data[0], 0x22);
    EXPECT_EQ(mem.captured_writes[2].id, 3);
    EXPECT_EQ(mem.captured_writes[2].addr, 0x1040u);
    EXPECT_EQ(mem.captured_writes[2].data[0], 0x33);
}

// Phase B-4: WRAP burst per-beat address wraps at wrap_upper = wrap_lower +
// total_burst_bytes (AXI4 IHI 0022 B1.4.3). wrap_lower = addr &
// ~(total_burst_bytes - 1). WRAP requires len ∈ {1,3,7,15} and addr aligned
// to (1<<size); the parser enforces these, so the slave can assume total is
// a power of 2.
TEST(AxiSlave, WrapBurstLen3_4BeatActualWrap) {
    SCENARIO(
        "AxiSlave WRAP len=3: 4 beats wrap at wrap_upper, beats 1-3 land at wrap_lower onward");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::ArBeat ar{};
    ar.id = 5;
    ar.addr = 0x1060;  // burst total = 4*32 = 128, wrap_lower = 0x1000, wrap_upper = 0x1080
    ar.len = 3;
    ar.size = 5;
    ar.burst = axi::Burst::WRAP;
    // beat 0 @ 0x1060; beat 1 @ 0x1080 → wraps → 0x1000; beat 2 @ 0x1020; beat 3 @ 0x1040
    slave.push_ar(ar);
    for (int t = 0; t < 6; ++t) slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 4u);
    EXPECT_EQ(mem.captured_reads[0].addr, 0x1060u);
    EXPECT_EQ(mem.captured_reads[1].addr, 0x1000u);
    EXPECT_EQ(mem.captured_reads[2].addr, 0x1020u);
    EXPECT_EQ(mem.captured_reads[3].addr, 0x1040u);
}

TEST(AxiSlave, WrapBurstLen1_2BeatNoWrap) {
    SCENARIO("AxiSlave WRAP len=1: 2 beats stay inside window, no actual wrap occurs");
    // 2-beat WRAP that stays inside the wrap window without crossing wrap_upper.
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);
    axi::ArBeat ar{};
    ar.id = 7;
    ar.addr = 0x1040;
    ar.len = 1;
    ar.size = 5;
    ar.burst = axi::Burst::WRAP;
    // burst total = 64, wrap_lower = 0x1040 (already aligned to 64), wrap_upper = 0x1080.
    // Beats at 0x1040, 0x1060 (both < 0x1080).
    slave.push_ar(ar);
    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 2u);
    EXPECT_EQ(mem.captured_reads[0].addr, 0x1040u);
    EXPECT_EQ(mem.captured_reads[1].addr, 0x1060u);
}

TEST(AxiSlave, WrapBurstLen7_8BeatActualWrap) {
    SCENARIO(
        "AxiSlave WRAP len=7: 8 beats wrap mid-burst (start mid-window), correct address order");
    // 8-beat WRAP, addr mid-window.
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);
    axi::ArBeat ar{};
    ar.id = 9;
    ar.addr = 0x10C0;
    ar.len = 7;
    ar.size = 5;
    ar.burst = axi::Burst::WRAP;
    // burst total = 256, wrap_lower = 0x1000, wrap_upper = 0x1100. Beats start at 0x10C0.
    // beat 0 @ 0x10C0, beat 1 @ 0x10E0, beat 2 @ 0x1100 → wraps → 0x1000, ..., beat 7 @ 0x10A0.
    slave.push_ar(ar);
    for (int t = 0; t < 10; ++t) slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 8u);
    EXPECT_EQ(mem.captured_reads[0].addr, 0x10C0u);
    EXPECT_EQ(mem.captured_reads[1].addr, 0x10E0u);
    EXPECT_EQ(mem.captured_reads[2].addr, 0x1000u);
    EXPECT_EQ(mem.captured_reads[7].addr, 0x10A0u);
}

TEST(AxiSlave, WrapBurstLen15_16Beat) {
    SCENARIO("AxiSlave WRAP len=15: 16-beat narrow (size=4, bpb=16) wraps at 256B window");
    // 16-beat narrow WRAP — size=4 (bpb=16, burst total = 16*16 = 256).
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);
    axi::ArBeat ar{};
    ar.id = 11;
    ar.addr = 0x10F0;
    ar.len = 15;
    ar.size = 4;
    ar.burst = axi::Burst::WRAP;
    // burst total = 256, wrap_lower = 0x1000, wrap_upper = 0x1100. 16 beats of 16 bytes.
    slave.push_ar(ar);
    for (int t = 0; t < 20; ++t) slave.tick();
    ASSERT_EQ(mem.captured_reads.size(), 16u);
    EXPECT_EQ(mem.captured_reads[0].addr, 0x10F0u);
    EXPECT_EQ(mem.captured_reads[1].addr, 0x1000u);  // wraps immediately after first beat
    EXPECT_EQ(mem.captured_reads[15].addr, 0x10E0u);
}

// Phase B-5a: Per-ID FIFO. AXI4 allows multi-outstanding bursts with the same
// ID provided responses come back in issue order. The slave now keys
// active_writes_ / active_reads_ as map<id, deque<state>>, so stacked same-id
// AWs are admitted (no exclusion) and W beats route to the oldest in-flight
// burst at that id via aw_issue_order_.
TEST(AxiSlave, SameIdMultiOutstanding_FifoOrder) {
    SCENARIO(
        "AxiSlave: 3 same-id outstanding writes admitted, W beats route per per-id FIFO order");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    for (int i = 0; i < 3; ++i) {
        axi::AwBeat aw{};
        aw.id = 5;
        aw.addr = 0x1000 + i * 0x40;
        aw.len = 0;
        aw.size = 5;
        aw.burst = axi::Burst::INCR;
        slave.push_aw(aw);
        axi::WBeat w{};
        w.data.fill(0xA0 + i);
        w.strb = 0xFFFFFFFFu;
        w.last = true;
        slave.push_w(w);
    }
    for (int t = 0; t < 5; ++t) slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 3u);
    EXPECT_EQ(mem.captured_writes[0].addr, 0x1000u);
    EXPECT_EQ(mem.captured_writes[1].addr, 0x1040u);
    EXPECT_EQ(mem.captured_writes[2].addr, 0x1080u);
    EXPECT_EQ(mem.captured_writes[0].data[0], 0xA0);
    EXPECT_EQ(mem.captured_writes[1].data[0], 0xA1);
    EXPECT_EQ(mem.captured_writes[2].data[0], 0xA2);

    // Drain B responses in submission order; each B should carry id=5.
    for (std::size_t i = 0; i < 3; ++i) {
        mem.queued_write_resps.push_back(
            axi::MemWriteResp{5, axi::Resp::OKAY, mem.captured_writes[i].tag});
    }
    for (int t = 0; t < 5; ++t) slave.tick();
    for (int i = 0; i < 3; ++i) {
        auto b = slave.pop_b();
        ASSERT_TRUE(b.has_value()) << "burst " << i;
        EXPECT_EQ(b->id, 5);
        EXPECT_EQ(b->resp, axi::Resp::OKAY);
    }
}

// Phase B-4: FIXED burst — every beat targets the same address (AXI4 IHI 0022
// B1.4.3). Memory sees N writes at addr; last-beat-wins semantics emerge
// from sequential storage updates.
TEST(AxiSlave, FixedBurstAllBeatsSameAddr) {
    SCENARIO("AxiSlave FIXED: every beat targets the same address (no increment, last-beat-wins)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);
    axi::AwBeat aw{};
    aw.id = 6;
    aw.addr = 0x1000;
    aw.len = 3;
    aw.size = 5;
    aw.burst = axi::Burst::FIXED;
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 4; ++i) {
        axi::WBeat w{};
        w.data.fill(0x10 + i);
        w.strb = 0xFFFFFFFFu;
        w.last = (i == 3);
        slave.push_w(w);
    }
    for (int t = 0; t < 8; ++t) slave.tick();
    ASSERT_EQ(mem.captured_writes.size(), 4u);
    for (auto& cw : mem.captured_writes) {
        EXPECT_EQ(cw.addr, 0x1000u);
    }
}

// ===========================================================================
// Phase C — AxiSlave exclusive monitor (IHI 0022 §A7.2.4)
// ===========================================================================

namespace {
// Push an exclusive AR (lock=1) with the canonical 1-beat / size=5 / INCR
// shape used by most monitor tests. Returns the constructed beat for the
// caller to inspect if needed.
inline axi::ArBeat push_exclusive_ar(axi::AxiSlave& slave, uint8_t id, uint64_t addr,
                                     uint8_t len = 0, uint8_t size = 5,
                                     axi::Burst burst = axi::Burst::INCR, uint8_t cache = 0,
                                     uint8_t prot = 0) {
    axi::ArBeat ar{};
    ar.id = id;
    ar.addr = addr;
    ar.len = len;
    ar.size = size;
    ar.burst = burst;
    ar.cache = cache;
    ar.prot = prot;
    ar.lock = 1;
    slave.push_ar(ar);
    return ar;
}

// Push an exclusive AW + its W beats. STRB is computed per beat from the
// (1<<size) byte-lane window starting at (per-beat-addr & (DATA_BYTES-1))
// to satisfy STRB_SPARSE_LEGAL. Caller must set memory bounds before calling.
inline void push_exclusive_aw_and_w(axi::AxiSlave& slave, uint8_t id, uint64_t addr,
                                    uint8_t len = 0, uint8_t size = 5,
                                    axi::Burst burst = axi::Burst::INCR, uint8_t cache = 0,
                                    uint8_t prot = 0, uint8_t lock = 1) {
    axi::AwBeat aw{};
    aw.id = id;
    aw.addr = addr;
    aw.len = len;
    aw.size = size;
    aw.burst = burst;
    aw.cache = cache;
    aw.prot = prot;
    aw.lock = lock;
    slave.push_aw(aw);
    const std::size_t bpb = 1ull << size;
    for (uint8_t i = 0; i <= len; ++i) {
        const uint64_t beat_a = axi::beat_addr(addr, len, size, burst, i);
        const std::size_t byte_lane = static_cast<std::size_t>(beat_a & (axi::DATA_BYTES - 1));
        const uint32_t strb = static_cast<uint32_t>(((1ull << bpb) - 1ull) << byte_lane);
        axi::WBeat w{};
        w.data.fill(0xE0 + i);
        w.strb = strb;
        w.last = (i == len);
        slave.push_w(w);
    }
}

// Drain an N-beat exclusive AR's R responses so the tag becomes ready (E5).
// Returns after RLAST so the caller can immediately push a matching AW.
inline void drain_exclusive_read(test::MockMemoryPort& mem, axi::AxiSlave& slave, uint8_t id,
                                 std::size_t beat_count) {
    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_GE(mem.captured_reads.size(), beat_count);
    for (std::size_t i = 0; i < beat_count; ++i) {
        axi::MemReadResp rresp{};
        rresp.id = id;
        rresp.data.fill(0xAA);
        rresp.resp = axi::Resp::OKAY;
        rresp.last = (i + 1 == beat_count);
        rresp.tag = mem.captured_reads[i].tag;
        mem.queued_read_resps.push_back(rresp);
    }
    for (int t = 0; t < 4; ++t) slave.tick();
    while (auto rb = slave.pop_r()) {
        (void)rb;
    }
}
}  // namespace

TEST(AxiSlaveExclusive, ExclusiveAR_SetsTag_NotReady) {
    SCENARIO("AxiSlave exclusive: AR.lock=1 records tag with addr range, ready=false until RLAST");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/7, /*addr=*/0x1040);
    slave.tick();

    EXPECT_TRUE(slave.has_exclusive_tag(7));
    EXPECT_FALSE(slave.exclusive_tag_ready(7));
    auto tag = slave.peek_exclusive_tag(7);
    EXPECT_EQ(tag.addr_start, 0x1040u);
    EXPECT_EQ(tag.addr_end, 0x1060u);  // 1 beat × 32B = 32B window
}

TEST(AxiSlaveExclusive, ExclusiveAR_RComplete_TagBecomesReady) {
    SCENARIO("AxiSlave exclusive: tag.ready flips to true after exclusive AR's RLAST drains");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/3, /*addr=*/0x1000);
    drain_exclusive_read(mem, slave, /*id=*/3, /*beat_count=*/1);

    EXPECT_TRUE(slave.has_exclusive_tag(3));
    EXPECT_TRUE(slave.exclusive_tag_ready(3));
}

TEST(AxiSlaveExclusive, NormalWrite_NoOverlap_TagSurvives) {
    SCENARIO("AxiSlave exclusive: normal AW outside tag window does not invalidate the tag");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/4, /*addr=*/0x1000);
    drain_exclusive_read(mem, slave, /*id=*/4, 1);

    // Normal AW at 0x1100 — outside tag's [0x1000, 0x1020) window.
    axi::AwBeat aw{};
    aw.id = 9;
    aw.addr = 0x1100;
    aw.len = 0;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    aw.lock = 0;
    slave.push_aw(aw);
    axi::WBeat w{};
    w.data.fill(0xC0);
    w.strb = 0xFFFFFFFFu;
    w.last = true;
    slave.push_w(w);
    slave.tick();

    EXPECT_TRUE(slave.has_exclusive_tag(4))
        << "non-overlapping normal AW must not invalidate the tag";
    EXPECT_TRUE(slave.exclusive_tag_ready(4));
}

TEST(AxiSlaveExclusive, NormalWrite_Overlap_TagCleared) {
    SCENARIO("AxiSlave exclusive: normal AW overlapping tag window invalidates the tag (§A7.2.4)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/4, /*addr=*/0x1000);
    drain_exclusive_read(mem, slave, /*id=*/4, 1);

    // Normal AW at 0x1010 (mid-tag, overlaps [0x1000, 0x1020)).
    axi::AwBeat aw{};
    aw.id = 9;
    aw.addr = 0x1010;
    aw.len = 0;
    aw.size = 4;  // 16-byte beat
    aw.burst = axi::Burst::INCR;
    aw.lock = 0;
    slave.push_aw(aw);
    axi::WBeat w{};
    w.data.fill(0xC0);
    w.strb = 0x0000FFFFu << 16;
    w.last = true;
    slave.push_w(w);
    slave.tick();

    EXPECT_FALSE(slave.has_exclusive_tag(4)) << "overlapping normal AW must invalidate the tag";
}

TEST(AxiSlaveExclusive, ExclusivePair_FullMatch_EXOKAY_CommitsMemory) {
    SCENARIO(
        "AxiSlave exclusive: AR+AW pair with full attribute match → EXOKAY B, memory committed");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/5, /*addr=*/0x1000);
    drain_exclusive_read(mem, slave, /*id=*/5, 1);

    // Matching exclusive AW: same id, addr, len, size, burst, cache, prot.
    push_exclusive_aw_and_w(slave, /*id=*/5, /*addr=*/0x1000);
    slave.tick();

    // Memory should see the write (commit). Slave should report EXOKAY in B.
    ASSERT_EQ(mem.captured_writes.size(), 1u);
    EXPECT_EQ(mem.captured_writes[0].addr, 0x1000u);
    EXPECT_FALSE(slave.has_exclusive_tag(5)) << "exclusive AW must erase the tag (success path)";

    mem.queued_write_resps.push_back(
        axi::MemWriteResp{5, axi::Resp::OKAY, mem.captured_writes[0].tag});
    slave.tick();
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::EXOKAY);
}

TEST(AxiSlaveExclusive, ExclusiveWrite_NoPriorRead_OKAY_NoCommit) {
    SCENARIO(
        "AxiSlave exclusive: exclusive AW with no prior exclusive AR → OKAY, no memory commit");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // No prior exclusive AR — tag absent.
    push_exclusive_aw_and_w(slave, /*id=*/6, /*addr=*/0x1000);
    slave.tick();

    EXPECT_EQ(mem.captured_writes.size(), 0u) << "failed exclusive must NOT submit to memory";
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::OKAY) << "failed exclusive returns OKAY (not EXOKAY)";
}

TEST(AxiSlaveExclusive, ExclusiveWrite_BeforeReady_OKAY_NoCommit) {
    SCENARIO("AxiSlave exclusive: exclusive AW before tag.ready=true → OKAY, no commit");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // Exclusive AR admitted but R has NOT drained yet → tag.ready=false.
    push_exclusive_ar(slave, /*id=*/8, /*addr=*/0x1000);
    slave.tick();
    EXPECT_TRUE(slave.has_exclusive_tag(8));
    EXPECT_FALSE(slave.exclusive_tag_ready(8));

    push_exclusive_aw_and_w(slave, /*id=*/8, /*addr=*/0x1000);
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 0u);
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::OKAY);
}

TEST(AxiSlaveExclusive, ExclusiveWrite_SizeMismatch_OKAY_NoCommit) {
    SCENARIO(
        "AxiSlave exclusive: AW size != tag.size → attribute mismatch, OKAY, tag still consumed");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // AR: 1 beat, size=5 (32B), addr=0x1000.
    push_exclusive_ar(slave, /*id=*/2, /*addr=*/0x1000, /*len=*/0, /*size=*/5);
    drain_exclusive_read(mem, slave, /*id=*/2, 1);

    // AW: 1 beat, size=4 (16B), addr=0x1000 — size mismatches the tag.
    push_exclusive_aw_and_w(slave, /*id=*/2, /*addr=*/0x1000, /*len=*/0,
                            /*size=*/4);
    slave.tick();
    EXPECT_EQ(mem.captured_writes.size(), 0u);
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::OKAY) << "attribute mismatch → OKAY, not EXOKAY";
    EXPECT_FALSE(slave.has_exclusive_tag(2)) << "exclusive AW consumes the tag regardless of match";
}

TEST(AxiSlaveExclusive, ExclusiveWriteOnOob_DECERR) {
    SCENARIO("AxiSlave exclusive: OOB pre-check trumps exclusive resp → DECERR (not OKAY/EXOKAY)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x100);  // 256 bytes

    // Exclusive AW outside bounds. The OOB pre-check runs BEFORE the exclusive
    // logic and emits DECERR — exclusive resp priority puts memory errors above
    // EXOKAY/OKAY. No tag exists, so no tag bookkeeping fires either.
    // 2-beat exclusive burst at upper bound — 1-beat (32B) would still fit at
    // 0x10E0..0x1100 because bounds end at 0x1100 (strict ">" check); the 2nd
    // beat at 0x1100..0x111F pushes the burst to 0x1120 > 0x1100 → DECERR.
    axi::AwBeat aw{};
    aw.id = 13;
    aw.addr = 0x10E0;
    aw.size = 5;
    aw.burst = axi::Burst::INCR;
    aw.lock = 1;
    aw.len = 1;  // 2 beats
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 2; ++i) {
        axi::WBeat w{};
        w.data.fill(0);
        w.strb = 0xFFFFFFFFu;
        w.last = (i == 1);
        slave.push_w(w);
    }
    slave.tick();

    EXPECT_EQ(mem.captured_writes.size(), 0u);
    auto b = slave.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->resp, axi::Resp::DECERR) << "OOB pre-check trumps exclusive resp priority";
}

TEST(AxiSlaveExclusive, ExclusiveWRAP_TagRangeIsWrapWindow) {
    SCENARIO(
        "AxiSlave exclusive WRAP: tag's addr_range equals the wrap window, burst field stored");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // EXCLUSIVE_ALIGN requires addr aligned to total burst bytes. For
    // len=3 size=5 → total=128, the only valid bases in [0x1000, 0x2000) are
    // multiples of 128. Pick 0x1080 → wrap_lower=0x1080, wrap_upper=0x1100.
    // The tag's [addr_start, addr_end) window matches the WRAP window; the
    // BURST type field also rides on the tag for the §A7.2.4 attribute match.
    push_exclusive_ar(slave, /*id=*/11, /*addr=*/0x1080, /*len=*/3,
                      /*size=*/5, axi::Burst::WRAP);
    slave.tick();
    ASSERT_TRUE(slave.has_exclusive_tag(11));
    auto tag = slave.peek_exclusive_tag(11);
    EXPECT_EQ(tag.addr_start, 0x1080u);
    EXPECT_EQ(tag.addr_end, 0x1100u);
    EXPECT_EQ(tag.burst, axi::Burst::WRAP);
}

TEST(AxiSlaveExclusive, MultiId_NormalWriteErasesMultipleTags_IteratorSafe) {
    SCENARIO(
        "AxiSlave exclusive: normal AW overlapping multiple tags erases each safely "
        "(iterator-safe)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // Three exclusive ARs at adjacent addresses — all tags overlap a single
    // normal AW at 0x1000 size=2 burst=INCR len=15 (16 × 4B = 64B).
    push_exclusive_ar(slave, /*id=*/1, /*addr=*/0x1000);  // [0x1000, 0x1020)
    push_exclusive_ar(slave, /*id=*/2, /*addr=*/0x1020);  // [0x1020, 0x1040)
    push_exclusive_ar(slave, /*id=*/3, /*addr=*/0x1080);  // [0x1080, 0x10A0)
    slave.tick();
    EXPECT_TRUE(slave.has_exclusive_tag(1));
    EXPECT_TRUE(slave.has_exclusive_tag(2));
    EXPECT_TRUE(slave.has_exclusive_tag(3));

    // Normal AW with addr range [0x1000, 0x1040) — overlaps id 1 and 2, not 3.
    // 16 beats × 4B = 64B. Per-beat byte_lane rolls 0,4,8,...,28,0,... so
    // STRB must track it to satisfy STRB_SPARSE_LEGAL.
    axi::AwBeat aw{};
    aw.id = 9;
    aw.addr = 0x1000;
    aw.len = 15;
    aw.size = 2;
    aw.burst = axi::Burst::INCR;
    aw.lock = 0;
    slave.push_aw(aw);
    for (uint8_t i = 0; i < 16; ++i) {
        const uint64_t ba = axi::beat_addr(0x1000, 15, 2, axi::Burst::INCR, i);
        const std::size_t bl = static_cast<std::size_t>(ba & (axi::DATA_BYTES - 1));
        axi::WBeat w{};
        w.data.fill(0xC0);
        w.strb = static_cast<uint32_t>(0xFu << bl);
        w.last = (i == 15);
        slave.push_w(w);
    }
    // Multiple ticks because the slave forwards 1 W beat per tick to memory
    // (one burst, 16 beats).
    for (int t = 0; t < 20; ++t) slave.tick();

    EXPECT_FALSE(slave.has_exclusive_tag(1));
    EXPECT_FALSE(slave.has_exclusive_tag(2));
    EXPECT_TRUE(slave.has_exclusive_tag(3)) << "tag outside the AW window must survive";
}

TEST(AxiSlaveExclusive, ExclusiveAR_SameId_SecondOverwritesFirst) {
    SCENARIO(
        "AxiSlave exclusive: 2nd exclusive AR same id overwrites prior tag (§A7.2.3 one-per-id)");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    push_exclusive_ar(slave, /*id=*/5, /*addr=*/0x1000);
    drain_exclusive_read(mem, slave, /*id=*/5, 1);
    EXPECT_TRUE(slave.exclusive_tag_ready(5));

    // Second exclusive AR at a different address — must replace the previous
    // tag (per §A7.2.3: one outstanding exclusive read per ID).
    push_exclusive_ar(slave, /*id=*/5, /*addr=*/0x1080);
    slave.tick();
    ASSERT_TRUE(slave.has_exclusive_tag(5));
    auto tag = slave.peek_exclusive_tag(5);
    EXPECT_EQ(tag.addr_start, 0x1080u);
    EXPECT_FALSE(tag.ready) << "the new tag starts not-ready";
}

// Audit fix D3-2 regression: when a 2nd exclusive AR with the same ID arrives
// while the 1st AR's R is still in flight, the 1st AR's RLAST must NOT promote
// the new tag to ready. The new tag should only become ready after its OWN
// RLAST. pending_rlasts accounts for in-flight RLASTs ahead of the new tag.
TEST(AxiSlaveExclusive, ExclusiveAR_SameId_RaceBetweenOverwriteAndOldRLAST) {
    SCENARIO(
        "AxiSlave exclusive: new same-id tag tracks pending_rlasts so old RLAST does not promote "
        "it");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // 1st exclusive AR — admit and submit to memory, but do NOT drain R yet.
    push_exclusive_ar(slave, /*id=*/5, /*addr=*/0x1000);
    slave.tick();
    ASSERT_TRUE(slave.has_exclusive_tag(5));
    // Tag's own AR is the only one in flight → pending_rlasts == 1.
    EXPECT_EQ(slave.exclusive_tag_pending_rlasts(5), 1u);
    EXPECT_FALSE(slave.exclusive_tag_ready(5));
    ASSERT_EQ(mem.captured_reads.size(), 1u);

    // 2nd exclusive AR (same id, different addr) admitted BEFORE 1st R completes.
    // The 1st AR is still queued in active_reads_[5], so E1 must set the NEW
    // tag's pending_rlasts to (1 in-flight + 1 self) = 2.
    push_exclusive_ar(slave, /*id=*/5, /*addr=*/0x1080);
    slave.tick();
    ASSERT_TRUE(slave.has_exclusive_tag(5));
    auto tag_after_overwrite = slave.peek_exclusive_tag(5);
    EXPECT_EQ(tag_after_overwrite.addr_start, 0x1080u)
        << "second exclusive AR must overwrite the per-id tag (§A7.2.3)";
    EXPECT_FALSE(tag_after_overwrite.ready);
    EXPECT_EQ(slave.exclusive_tag_pending_rlasts(5), 2u)
        << "new tag must wait for both the in-flight 1st RLAST and its own RLAST";

    // 2nd AR's memory submit — both reads are now queued.
    ASSERT_GE(mem.captured_reads.size(), 2u);

    // Push memory R responses: 1st AR's RLAST FIRST.
    axi::MemReadResp r1{};
    r1.id = 5;
    r1.data.fill(0xA0);
    r1.resp = axi::Resp::OKAY;
    r1.last = true;
    r1.tag = mem.captured_reads[0].tag;
    mem.queued_read_resps.push_back(r1);
    slave.tick();
    while (auto rb = slave.pop_r()) {
        (void)rb;
    }

    // After the OLD AR's RLAST: pending_rlasts must decrement to 1, NOT promote
    // the new tag to ready. This is the core race the fix addresses — pre-fix
    // code unconditionally set ready=true here.
    ASSERT_TRUE(slave.has_exclusive_tag(5));
    EXPECT_EQ(slave.exclusive_tag_pending_rlasts(5), 1u);
    EXPECT_FALSE(slave.exclusive_tag_ready(5))
        << "1st AR's RLAST must not promote the 2nd AR's tag (bug pre-fix)";

    // Push 2nd AR's RLAST — only NOW should the tag become ready.
    axi::MemReadResp r2{};
    r2.id = 5;
    r2.data.fill(0xB0);
    r2.resp = axi::Resp::OKAY;
    r2.last = true;
    r2.tag = mem.captured_reads[1].tag;
    mem.queued_read_resps.push_back(r2);
    slave.tick();
    while (auto rb = slave.pop_r()) {
        (void)rb;
    }

    EXPECT_EQ(slave.exclusive_tag_pending_rlasts(5), 0u);
    EXPECT_TRUE(slave.exclusive_tag_ready(5))
        << "tag ready only after its OWN AR's RLAST has arrived";
}

TEST(AxiSlaveExclusive, DifferentId_ExclusiveAW_DoesNotAffectOtherTag) {
    SCENARIO("AxiSlave exclusive: exclusive AW on id=1 consumes id=1 tag only, id=2 tag survives");
    test::MockMemoryPort mem;
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // Two exclusive ARs at different ids; drain both so both tags ready.
    push_exclusive_ar(slave, /*id=*/1, /*addr=*/0x1000);
    push_exclusive_ar(slave, /*id=*/2, /*addr=*/0x1100);
    for (int t = 0; t < 4; ++t) slave.tick();
    ASSERT_GE(mem.captured_reads.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        axi::MemReadResp rresp{};
        rresp.id = mem.captured_reads[i].id;
        rresp.data.fill(0xAA);
        rresp.resp = axi::Resp::OKAY;
        rresp.last = true;
        rresp.tag = mem.captured_reads[i].tag;
        mem.queued_read_resps.push_back(rresp);
    }
    for (int t = 0; t < 4; ++t) slave.tick();
    while (auto rb = slave.pop_r()) {
        (void)rb;
    }
    EXPECT_TRUE(slave.exclusive_tag_ready(1));
    EXPECT_TRUE(slave.exclusive_tag_ready(2));

    // Exclusive AW on id=1 consumes only id=1's tag.
    push_exclusive_aw_and_w(slave, /*id=*/1, /*addr=*/0x1000);
    slave.tick();
    EXPECT_FALSE(slave.has_exclusive_tag(1));
    EXPECT_TRUE(slave.has_exclusive_tag(2))
        << "exclusive AW on id=1 must leave id=2's tag untouched";
}
