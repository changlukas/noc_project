// Regression: WireSlavePort presents at most ONE request beat per tick, and it
// must be the FIRST offered (oldest), not the last.
//
// Bug (STR-001 multi-outstanding): tick_push_aw_w_ walks the whole
// active_write_ops_ map every tick, so multiple ids call push_aw in one tick.
// push_aw latched last_aw_ BEFORE the gate, so during the wait cycles (awready
// low) a later id overwrote last_aw_ to the LAST id walked. The wire then
// presented the wrong AW (id 8) and replayed it while ids 1..7 were lost, so
// the subordinate saw duplicate writes and returned an extra B -> master
// B_FRONT_CAN_ACCEPT assert. Fix: gate at OFFER time so the first offered beat
// stays presented; later ids retry next tick.

#include "axi/axi_master.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

namespace {

axi::AwBeat aw_with_id(uint8_t id) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = 0x1000u + id;  // distinct address per id
    return b;
}
axi::ArBeat ar_with_id(uint8_t id) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = 0x2000u + id;
    return b;
}

// A tick where the caller offers 3 distinct-id AWs while NOT ready must present
// the FIRST (id=1), not the last (id=3). This is the exact multi-outstanding
// overwrite the bug produced.
TEST(WireSlavePortPresentation, AwFirstOfferedWinsWhileNotReady) {
    axi::detail::WireSlavePort wp;

    wp.set_awready(false);  // tick start: reset offer gate, subordinate not ready
    EXPECT_FALSE(wp.push_aw(aw_with_id(1)));  // offered, held (A3.2.1)
    EXPECT_FALSE(wp.push_aw(aw_with_id(2)));  // blocked: one AW already offered this tick
    EXPECT_FALSE(wp.push_aw(aw_with_id(3)));  // blocked

    auto pend = wp.pending_aw();
    ASSERT_TRUE(pend.has_value());
    EXPECT_EQ(pend->id, 1u) << "wire must present the FIRST offered AW, not the last";
}

// Ready tick: exactly one AW delivers; later same-tick offers are blocked.
TEST(WireSlavePortPresentation, AwOneDeliveryPerTick) {
    axi::detail::WireSlavePort wp;

    wp.set_awready(true);
    EXPECT_TRUE(wp.push_aw(aw_with_id(1)));   // id 1 handshakes
    EXPECT_FALSE(wp.push_aw(aw_with_id(2)));  // blocked this tick
    EXPECT_FALSE(wp.pending_aw().has_value())  // delivered -> nothing left pending
        << "after delivery the AW is consumed, awvalid drops";
}

// After id 1 delivers, the next tick presents id 2 (progression, no starvation).
TEST(WireSlavePortPresentation, AwProgressesToNextIdNextTick) {
    axi::detail::WireSlavePort wp;

    wp.set_awready(true);
    ASSERT_TRUE(wp.push_aw(aw_with_id(1)));  // deliver id 1

    wp.set_awready(false);  // next tick, not ready
    EXPECT_FALSE(wp.push_aw(aw_with_id(2)));
    EXPECT_FALSE(wp.push_aw(aw_with_id(3)));
    auto pend = wp.pending_aw();
    ASSERT_TRUE(pend.has_value());
    EXPECT_EQ(pend->id, 2u) << "next tick must present the next id, not replay id 1";
}

// Same guarantee on the AR channel (the read-side 4KB sub-burst loop that the
// AR-drop fix targeted). First offered AR wins while not ready.
TEST(WireSlavePortPresentation, ArFirstOfferedWinsWhileNotReady) {
    axi::detail::WireSlavePort wp;

    wp.set_arready(false);
    EXPECT_FALSE(wp.push_ar(ar_with_id(1)));
    EXPECT_FALSE(wp.push_ar(ar_with_id(2)));
    EXPECT_FALSE(wp.push_ar(ar_with_id(3)));

    auto pend = wp.pending_ar();
    ASSERT_TRUE(pend.has_value());
    EXPECT_EQ(pend->id, 1u) << "wire must present the FIRST offered AR, not the last";
}

}  // namespace
