// RouterWrap unit tests (S3a T5): three physical networks, one uniform
// per-port array shape (LOCAL + N/E/S/W). REQ/RSP (SimpleRouter) are
// ready/valid -- a flit only transfers on a cycle where the downstream side
// asserts ready; DAT (Router) keeps the pre-S3a credit mechanism, now wired
// uniformly to LOCAL too instead of LOCAL-special-cased + LINK-looped.
//
// Pattern: present a flit for exactly ONE tick then clear valid (mirrors the
// pre-S3a tests) -- ready/valid does not require holding valid steady here
// since these tests assert readiness for the whole poll window, so the
// single presented flit is granted well before any retry would be needed.
// Holding valid across the whole poll loop would re-inject a new flit every
// cycle once the previous one drains, corrupting pulse/occupancy counts.
#include "wrap/router_wrap.hpp"
#include "wrap/flit_byte_conv.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using namespace ni::cmodel::wrap;
using ni::cmodel::router::RouterPort;

namespace {

constexpr std::size_t LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
constexpr std::size_t NORTH = static_cast<std::size_t>(RouterPort::NORTH);
constexpr std::size_t EAST = static_cast<std::size_t>(RouterPort::EAST);
constexpr std::size_t SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);
constexpr std::size_t WEST = static_cast<std::size_t>(RouterPort::WEST);

// Build a request flit destined for dst_id, vc 0.
ni::cmodel::Flit make_req(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowAr);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    return f;
}

ni::cmodel::Flit make_rsp(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowR);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    return f;
}

ni::cmodel::Flit make_dat_r(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    return f;
}

}  // namespace

// Node0 NMU injects a request to dst=(1,0). route_compute sends it EAST = the
// live LINK direction (default mesh_x_dim=2, mesh_y_dim=1). Ready/valid: the
// neighbor must assert tx_req_ready[EAST] for the flit to actually transfer.
// tx_req_ready is a steady-state assumption for this test (poll window), so
// it is held across the whole loop -- only rx_req_valid[LOCAL] is one-shot.
TEST(RouterWrap, NmuReqRoutesToLinkOut) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.rx_req_valid[LOCAL] = true;
    in.rx_req_flit[LOCAL] = flit_to_bytes(make_req(/*dst=*/0x01));
    in.tx_req_ready[EAST] = true;  // neighbor always ready
    a.set_inputs(in);
    a.tick();
    in.rx_req_valid[LOCAL] = false;  // one-shot: stop re-presenting
    a.set_inputs(in);

    RouterOutputs out{};
    bool seen = false;
    for (int cyc = 0; cyc < 16 && !seen; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_req_valid[EAST]) {
            seen = true;
            auto f = flit_from_bytes(out.tx_req_flit[EAST]);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);
        }
    }
    EXPECT_TRUE(seen) << "node0 NMU request never appeared on tx_req[EAST]";
}

// A REQ flit arriving on the LINK input destined LOCAL (0,0) must eject at
// tx_req[LOCAL] (toward NSU). tx_req_ready[LOCAL] models NSU's readiness.
TEST(RouterWrap, LinkInReqEjectsAtNsu) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.rx_req_valid[EAST] = true;
    in.rx_req_flit[EAST] = flit_to_bytes(make_req(/*dst=*/0x00));
    in.tx_req_ready[LOCAL] = true;  // NSU always ready
    a.set_inputs(in);
    a.tick();
    in.rx_req_valid[EAST] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_req_valid[LOCAL]) {
            ejected = true;
            auto f = flit_from_bytes(out.tx_req_flit[LOCAL]);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x00u);
        }
    }
    EXPECT_TRUE(ejected) << "LINK-in request never ejected at tx_req[LOCAL]";
}

// Symmetric RSP path: a RSP flit on the LINK input destined LOCAL must eject
// at tx_rsp[LOCAL] (toward NMU).
TEST(RouterWrap, LinkInRspEjectsAtNmu) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.rx_rsp_valid[EAST] = true;
    in.rx_rsp_flit[EAST] = flit_to_bytes(make_rsp(/*dst=*/0x00));
    in.tx_rsp_ready[LOCAL] = true;  // NMU always ready
    a.set_inputs(in);
    a.tick();
    in.rx_rsp_valid[EAST] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_rsp_valid[LOCAL]) ejected = true;
    }
    EXPECT_TRUE(ejected) << "LINK-in response never ejected at tx_rsp[LOCAL]";
}

// Ready/valid backpressure: with tx_req_ready[LOCAL] held low, a LOCAL-bound
// flit must stay queued (never transfer, never lost) and no duplicate ever
// appears. Once ready is asserted, the SAME flit (not a duplicate) transfers
// exactly once.
TEST(RouterWrap, ReqStaysQueuedUntilDownstreamReady) {
    RouterWrap a;
    a.init(/*x_coord=*/0);

    // Present the flit for exactly one tick, NSU not ready.
    RouterInputs in{};
    in.rx_req_valid[EAST] = true;
    in.rx_req_flit[EAST] = flit_to_bytes(make_req(/*dst=*/0x00));  // LOCAL-bound
    in.tx_req_ready[LOCAL] = false;
    a.set_inputs(in);
    a.tick();

    // Stop re-presenting; hold NSU not-ready for several more cycles. The
    // flit must sit in the input FIFO with no transfer.
    in = RouterInputs{};
    a.set_inputs(in);
    RouterOutputs out{};
    for (int cyc = 0; cyc < 8; ++cyc) {
        a.tick();
        a.get_outputs(out);
        EXPECT_FALSE(out.tx_req_valid[LOCAL])
            << "cycle " << cyc << ": must not transfer while not ready";
    }

    // Now the NSU asserts ready; the queued flit must transfer exactly once.
    in.tx_req_ready[LOCAL] = true;
    a.set_inputs(in);
    int transfers = 0;
    for (int cyc = 0; cyc < 8; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_req_valid[LOCAL]) ++transfers;
    }
    EXPECT_EQ(transfers, 1) << "the queued flit must transfer exactly once, no loss/duplication";
}

// Node1 (x=1): a request to dst=(0,0) routes WEST = its live LINK direction.
TEST(RouterWrap, Node1NmuReqRoutesToLinkOut) {
    RouterWrap a;
    a.init(/*x_coord=*/1);
    RouterInputs in{};
    in.rx_req_valid[LOCAL] = true;
    in.rx_req_flit[LOCAL] = flit_to_bytes(make_req(/*dst=*/0x00));
    in.tx_req_ready[WEST] = true;
    a.set_inputs(in);
    a.tick();
    in.rx_req_valid[LOCAL] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    bool seen = false;
    for (int cyc = 0; cyc < 16 && !seen; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_req_valid[WEST]) seen = true;
    }
    EXPECT_TRUE(seen) << "node1 NMU request never appeared on tx_req[WEST]";
}

// Multi-direction node: corner (1,1) of a 2x2 mesh has TWO live directions —
// WEST (to x=0) and SOUTH (to y=0); NORTH/EAST are boundary (tied off). XY DOR
// routes X first, so a (0,1)-dst flit exits WEST and a (1,0)-dst flit exits
// SOUTH. This is the genuine per-direction discriminator the single-link
// tests (init(x_coord=0)) cannot reach: it checks that each direction wires
// independently and that a flit on one direction never bleeds onto another.
TEST(RouterWrap, CornerNodeRoutesPerDirectionIndependently) {
    // dst coord ids: (0,1) = (1<<4)|0 = 0x10 routes WEST; (1,0) = 0x01 routes SOUTH.
    auto route_one = [](uint8_t dst, std::size_t expect_dir, std::size_t other_dir) {
        RouterWrap a;
        a.init(/*x=*/1, /*y=*/1, /*mesh_x=*/2, /*mesh_y=*/2);
        RouterInputs in{};
        in.rx_req_valid[LOCAL] = true;
        in.rx_req_flit[LOCAL] = flit_to_bytes(make_req(dst));
        in.tx_req_ready[WEST] = true;
        in.tx_req_ready[SOUTH] = true;
        a.set_inputs(in);
        a.tick();
        in.rx_req_valid[LOCAL] = false;
        a.set_inputs(in);

        RouterOutputs out{};
        bool on_expect = false;
        for (int cyc = 0; cyc < 16 && !on_expect; ++cyc) {
            a.tick();
            a.get_outputs(out);
            // The other live direction and BOTH tied-off directions must stay quiet
            // every cycle — no cross-direction bleed, no boundary drive.
            EXPECT_FALSE(out.tx_req_valid[other_dir]) << "flit bled onto the wrong direction";
            EXPECT_FALSE(out.tx_req_valid[NORTH]) << "boundary NORTH driven";
            EXPECT_FALSE(out.tx_req_valid[EAST]) << "boundary EAST driven";
            if (out.tx_req_valid[expect_dir]) {
                on_expect = true;
                EXPECT_EQ(flit_from_bytes(out.tx_req_flit[expect_dir]).get_header_field("dst_id"),
                          dst);
            }
        }
        EXPECT_TRUE(on_expect) << "request never appeared on the expected direction";
    };

    route_one(/*dst=*/0x10, /*expect=*/WEST, /*other=*/SOUTH);
    route_one(/*dst=*/0x01, /*expect=*/SOUTH, /*other=*/WEST);
}

// --- DAT network (Router, credit, unchanged mechanism) ---------------------
//
// DAT is the only network RouterWrap still gates on credit (S3a T5); these
// tests mirror the pre-S3a REQ/RSP credit tests, retargeted at dat_router().

// LOCAL input drain: the LOCAL input FIFO draining a NMU-injected DAT flit
// (grant) must surface exactly one rx_dat_crdvalid[LOCAL] pulse — not a
// steady level.
TEST(RouterWrap, DatLocalInputDrainEmitsCreditPulse) {
    RouterWrap a;
    a.init(/*x=*/0, /*y=*/0, /*mesh_x=*/2, /*mesh_y=*/1, /*dat_num_vc=*/1);
    RouterInputs in{};
    in.rx_dat_valid[LOCAL] = true;
    in.rx_dat_flit[LOCAL] = flit_to_bytes(make_dat_r(/*dst=*/0x00));  // LOCAL-bound
    a.set_inputs(in);
    a.tick();
    in.rx_dat_valid[LOCAL] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    int pulses = 0;
    for (int cyc = 0; cyc < 16; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.rx_dat_crdvalid[LOCAL][0]) ++pulses;
    }
    EXPECT_EQ(pulses, 1) << "expected exactly one LOCAL-input drain credit pulse";
}

// The router's built-in credit_[LOCAL] sender counter (router->NSU output
// direction) is replenished by the downstream's returned pulse on
// tx_dat_crdvalid[LOCAL]. Eject one LOCAL-bound flit first (spends one
// credit_[LOCAL] on the router output), THEN return the credit pulse;
// dat_router().receive_credit(LOCAL) must restore the counter.
TEST(RouterWrap, DatLocalCreditReturnReplenishesRouter) {
    RouterWrap a;
    a.init(/*x=*/0, /*y=*/0, /*mesh_x=*/2, /*mesh_y=*/1, /*dat_num_vc=*/1);
    const std::size_t seed = a.dat_router().credit(LOCAL, /*vc=*/0);
    ASSERT_GT(seed, 0u) << "router LOCAL output credit must seed > 0";

    RouterInputs in{};
    in.rx_dat_valid[LOCAL] = true;
    in.rx_dat_flit[LOCAL] = flit_to_bytes(make_dat_r(/*dst=*/0x00));  // LOCAL-bound
    a.set_inputs(in);
    a.tick();
    in.rx_dat_valid[LOCAL] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_dat_valid[LOCAL]) ejected = true;
    }
    ASSERT_TRUE(ejected);
    ASSERT_EQ(a.dat_router().credit(LOCAL, /*vc=*/0), seed - 1)
        << "ejecting one LOCAL flit must spend exactly one credit_[LOCAL]";

    in = RouterInputs{};
    in.tx_dat_crdvalid[LOCAL][0] = true;
    a.set_inputs(in);
    a.tick();
    a.get_outputs(out);
    EXPECT_FALSE(out.tx_dat_valid[LOCAL]) << "credit-return pulse must not surface as a flit";
    EXPECT_EQ(a.dat_router().credit(LOCAL, /*vc=*/0), seed)
        << "tx_dat_crdvalid[LOCAL] must replenish credit_[LOCAL] back to its seed";
}

// Multi-direction node: a credit pulse returned on one live direction must
// replenish only that direction's router credit counter, never the other
// live direction's. Corner (1,1): spend one credit on WEST (eject a
// WEST-bound flit), then return the WEST credit; SOUTH's credit must be
// untouched.
TEST(RouterWrap, DatCornerNodeCreditDoesNotCrossDirections) {
    RouterWrap a;
    a.init(/*x=*/1, /*y=*/1, /*mesh_x=*/2, /*mesh_y=*/2, /*dat_num_vc=*/1);
    const std::size_t west_seed = a.dat_router().credit(WEST, /*vc=*/0);
    const std::size_t south_seed = a.dat_router().credit(SOUTH, /*vc=*/0);
    ASSERT_GT(west_seed, 0u);
    ASSERT_GT(south_seed, 0u);

    // Eject one WEST-bound flit (dst (0,1) = 0x10): the grant spends one WEST credit.
    RouterInputs in{};
    in.rx_dat_valid[LOCAL] = true;
    in.rx_dat_flit[LOCAL] = flit_to_bytes(make_dat_r(/*dst=*/0x10));
    a.set_inputs(in);
    a.tick();
    in.rx_dat_valid[LOCAL] = false;
    a.set_inputs(in);

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.tx_dat_valid[WEST]) ejected = true;
    }
    ASSERT_TRUE(ejected);
    ASSERT_EQ(a.dat_router().credit(WEST, 0), west_seed - 1) << "WEST eject must spend one credit";
    ASSERT_EQ(a.dat_router().credit(SOUTH, 0), south_seed) << "SOUTH credit must be untouched";

    // Return the WEST credit pulse: only WEST replenishes; SOUTH stays at its seed.
    in = RouterInputs{};
    in.tx_dat_crdvalid[WEST][0] = true;
    a.set_inputs(in);
    a.tick();
    EXPECT_EQ(a.dat_router().credit(WEST, 0), west_seed)
        << "WEST credit-return must replenish WEST";
    EXPECT_EQ(a.dat_router().credit(SOUTH, 0), south_seed) << "WEST credit must not touch SOUTH";
}
