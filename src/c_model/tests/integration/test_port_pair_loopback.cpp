// PortParamsSplit: regression gate for the T3 refactor that split the single
// shared PortParams into independent NMU / NSU per-side params.
//
// The end-to-end port-pair loopback that used to live here was retired: it
// re-proved scoreboard-clean transport over a hand-assembled C++ fabric, which
// the wire-level co-sim already gates more truthfully. What survives is the
// per-side config logic co-sim does not exercise: asymmetric queue-depth
// independence and the YAML loader's fail-loud contracts.
#include "common/loopback_channel_set.hpp"
#include "common/tmp_path.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/port_params.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>

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

// 2. Loader fail-loud: YAML missing 'nmu:' block throws std::runtime_error.
TEST(PortParamsSplit, LoaderMissingNmuBlockThrows) {
    auto p = ni::cmodel::testing::unique_temp_path("bad_nmu") + ".yaml";
    std::ofstream(p) << "nsu: {}\nchannel_model: {}\n";
    EXPECT_THROW(nmu::load_nmu_port_params(p), std::runtime_error);
}

// 3. Loader fail-loud: YAML with 'nmu:' present but missing 'w_queue_depth'
//    inside 'queues:' throws (yaml-cpp throws BadConversion or KeyNotFound).
TEST(PortParamsSplit, LoaderMissingNmuQueueKeyThrows) {
    auto p = ni::cmodel::testing::unique_temp_path("bad_nmu_key") + ".yaml";
    std::ofstream(p) << "nmu:\n  queues:\n    aw_queue_depth: 32\n"
                        "    # w_queue_depth intentionally missing\n"
                        "    ar_queue_depth: 32\n    b_queue_depth: 32\n    r_queue_depth: 32\n"
                        "  depacketize: { b_q_depth: 32, r_q_depth: 32 }\n";
    // EXPECT_ANY_THROW (not EXPECT_THROW with a specific type) because
    // yaml-cpp's missing-key path throws YAML::TypedBadConversion (a
    // yaml-cpp internal subclass of std::runtime_error), not the explicit
    // std::runtime_error the block-level guards throw.
    EXPECT_ANY_THROW(nmu::load_nmu_port_params(p));
}
