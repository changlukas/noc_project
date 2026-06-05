// Unit tests for LoopbackNocShellAdapter — Stage 5b T7.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Two cases cover the key behavioral invariants:
//   1. A req_in flit at cycle T appears as req_out at cycle T+1 (beta tick latency).
//   2. An idle adapter produces no valid output.
//
// Include c_model/tests/ via CMake target_include_directories so that
// "common/loopback_noc.hpp" (transitively included by the adapter) resolves.
#include "common/scenario.hpp"
#include "cosim2/loopback_noc_shell_adapter.hpp"
#include "cosim2/loopback_noc_shell_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::cosim2::LoopbackNocInputs;
using ni::cmodel::cosim2::LoopbackNocOutputs;
using ni::cmodel::cosim2::LoopbackNocShellAdapter;

TEST(LoopbackNocShellAdapter, req_flit_forwards_after_one_tick) {
    SCENARIO("req_in flit at cycle T appears as req_out at cycle T+1 (beta tick)");
    LoopbackNocShellAdapter adapter;
    adapter.init();

    LoopbackNocInputs in{};
    in.req_in_valid  = true;
    in.req_in_flit[0] = 0xAB;
    in.req_in_credit_return = false;
    in.rsp_in_valid  = false;

    adapter.set_inputs(in);
    adapter.tick();

    LoopbackNocOutputs out{};
    adapter.get_outputs(out);

    EXPECT_TRUE(out.req_out_valid);
    EXPECT_EQ(out.req_out_flit[0], 0xAB);
    EXPECT_FALSE(out.rsp_out_valid);
}

TEST(LoopbackNocShellAdapter, idle_adapter_produces_no_valid_output) {
    SCENARIO("idle adapter (all inputs false) produces no valid output");
    LoopbackNocShellAdapter adapter;
    adapter.init();

    LoopbackNocInputs in{};  // all zero / false
    adapter.set_inputs(in);
    adapter.tick();

    LoopbackNocOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.req_out_valid);
    EXPECT_FALSE(out.rsp_out_valid);
}
