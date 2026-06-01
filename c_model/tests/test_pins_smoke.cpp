#include "ni_spec.hpp"
#include <gtest/gtest.h>

TEST(PinsSmoke, AxiSlavePortPinsCompilesAndResets) {
  ni::pins::AxiSlavePortPins pins{};
  pins.reset_outputs();  // must not throw, must not require specific input values
  SUCCEED();
}

TEST(PinsSmoke, AllBundlesInstantiable) {
  ni::pins::AxiSlavePortPins  axi_s{};
  ni::pins::AxiMasterPortPins axi_m{};
  ni::pins::NocReqOutPins     nq_o{};
  ni::pins::NocReqInPins      nq_i{};
  ni::pins::NocRspOutPins     nr_o{};
  ni::pins::NocRspInPins      nr_i{};
  ni::pins::CsrPins           csr{};
  (void)axi_s; (void)axi_m; (void)nq_o; (void)nq_i; (void)nr_o; (void)nr_i; (void)csr;
  SUCCEED();
}
