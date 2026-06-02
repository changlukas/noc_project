// Test-only Packetizer adapter that wraps nmu::Packetize and auto-calls
// the sticky header-extras setters before each push_aw / push_ar. This is
// the bridge between the NMU AxiSlavePort's Packetizer-interface contract
// (which calls only push_aw(beat) / push_w(beat) / push_ar(beat)) and
// nmu::Packetize's sticky-setter API (set_aw_header_extras /
// set_ar_header_extras must be called first or the push_* will assert).
//
// Production wiring (the future nmu::AddrTrans task) replaces this adapter
// with a real address-translation layer that derives dst_id / rob_* from
// the AXI address and outstanding-transaction bookkeeping.
//
// Scope: integration-test glue only. push_b / push_r assert false because
// NMU never emits responses; the response path runs through nsu::Packetize
// on the peer side. The held real_ packetizer is the actual nmu::Packetize
// instance — this adapter neither owns nor copies any beats.
#pragma once
#include "axi/types.hpp"
#include "ni/packetizer.hpp"
#include "nmu/packetize.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace ni::cmodel::testing {

class TestPacketize : public Packetizer {
 public:
  TestPacketize(nmu::Packetize& real, uint8_t fixed_dst_id)
      : real_(real), dst_(fixed_dst_id) {}

  bool push_aw(const axi::AwBeat& b) override {
    real_.set_aw_header_extras(dst_, 0, 0);
    return real_.push_aw(b);
  }
  bool push_w(const axi::WBeat& b) override {
    // dst inherited from the AW write-meta FIFO inside nmu::Packetize.
    return real_.push_w(b);
  }
  bool push_ar(const axi::ArBeat& b) override {
    real_.set_ar_header_extras(dst_, 0, 0);
    return real_.push_ar(b);
  }
  bool push_b(const axi::BBeat&) override {
    assert(false && "TestPacketize: NMU never emits B");
    std::abort();
    return false;
  }
  bool push_r(const axi::RBeat&) override {
    assert(false && "TestPacketize: NMU never emits R");
    std::abort();
    return false;
  }

 private:
  nmu::Packetize& real_;
  uint8_t dst_;
};

}  // namespace ni::cmodel::testing
