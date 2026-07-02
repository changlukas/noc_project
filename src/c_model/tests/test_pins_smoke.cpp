#include "ni_spec.hpp"
#include <gtest/gtest.h>

TEST(PinsSmoke, AxiSlavePortPinsCompilesAndResets) {
    ni::pins::AxiSlavePortPins pins{};
    pins.reset_outputs();  // must not throw, must not require specific input values
    SUCCEED();
}

TEST(PinsSmoke, AllBundlesInstantiable) {
    ni::pins::AxiSlavePortPins axi_s{};
    ni::pins::AxiMasterPortPins axi_m{};
    ni::pins::NocIntfMosiPins noc_mosi{};
    ni::pins::NocIntfMisoPins noc_miso{};
    ni::pins::CsrPins csr{};
    (void)axi_s;
    (void)axi_m;
    (void)noc_mosi;
    (void)noc_miso;
    (void)csr;
    SUCCEED();
}
