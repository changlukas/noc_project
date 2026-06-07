// LoopbackNocShellAdapter — Stage 5b ShellAdapter for the LoopbackNoc component.
//
// Owns a LoopbackNoc instance (standalone ctor via LoopbackNocConfig — hermetic,
// no cross-component refs). Implements the 3-step pattern per spec §5.1:
//   set_inputs(in)   → latch SV wire values into in_
//   tick()           → push latched inputs into c_model → advance → sample outputs
//   get_outputs(out) → copy output latch to caller
//
// Hermetic invariant: no refs to other ShellAdapters. All cross-component
// dataflow is via SV wires driven by DPI handlers in cosim/c/cmodel_dpi.cpp.
//
// LoopbackNoc dataflow:
//   req path:  NMU side → nmu_req_out().push_flit() → internal queue →
//              nsu_req_in(0).pop_flit() → NSU side
//   rsp path:  NSU side → nsu_rsp_out(0).push_flit() → internal queue →
//              nmu_rsp_in().pop_flit() → NMU side
//
// pop_flit() is destructive (no peek API). Sampled outputs are staged in out_
// and held until the next tick() overwrites them.
#pragma once
#include "common/loopback_noc.hpp"
#include "cosim/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "cosim/loopback_noc_shell_io.hpp"
#include "cosim/poc_defaults.hpp"  // kPoCLoopbackNocDepth
#include <memory>

namespace ni::cmodel::cosim {

class LoopbackNocShellAdapter {
  public:
    void init() {
        // Single-NSU backward-compat ctor: auto-routes all dst_id to NSU_0.
        loopback_ = std::make_unique<testing::LoopbackNoc>(
            /*req_depth=*/kPoCLoopbackNocDepth, /*rsp_depth=*/kPoCLoopbackNocDepth);
        in_ = LoopbackNocInputs{};
        out_ = LoopbackNocOutputs{};
    }

    void set_inputs(const LoopbackNocInputs& in) { in_ = in; }

    void tick() {
        // Step 1: drive c_model from input latch.
        //
        // req_in: flit arriving from NMU side → push into loopback NMU req port.
        // Backpressure (push_flit returns false) is not expected at depth=64 + 1
        // flit/cycle; the SV wrapper enforces the same credit discipline.
        if (in_.req_in_valid) {
            loopback_->nmu_req_out().push_flit(flit_from_bytes(in_.req_in_flit));
        }

        // rsp_in: flit arriving from NSU side → push into loopback NSU rsp port.
        if (in_.rsp_in_valid) {
            loopback_->nsu_rsp_out(0).push_flit(flit_from_bytes(in_.rsp_in_flit));
        }

        // Step 2: advance c_model one cycle.
        loopback_->tick();

        // Step 3: sample output state from c_model into output latch.
        out_ = LoopbackNocOutputs{};

        // req_out: NSU side pops one request flit from the loopback per cycle.
        if (auto f = loopback_->nsu_req_in(0).pop_flit()) {
            out_.req_out_valid = true;
            out_.req_out_flit = flit_to_bytes(*f);
        }

        // rsp_out: NMU side pops one response flit from the loopback per cycle.
        if (auto f = loopback_->nmu_rsp_in().pop_flit()) {
            out_.rsp_out_valid = true;
            out_.rsp_out_flit = flit_to_bytes(*f);
        }

        // credit_return: loopback has capacity for next flit (NUM_VC=1 PoC, vc=0).
        out_.req_out_credit_return = loopback_->nmu_req_out().credit_avail(0);
        out_.rsp_out_credit_return = loopback_->nsu_rsp_out(0).credit_avail(0);
    }

    void get_outputs(LoopbackNocOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<testing::LoopbackNoc> loopback_;
    LoopbackNocInputs in_{};
    LoopbackNocOutputs out_{};

    // FlitBytes <-> c_model Flit helpers live in cosim/flit_byte_conv.hpp;
    // calls use flit_from_bytes(...) / flit_to_bytes(...) directly via ADL.
};

}  // namespace ni::cmodel::cosim
