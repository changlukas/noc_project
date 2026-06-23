// Unit tests for ChannelModelWrap — Stage 5b T7.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Two cases cover the key behavioral invariants:
//   1. A req_in flit at cycle T appears as req_out at cycle T+1 (beta tick latency).
//   2. An idle adapter produces no valid output.
//
// Include c_model/tests/ via CMake target_include_directories so that
// "common/channel_model.hpp" (transitively included by the adapter) resolves.
#include "common/scenario.hpp"
#include "wrap/channel_model_wrap.hpp"
#include "wrap/channel_model_wrap_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::wrap::ChannelModelInputs;
using ni::cmodel::wrap::ChannelModelOutputs;
using ni::cmodel::wrap::ChannelModelWrap;

TEST(ChannelModelWrap, req_flit_forwards_after_one_tick) {
    SCENARIO("req_in flit at cycle T appears as req_out at cycle T+1 (beta tick)");
    ChannelModelWrap adapter;
    adapter.init();

    ChannelModelInputs in{};
    in.req_in_valid = true;
    in.req_in_flit[0] = 0xAB;
    in.req_in_credit_return = false;
    in.rsp_in_valid = false;

    adapter.set_inputs(in);
    adapter.tick();

    ChannelModelOutputs out{};
    adapter.get_outputs(out);

    EXPECT_TRUE(out.req_out_valid);
    EXPECT_EQ(out.req_out_flit[0], 0xAB);
    EXPECT_FALSE(out.rsp_out_valid);
}

TEST(ChannelModelWrap, idle_adapter_produces_no_valid_output) {
    SCENARIO("idle adapter (all inputs false) produces no valid output");
    ChannelModelWrap adapter;
    adapter.init();

    ChannelModelInputs in{};  // all zero / false
    adapter.set_inputs(in);
    adapter.tick();

    ChannelModelOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.req_out_valid);
    EXPECT_FALSE(out.rsp_out_valid);
}
