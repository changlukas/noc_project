// Regression: the NMU request bridge must NOT head-of-line-block W/AR behind a
// full AW wormhole input. Reproduces the 8R/8W co-sim self-deadlock
// deterministically at the bridge+packetize+wormhole level.
#include "nmu/nmu.hpp"
#include "nmu/packetize.hpp"
#include "ni/wormhole_arbiter.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::nmu::AwHeaderMeta;
using ni::cmodel::nmu::NmuReqS1Bridge;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::router::ChannelPairing;
using ni::cmodel::router::NocReqOut;
using ni::cmodel::router::WormholeArbiter;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x12;
constexpr uint8_t kDst = 0x03;
constexpr std::size_t kAwInputDepth = 4;

axi::AwBeat make_aw(uint8_t id) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = 0x1000;
    b.len = 0;
    b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::WBeat make_w(bool last) {
    axi::WBeat b{};
    for (int i = 0; i < 32; ++i) b.data[i] = static_cast<uint8_t>(i);
    b.strb = 0xFFFFFFFF;
    b.last = last;
    return b;
}
axi::ArBeat make_ar(uint8_t id) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = 0x2000;
    b.len = 0;
    b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
AwHeaderMeta meta() {
    return AwHeaderMeta{kDst, 0x1000, 0, 0};
}
}  // namespace

// Wires the request sub-assembly as Nmu does: Packetize -> WormholeArbiter
// (3 inputs, AW->W pairing {0,1}, per-input depth 4) -> ReqCapture; the bridge
// feeds Packetize. step() advances the bridge THEN the arbiter (drain then
// grant). This is the opposite local order from Nmu::tick (which ticks the
// arbiter before the bridge). It is intentional: combining enqueue and grant in
// one helper step keeps the isolated regression deterministic. The deadlock
// under test is order-independent (a held lock never releases either way).
TEST(NmuReqBridge, WAndArDrainDespiteFullAwInput) {
    SCENARIO(
        "Bridge must drain W (for the locked write) and admit AR even when the "
        "wormhole AW-input is full and a later AW is stuck in the bridge. Pre-fix "
        "this self-deadlocks (line-78 HOL); post-fix W/AR flow.");
    ReqCapture out;
    WormholeArbiter<NocReqOut> wh(out, /*num_inputs=*/3, std::vector<ChannelPairing>{{0, 1}},
                                  kAwInputDepth);
    // Bridge always drives push_*_with_meta (never push_aw/push_ar), so the
    // direct-path interface's SamTable is never touched here — default is fine.
    Packetize pkt(wh.input(0), wh.input(1), wh.input(2), kSrcId, {});
    NmuReqS1Bridge bridge;

    auto step = [&] {
        bridge.tick(pkt);
        wh.tick();
    };

    // AW0 flows through and locks the arbiter to the W input (pairing {0,1}).
    ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(0), meta()));
    step();
    EXPECT_TRUE(wh.is_locked()) << "AW0 (flit_tail=0) must lock the arbiter to its W";

    // AW1..AW4 accumulate in the wormhole AW input (locked -> not granted),
    // filling it to depth 4.
    for (uint8_t id = 1; id <= kAwInputDepth; ++id) {
        ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(id), meta()));
        step();
    }
    EXPECT_EQ(wh.pending_size(0), kAwInputDepth) << "AW input full";

    // A further AW is stuck in the bridge slot (wormhole AW input full).
    ASSERT_TRUE(bridge.push_aw_with_meta(make_aw(99), meta()));
    step();
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_NarrowAw), 1u) << "5th AW stuck in bridge";

    // The locked write's W and an independent AR are now offered.
    ASSERT_TRUE(bridge.push_w(make_w(/*last=*/true)));
    ASSERT_TRUE(bridge.push_ar_with_meta(make_ar(9), meta()));
    for (int i = 0; i < 6; ++i) step();

    bool got_w = false;
    while (auto f = out.pop())
        if (f->get_header_field("axi_ch") == static_cast<uint64_t>(ni::AXI_CH_NarrowW))
            got_w = true;
    EXPECT_TRUE(got_w) << "W must drain despite full AW input (pre-fix deadlocks here)";
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_NarrowW), 0u) << "W left the bridge";
    EXPECT_EQ(bridge.occupancy(ni::AXI_CH_NarrowAr), 0u) << "AR admitted independently of stuck AW";
}

// push_w with no prior push_aw must backpressure (return false), not assert, so
// a W whose AW is not yet admitted waits without aborting or blocking AW/AR.
TEST(NmuReqBridge, PushWBackpressuresOnEmptyMeta) {
    SCENARIO("Packetize::push_w returns false when w_meta_fifo_ is empty.");
    ReqCapture aw_out, w_out, ar_out;
    Packetize pkt(aw_out, w_out, ar_out, kSrcId, {});  // push_w never touches sam_
    EXPECT_FALSE(pkt.push_w(make_w(/*last=*/true)))
        << "W with no admitted AW must backpressure, not abort";
}
