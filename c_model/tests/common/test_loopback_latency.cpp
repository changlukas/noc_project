// Multi-NSU LoopbackNoc unit tests + backward-compat invariant.
//
// Verifies:
//   - Multi-NSU ctor routes per-flit by dst_id via set_dst_route().
//   - Unmapped dst_id push asserts (DEATH test).
//   - Per-NSU static + random response latency (set_nsu_latency /
//     set_nsu_latency_range + set_random_seed).
//   - NSU req queue full on one NSU does not block another.
//   - Single-NSU ctor backward-compat: dst defaults route to NSU_0, legacy
//     aliases (req_in/req_out/rsp_in/rsp_out) reach NSU_0, legacy
//     set_rsp_delay still applies globally.
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni_flit_constants.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::Flit;

namespace {

Flit make_req_flit(uint8_t src, uint8_t dst, uint8_t rob_req, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AW);
    f.set_header_field("src_id", src);
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id",  0);
    f.set_header_field("last",   1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    return f;
}

Flit make_rsp_flit(uint8_t src, uint8_t dst, uint8_t rob_req, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("src_id", src);
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id",  0);
    f.set_header_field("last",   1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    return f;
}

}  // namespace

TEST(LoopbackNocMultiNsu, RouteByDstId) {
    SCENARIO("LoopbackNoc: multi-NSU ctor routes per-flit by dst_id via set_dst_route map");
    LoopbackNoc noc(/*num_nsu=*/2, /*req_per_nsu=*/16, /*rsp_total=*/16);
    noc.set_dst_route(/*dst=*/0x00, /*nsu_idx=*/0);
    noc.set_dst_route(/*dst=*/0x01, /*nsu_idx=*/1);

    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x01, 0, 0)));

    auto f0 = noc.nsu_req_in(0).pop_flit();
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->get_header_field("dst_id"), 0x00u);

    auto f1 = noc.nsu_req_in(1).pop_flit();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("dst_id"), 0x01u);

    EXPECT_FALSE(noc.nsu_req_in(0).pop_flit().has_value());
    EXPECT_FALSE(noc.nsu_req_in(1).pop_flit().has_value());
}

TEST(LoopbackNocMultiNsuDeath, UnmappedDst_Assert) {
    SCENARIO("LoopbackNoc: push of flit with unmapped dst_id asserts (no silent drop or default route)");
    LoopbackNoc noc(2, 16, 16);
    // No set_dst_route called -> all dst unmapped -> push asserts
    EXPECT_DEATH(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x99, 0, 0)), ".*");
}

TEST(LoopbackNocMultiNsu, PerNsuLatency_StaticDelay) {
    SCENARIO("LoopbackNoc: set_nsu_latency=3 holds rsp flit for 3 ticks before NMU pop_flit sees it");
    LoopbackNoc noc(2, 16, 16);
    noc.set_nsu_latency(/*nsu_idx=*/1, /*cycles=*/3);

    ASSERT_TRUE(noc.nsu_rsp_out(1).push_flit(make_rsp_flit(0x11, 0x01, 1, 0)));
    // Not visible yet
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 1st aging
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 2nd
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 3rd -> cycles_remaining hits 0, released to rsp_q
    auto f = noc.nmu_rsp_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("src_id"), 0x11u);
}

TEST(LoopbackNocMultiNsu, PerNsuLatency_RandomBounded) {
    SCENARIO("LoopbackNoc: random latency in [2,8] honored per-flit; all 100 flits release within bounds");
    LoopbackNoc noc(2, 16, /*rsp_total=*/512);
    noc.set_nsu_latency_range(/*nsu_idx=*/1, /*min=*/2, /*max=*/8);
    noc.set_random_seed(42);

    std::vector<std::size_t> release_tick(100, 0);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(noc.nsu_rsp_out(1).push_flit(make_rsp_flit(0x11, 0x01, 1, 0)));
    }
    // Each push samples a latency in [2, 8]; tick until everything drains.
    int produced = 0;
    for (int t = 1; t <= 20 && produced < 100; ++t) {
        noc.tick();
        while (true) {
            auto f = noc.nmu_rsp_in().pop_flit();
            if (!f) break;
            release_tick[produced++] = static_cast<std::size_t>(t);
        }
    }
    EXPECT_EQ(produced, 100);
    for (auto t : release_tick) {
        EXPECT_GE(t, 2u);
        EXPECT_LE(t, 8u);
    }
}

TEST(LoopbackNocMultiNsu, PerNsuQueueFull_DoesNotBlockOtherNsu) {
    SCENARIO("LoopbackNoc: NSU_0 req queue full does not block push to NSU_1 (per-NSU independent queues)");
    LoopbackNoc noc(/*num_nsu=*/2, /*req_per_nsu=*/1, /*rsp_total=*/16);
    noc.set_dst_route(0x00, 0);
    noc.set_dst_route(0x01, 1);

    // Fill NSU_0 req queue (depth 1) -- next push to dst=0 must fail
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    EXPECT_FALSE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    // NSU_1 queue still empty -- push to dst=1 must succeed
    EXPECT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x01, 0, 0)));
}

TEST(LoopbackNocBackwardCompat, SingleNsuCtor_LegacyAccessAndDelayPreserved) {
    SCENARIO("LoopbackNoc: single-NSU ctor keeps backward compat (legacy aliases + set_rsp_delay)");
    // Single-NSU ctor: dst_to_nsu_ defaults to all NSU_0
    LoopbackNoc noc(/*req_depth=*/4, /*rsp_depth=*/4);

    // Invariant 1: dst defaults route to NSU_0 (no set_dst_route needed)
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x55, 0, 0)));
    auto f = noc.nsu_req_in(0).pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("dst_id"), 0x55u);

    // Invariant 2: legacy aliases (req_in/req_out/rsp_in/rsp_out) point at
    // NSU_0 endpoints (same observable behavior). Push via alias req_out,
    // pop via alias req_in -- both go through NSU_0.
    ASSERT_TRUE(noc.req_out().push_flit(make_req_flit(0x10, 0x77, 0, 0)));
    auto f2 = noc.req_in().pop_flit();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("dst_id"), 0x77u);

    // Invariant 3: legacy set_rsp_delay still works (global delay applies)
    noc.set_rsp_delay(2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_rsp_flit(0x10, 0x01, 0, 0)));
    EXPECT_FALSE(noc.rsp_in().pop_flit().has_value());   // not visible yet
    noc.tick(); noc.tick();   // age 2 cycles
    auto f3 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->get_header_field("src_id"), 0x10u);
}
