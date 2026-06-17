#include "axi/types.hpp"
#include "nmu/nmu.hpp"
#include <gtest/gtest.h>

namespace nmu = ni::cmodel::nmu;
namespace axi = ni::cmodel::axi;

// Empty RoB reports zero occupancy; after one AW is accepted and forwarded
// (push_aw queues in the port; tick() forwards it into the RoB) the write
// occupancy is non-zero. With no B response arriving (null rsp stub) the
// outstanding entry persists, so occupancy stays > 0.
TEST(RobOccupancy, EmptyThenNonZeroAfterAw) {
    nmu::NmuConfig cfg;  // defaults
    // NmuConfig{} zero-initializes PortParams queue depths; must set non-zero
    // values for push_aw to be accepted (depth=0 means queue full immediately).
    cfg.port_params.aw_queue_depth = 4;
    cfg.port_params.w_queue_depth = 4;
    cfg.port_params.ar_queue_depth = 4;
    cfg.port_params.b_queue_depth = 4;
    cfg.port_params.r_queue_depth = 4;
    cfg.port_params.depkt_b_q_depth = 4;
    cfg.port_params.depkt_r_q_depth = 4;
    nmu::NmuStandalone n(cfg);
    EXPECT_EQ(n.rob().write_occupancy(), 0u);
    EXPECT_EQ(n.rob().read_occupancy(), 0u);

    axi::AwBeat aw{/*id=*/0,         /*addr=*/0x1000, /*len=*/0,  /*size=*/5,
                   axi::Burst::INCR, /*cache=*/0xA,   /*lock=*/0, /*prot=*/0x3,
                   /*region=*/0xF,   /*user=*/0x55,   /*qos=*/0xC};
    ASSERT_TRUE(n.axi_slave_port().push_aw(aw));
    n.tick();  // port forwards AW into the RoB
    EXPECT_GT(n.rob().write_occupancy(), 0u);
}
