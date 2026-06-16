#include "cosim/router_shell_adapter.hpp"
#include "cosim/flit_byte_conv.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using namespace ni::cmodel::cosim;

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
TEST(RouterShellAdapter, NmuReqRoutesToLinkOut) {
    RouterShellAdapter a;
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
TEST(RouterShellAdapter, LinkInReqEjectsAtNsu) {
    RouterShellAdapter a;
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
TEST(RouterShellAdapter, LinkInRspEjectsAtNmu) {
    RouterShellAdapter a;
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
TEST(RouterShellAdapter, LinkInputDrainEmitsCreditPulse) {
    RouterShellAdapter a;
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

// Node1 (x=1): a request to dst=(0,0) routes WEST = its LINK port.
TEST(RouterShellAdapter, Node1NmuReqRoutesToLinkOut) {
    RouterShellAdapter a;
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
