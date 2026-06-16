#include "cosim/router_channel_shell_adapter.hpp"
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

}  // namespace

// node1.NMU injects a request to dst=(0,0); it must eject at node0.NSU.
TEST(RouterChannelShellAdapter, ReqNode1ToNode0) {
    RouterChannelShellAdapter a;
    a.init();
    RouterChannelInputs in{};
    in.node[1].req_in_valid = true;
    in.node[1].req_in_flit = flit_to_bytes(make_req(/*dst=*/0x00));
    a.set_inputs(in);
    a.tick();
    // After the injection tick, stop driving and pump until ejection.
    a.set_inputs(RouterChannelInputs{});
    RouterChannelOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.node[0].req_out_valid) {
            ejected = true;
            auto f = flit_from_bytes(out.node[0].req_out_flit);
            EXPECT_EQ(f.get_header_field("dst_id"), 0x00u);
        }
    }
    EXPECT_TRUE(ejected) << "request never ejected at node0 NSU";
}

// node0.NSU injects a response to dst=(1,0); it must eject at node1.NMU.
TEST(RouterChannelShellAdapter, RspNode0ToNode1) {
    RouterChannelShellAdapter a;
    a.init();
    ni::cmodel::Flit rsp;
    rsp.set_header_field("axi_ch", ni::AXI_CH_R);
    rsp.set_header_field("dst_id", 0x01);
    rsp.set_header_field("vc_id", 0);
    rsp.set_header_field("last", 1);
    RouterChannelInputs in{};
    in.node[0].rsp_in_valid = true;
    in.node[0].rsp_in_flit = flit_to_bytes(rsp);
    a.set_inputs(in);
    a.tick();
    a.set_inputs(RouterChannelInputs{});
    RouterChannelOutputs out{};
    bool ejected = false;
    for (int cyc = 0; cyc < 16 && !ejected; ++cyc) {
        a.tick();
        a.get_outputs(out);
        if (out.node[1].rsp_out_valid) ejected = true;
    }
    EXPECT_TRUE(ejected) << "response never ejected at node1 NMU";
}
