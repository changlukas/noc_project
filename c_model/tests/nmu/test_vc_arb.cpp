#include "nmu/vc_arb.hpp"
#include "nmu/packetize.hpp"
#include "axi/types.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nmu::VcArb;
using ni::cmodel::nmu::VcMode;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_flit(uint8_t axi_ch, uint8_t dst_id = 0, uint8_t initial_vc = 0,
               uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id",  initial_vc);
    f.set_header_field("src_id", 0x12);
    f.set_header_field("last",   1);  // legacy; VcArb does not consult header.last
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    }
    return f;
}

}  // namespace

TEST(NmuVcArb, Degenerate_NumVc1_AllModesPassthrough) {
    SCENARIO("NMU VcArb: NUM_VC=1, both Mode A (read_write_split) and Mode B "
             "(multi_candidate) route every axi_ch → VC=0; behavior "
             "observationally identical to direct Packetize → LoopbackNoc");

    // Mode A
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick(); arb.tick(); arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
    // Mode B — even with multi_candidate, num_vc=1 forces VC=0.
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        std::array<std::vector<uint8_t>, VcArb::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_AW] = {0};
        candidates[ni::AXI_CH_W]  = {0};
        candidates[ni::AXI_CH_AR] = {0};
        auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/1, candidates);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick(); arb.tick(); arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
}

TEST(NmuVcArb, ReadWriteSplit_AW_AR_GoSeparateVcs) {
    SCENARIO("NMU VcArb Mode A NUM_VC=2: AW → write_vc=0, AR → read_vc=1; "
             "verify pending queues + downstream stamps");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2,
                                       /*write_vc=*/0, /*read_vc=*/1);

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 1u);  // AW landed on write_vc
    EXPECT_EQ(arb.pending_size(1), 1u);  // AR landed on read_vc

    arb.tick(); arb.tick();
    auto f0 = noc.req_in().pop_flit(); ASSERT_TRUE(f0.has_value());
    auto f1 = noc.req_in().pop_flit(); ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 → VC=0 (AW) drains first, then VC=1 (AR).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f0->get_header_field("vc_id"),  0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f1->get_header_field("vc_id"),  1u);
}

TEST(NmuVcArb, MultiCandidate_HoLAvoidance) {
    SCENARIO("NMU VcArb Mode B NUM_VC=4: AW candidates {0,1}, AR candidates {2,3}; "
             "saturate VC=0 pending → next AW picks VC=1, avoiding head-of-line block");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0, 1};
    candidates[ni::AXI_CH_W]  = {0, 1};  // W follows AW via pending_w_routes_
    candidates[ni::AXI_CH_AR] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/2);

    // Fill VC=0 pending to capacity (2 AWs land on VC=0).
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next AW: VC=0 full → candidate fallback picks VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}

TEST(NmuVcArb, WFollowsAW_InvariantEnforced) {
    SCENARIO("NMU VcArb Mode B NUM_VC=4: AW1 lands VC=0, AW2 lands VC=1 (Mode B "
             "candidate fallback). All W beats of burst 1 route to VC=0, all W "
             "beats of burst 2 route to VC=1 — even though Mode B candidate list "
             "for W is {0,1}, pending_w_routes_ forces correct routing.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0, 1};
    candidates[ni::AXI_CH_W]  = {0, 1};
    candidates[ni::AXI_CH_AR] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/4);

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0
    // Saturate VC=0 with extra non-W traffic so the next AW must spill to VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0 (still room)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=0 (full now)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));  // VC=1 (overflow)
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_w_routes_size(), 5u);

    // Drain all AWs.
    for (int i = 0; i < 5; ++i) { arb.tick(); auto _ = noc.req_in().pop_flit(); }

    // Now push 2 W beats for AW1 (VC=0) and 2 W beats for AW2 to AW4 (VC=0)
    // and 1 W beat for AW5 (VC=1) — all single-beat bursts (wlast=1).
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
    }
    // First 4 W beats correspond to AWs that landed on VC=0; 5th to AW on VC=1.
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_w_routes_size(), 0u);  // all popped on wlast=1
}

TEST(NmuVcArb, WlastFromPayloadNotHeader) {
    SCENARIO("NMU VcArb: pending_w_routes_ pops based on payload.W.wlast, "
             "NOT header.last. Push AW + 3 W beats; only the 3rd has "
             "payload.wlast=1; verify deque only pops on the 3rd.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2, 0, 1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u);

    // Beat 1: payload.wlast=0 (intermediate W beat); even if header.last=1 in
    // the input flit (legacy bug shape), pending_w_routes_ MUST NOT pop.
    Flit w1; w1.set_header_field("axi_ch", ni::AXI_CH_W);
            w1.set_header_field("last", 1);   // bait: legacy bug-shape header.last
            w1.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w1));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u) << "wlast=0 → must not pop";

    Flit w2; w2.set_header_field("axi_ch", ni::AXI_CH_W);
            w2.set_header_field("last", 1);
            w2.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w2));
    EXPECT_EQ(arb.pending_w_routes_size(), 1u);

    Flit w3; w3.set_header_field("axi_ch", ni::AXI_CH_W);
            w3.set_header_field("last", 1);
            w3.set_payload_field("W", "wlast", 1);  // genuine burst end
    ASSERT_TRUE(arb.push_flit(w3));
    EXPECT_EQ(arb.pending_w_routes_size(), 0u);
}

TEST(NmuVcArb, RoundRobinFairness_AllVcsServiced_NoStarvation) {
    SCENARIO("NMU VcArb NUM_VC=4: 4 ARs pre-routed to different VCs via Mode B "
             "candidates {0,1,2,3}; tick 4 times → 4 flits emerge in "
             "round-robin order (VC0, VC1, VC2, VC3 successively).");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0};
    candidates[ni::AXI_CH_W]  = {0};
    // Each candidate list of size 1 → AR_i always picks VC=i via successive pushes.
    candidates[ni::AXI_CH_AR] = {0, 1, 2, 3};
    auto arb = VcArb::multi_candidate(noc.req_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/1);
    // Push 1 AR — picks VC=0 (first candidate with space).
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    // VC=0 now full; push AR → VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);
    EXPECT_EQ(arb.pending_size(2), 1u);
    EXPECT_EQ(arb.pending_size(3), 1u);

    arb.tick(); arb.tick(); arb.tick(); arb.tick();
    EXPECT_EQ(arb.pending_size(0), 0u);
    EXPECT_EQ(arb.pending_size(1), 0u);
    EXPECT_EQ(arb.pending_size(2), 0u);
    EXPECT_EQ(arb.pending_size(3), 0u);

    for (uint8_t expected_vc = 0; expected_vc < 4; ++expected_vc) {
        auto f = noc.req_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), expected_vc);
    }
}

TEST(NmuVcArb, CreditGating_TickIdleWhenAllVcsBlocked) {
    SCENARIO("NMU VcArb Mode A NUM_VC=1: all flits → VC=0; LoopbackNoc "
             "per_vc_depth=1 caps downstream credit. Push 4 AR flits, tick "
             "drains 1 (downstream credit consumed). Subsequent tick is idle "
             "(VC=0 has pending=3 but downstream credit_avail=false). Pop "
             "downstream → credit returns → next tick drains 1 more.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    noc.set_per_vc_depth(1);  // downstream credit = 1 for VC=0
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0,
                                       /*pending_depth=*/8);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 4u);

    // First tick: VC=0 has pending + downstream credit → 1 flit out.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u);
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Downstream credit exhausted (per_vc_depth=1) → next tick is idle.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u) << "tick must be idle, no spurious push";
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Pop downstream → credit returns → next tick drains.
    auto f = noc.req_in().pop_flit(); ASSERT_TRUE(f.has_value());
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 0u);
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 2u);
}

TEST(NmuVcArb, BackpressureChain_VcArbToUpstream) {
    SCENARIO("NMU VcArb: VcArb pending_depth=2, single VC. After 2 pushes, "
             "VcArb's own pending_[0] is full → push_flit returns false; "
             "credit_avail(0) also returns false. Backpressure visible to "
             "upstream Packetize.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0,
                                       /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_FALSE(arb.credit_avail(0));
    EXPECT_FALSE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
}

TEST(NmuVcArb, EnabledModeMixedWith_PriorRoundTests) {
    SCENARIO("NMU VcArb decorator is transparent to nmu::Packetize: wire "
             "Packetize → VcArb → LoopbackNoc with NUM_VC=1 and verify "
             "Packetize-emitted AW + W flits arrive intact at NSU side.");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::nmu::Packetize pkt(arb, /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07; aw.addr = 0x340000; aw.len = 0; aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    ni::cmodel::axi::WBeat w{};
    for (int i = 0; i < 32; ++i) w.data[i] = static_cast<uint8_t>(i);
    w.strb = 0xFFFFFFFF; w.last = true;
    ASSERT_TRUE(pkt.push_w(w));

    arb.tick();
    arb.tick();
    auto f_aw = noc.req_in().pop_flit(); ASSERT_TRUE(f_aw.has_value());
    auto f_w  = noc.req_in().pop_flit(); ASSERT_TRUE(f_w.has_value());
    EXPECT_EQ(f_aw->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f_w->get_header_field("axi_ch"),  ni::AXI_CH_W);
    EXPECT_EQ(f_aw->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(f_w->get_header_field("dst_id"),  0x34u);
}

TEST(NmuVcArb, WHeaderLastMatchesWlast) {
    SCENARIO("NMU VcArb: header.last on W flits emitted via Packetize → VcArb "
             "→ downstream matches payload.wlast (verifies §12 packetize fix "
             "is preserved end-to-end through the decorator pipeline).");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::nmu::Packetize pkt(arb, /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07; aw.addr = 0x340000; aw.len = 2; aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    auto make_w = [](bool last) {
        ni::cmodel::axi::WBeat w{};
        for (int i = 0; i < 32; ++i) w.data[i] = 0;
        w.strb = 0xFFFFFFFF; w.last = last; return w;
    };
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(true)));

    // Drain AW + 3 W flits through tick().
    for (int i = 0; i < 4; ++i) arb.tick();

    noc.req_in().pop_flit();  // discard AW
    for (int i = 0; i < 3; ++i) {
        auto f = noc.req_in().pop_flit(); ASSERT_TRUE(f.has_value());
        uint64_t expected = (i == 2) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("last"), expected);
        EXPECT_EQ(f->get_payload_field("W", "wlast"), expected);
    }
}

namespace {

class LyingDownstream : public ni::cmodel::noc::NocReqOut {
public:
    bool push_flit(const Flit&) override { return false; }
    bool credit_avail(uint8_t) const override { return true; }
};

}  // namespace

TEST(NmuVcArbDeath, WFollowsAW_WBeforeAW_DeathTest) {
    SCENARIO("NMU VcArb: push_flit(W) before any push_flit(AW) violates the "
             "W-follows-AW invariant; pending_w_routes_ is empty so VcArb "
             "must assert+abort instead of UB on front().");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    auto arb = VcArb::read_write_split(noc.req_out(), /*num_vc=*/2, 0, 1);
    EXPECT_DEATH({
        Flit w; w.set_header_field("axi_ch", ni::AXI_CH_W);
                w.set_payload_field("W", "wlast", 1);
        arb.push_flit(w);
    }, "nmu::VcArb::push_flit: W arrived with empty pending_w_routes_");
}

TEST(NmuVcArbDeath, ProtocolViolation_LyingDownstream_DeathTest) {
    SCENARIO("NMU VcArb: downstream lies — credit_avail returns true but "
             "push_flit returns false. VcArb::tick must assert+abort (the "
             "protocol guarantees credit_avail=true implies push_flit "
             "success on the next call).");
    LyingDownstream liar;
    auto arb = VcArb::read_write_split(liar, /*num_vc=*/1, 0, 0);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_DEATH({ arb.tick(); },
                 "nmu::VcArb::tick: downstream returned credit_avail=true");
}
