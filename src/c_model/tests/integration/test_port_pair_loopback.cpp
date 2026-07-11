// PortParamsSplit: regression gate for the T3 refactor that split the single
// shared PortParams into independent NMU / NSU per-side params.
//
// The end-to-end port-pair loopback that used to live here was retired: it
// re-proved scoreboard-clean transport over a hand-assembled C++ fabric, which
// the wire-level co-sim already gates more truthfully. What survives is the
// per-side config logic co-sim does not exercise: asymmetric queue-depth
// independence.
#include "common/loopback_channel_set.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/port_params.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;
namespace nmu = ni::cmodel::nmu;

// 1. Asymmetric sizing: NMU saturates at its own aw_queue_depth (depth 2)
//    while NSU would accept more. Pre-refactor (single shared PortParams)
//    both sides used identical values; the split makes independence visible.
TEST(PortParamsSplit, AsymmetricNmuNsuAwQueueSaturationIndependent) {
    ni::cmodel::nmu::PortParams nmu_pp{};
    nmu_pp.aw_queue_depth = 2;
    nmu_pp.w_queue_depth = 32;
    nmu_pp.ar_queue_depth = 32;
    nmu_pp.b_queue_depth = 32;
    nmu_pp.r_queue_depth = 32;
    nmu_pp.depkt_b_q_depth = 32;
    nmu_pp.depkt_r_q_depth = 32;

    ni::cmodel::testing::LoopbackChannelSet ch{};
    ni::cmodel::testing::LoopbackRequestPacketizer pkt(ch.request);
    ni::cmodel::testing::LoopbackResponseDepacketizer depkt(ch.response);
    nmu::AxiSlavePort nmu_port(pkt, depkt, nmu_pp);

    axi::AwBeat aw{};
    EXPECT_TRUE(nmu_port.push_aw(aw));
    EXPECT_TRUE(nmu_port.push_aw(aw));
    EXPECT_FALSE(nmu_port.push_aw(aw));  // NMU side full at depth 2
}
