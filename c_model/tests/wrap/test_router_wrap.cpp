#include "wrap/router_wrap.hpp"
#include "wrap/flit_byte_conv.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using namespace ni::cmodel::wrap;

namespace {

// Build a request flit destined for dst_id, vc 0.
ni::cmodel::Flit make_req(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AR);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    return f;
}

ni::cmodel::Flit make_rsp(uint8_t dst_id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    return f;
}

}  // namespace

// Node0 NMU injects a request to dst=(1,0). route_compute sends it EAST = the
// LINK port, so it must appear on link_req_out (toward the neighbor).
TEST(RouterWrap, NmuReqRoutesToLinkOut) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.req_in_valid = true;
    in.req_in_flit = flit_to_bytes(make_req(/*dst=*/0x01));
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    bool seen = false;
    for (int cyc = 0; cyc < 16 && !seen; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.link_req_out_valid) {
            seen = true;
            auto f = flit_from_bytes(out.link_req_out_flit);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x01u);
        }
    }
    EXPECT_TRUE(seen) << "node0 NMU request never appeared on link_req_out";
}

// A REQ flit arriving on the LINK input destined LOCAL (0,0) must eject at the
// node0 NSU-facing req_out.
TEST(RouterWrap, LinkInReqEjectsAtNsu) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.link_req_in_valid = true;
    in.link_req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.req_out_valid) {
            ejected = true;
            auto f = flit_from_bytes(out.req_out_flit);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x00u);
        }
    }
    EXPECT_TRUE(ejected) << "LINK-in request never ejected at NSU req_out";
}

// Symmetric RSP path: a RSP flit on the LINK input destined LOCAL must eject at
// the node0 NMU-facing rsp_out.
TEST(RouterWrap, LinkInRspEjectsAtNmu) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.link_rsp_in_valid = true;
    in.link_rsp_in_flit = flit_to_bytes(make_rsp(/*dst=*/0x00));
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.rsp_out_valid) ejected = true;
    }
    EXPECT_TRUE(ejected) << "LINK-in response never ejected at NMU rsp_out";
}

// LinkCreditOut pulse: when a LINK-input flit drains out of the router input
// FIFO (grant), the router emits a registered input-drain credit pulse; the
// shell must surface it as a single-cycle assert on link_req_in_credit.
TEST(RouterWrap, LinkInputDrainEmitsCreditPulse) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.link_req_in_valid = true;
    in.link_req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));  // LOCAL-bound
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    int pulses = 0;
    for (int cyc = 0; cyc < 16; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.link_req_in_credit) ++pulses;
    }
    // Exactly one flit drained from the LINK input FIFO -> exactly one credit pulse.
    EXPECT_EQ(pulses, 1) << "expected exactly one LINK-input drain credit pulse";
}

// R2: the LOCAL (NI-edge) credit is now a single-cycle PULSE, identical to the
// LINK. When the router's LOCAL input FIFO drains an NMU-injected req flit
// (grant), the shell must surface exactly one req_out_credit_return pulse — NOT
// a steady credit_avail level. Inject one LOCAL-bound req at node0 (dst=(0,0)
// routes LOCAL, ejecting back at this node's NSU-facing req_out) and count the
// returned credit pulses.
TEST(RouterWrap, LocalInputDrainEmitsCreditPulse) {
    RouterWrap a;
    a.init(/*x_coord=*/0);
    RouterInputs in{};
    in.req_in_valid = true;
    in.req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));  // LOCAL-bound
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    int pulses = 0;
    for (int cyc = 0; cyc < 16; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.req_out_credit_return) ++pulses;
    }
    // Exactly one flit drained from the LOCAL input FIFO -> exactly one pulse.
    EXPECT_EQ(pulses, 1) << "expected exactly one LOCAL-input drain credit pulse";
}

// R2: the router's built-in credit_[LOCAL] sender counter (router->NI output
// direction) is replenished by the NI's returned pulse on req_in_credit_return.
// Eject one LOCAL-bound req first (which spends one credit_[LOCAL] on the router
// output), THEN return the credit pulse; router.receive_credit(LOCAL) must
// restore the counter. This test is a genuine discriminator: it reads the
// router's credit_[LOCAL] before/after the return hop. A dropped hop (the shell
// failing to call receive_credit(LOCAL) on req_in_credit_return) leaves the
// counter one short and FAILS the final assertion — a no-abort-only test would
// not catch that.
TEST(RouterWrap, LocalInCreditReturnReplenishesRouter) {
    constexpr std::size_t LOCAL = static_cast<std::size_t>(ni::cmodel::router::RouterPort::LOCAL);

    RouterWrap a;
    a.init(/*x_coord=*/0);
    const std::size_t seed = a.req_router().credit(LOCAL, /*vc=*/0);
    ASSERT_GT(seed, 0u) << "router LOCAL output credit must seed > 0";

    RouterInputs in{};
    in.req_in_valid = true;
    in.req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));  // LOCAL-bound
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    // Let the flit eject at req_out. The grant that moves it into the LOCAL
    // output FIFO spends exactly one credit_[LOCAL] (router.hpp stage 2).
    RouterOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.req_out_valid) ejected = true;
    }
    ASSERT_TRUE(ejected);
    ASSERT_EQ(a.req_router().credit(LOCAL, /*vc=*/0), seed - 1)
        << "ejecting one LOCAL flit must spend exactly one credit_[LOCAL]";

    // Now the NSU returns the consumer credit pulse. The shell must route it to
    // router.receive_credit(LOCAL, 0), incrementing credit_[LOCAL] back to seed.
    // If the hop were dropped the counter would stay at seed-1 and this fails.
    in = RouterInputs{};
    in.req_in_credit_return = true;
    a.set_inputs(in);
    a.tick();
    a.get_outputs(out);
    EXPECT_FALSE(out.req_out_valid) << "credit-return pulse must not surface as a flit";
    EXPECT_EQ(a.req_router().credit(LOCAL, /*vc=*/0), seed)
        << "req_in_credit_return must replenish credit_[LOCAL] back to its seed";
}

// Node1 (x=1): a request to dst=(0,0) routes WEST = its LINK port.
TEST(RouterWrap, Node1NmuReqRoutesToLinkOut) {
    RouterWrap a;
    a.init(/*x_coord=*/1);
    RouterInputs in{};
    in.req_in_valid = true;
    in.req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterInputs{});

    RouterOutputs out{};
    bool seen = false;
    for (int cyc = 0; cyc < 16 && !seen; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.link_req_out_valid) seen = true;
    }
    EXPECT_TRUE(seen) << "node1 NMU request never appeared on link_req_out";
}
