#pragma once
#include "flit.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/router.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

// One flit's crossing of a boundary: the cycle it crossed plus the header
// fields needed for path / channel / VC attribution downstream.
struct FlitCrossing {
    uint64_t cycle;
    uint8_t axi_ch;
    uint8_t vc_id;
    uint8_t src_id;
    uint8_t dst_id;
};

// Per-boundary ordered crossing log. The probes write here; the dwell observer
// (Task 5) reads pairs of logs and matches by FIFO order.
class FlitLog {
  public:
    explicit FlitLog(std::string boundary_id) : boundary_id_(std::move(boundary_id)) {}

    void record(const Flit& f, uint64_t cycle) {
        crossings_.push_back(FlitCrossing{cycle, static_cast<uint8_t>(f.get_header_field("axi_ch")),
                                          static_cast<uint8_t>(f.get_header_field("vc_id")),
                                          static_cast<uint8_t>(f.get_header_field("src_id")),
                                          static_cast<uint8_t>(f.get_header_field("dst_id"))});
    }
    const std::vector<FlitCrossing>& crossings() const { return crossings_; }
    const std::string& boundary_id() const { return boundary_id_; }

  private:
    std::string boundary_id_;
    std::vector<FlitCrossing> crossings_;
};

// Push-side decorators: record on a successful (true-returning) push only, so a
// backpressured retry is not double-counted. Forward unchanged otherwise.
class ReqOutProbe : public noc::NocReqOut {
  public:
    ReqOutProbe(noc::NocReqOut& inner, FlitLog& log, const uint64_t& now)
        : inner_(inner), log_(log), now_(now) {}
    bool push_flit(const Flit& f) override {
        const bool ok = inner_.push_flit(f);
        if (ok) log_.record(f, now_);
        return ok;
    }
    bool credit_avail(uint8_t vc) const override { return inner_.credit_avail(vc); }

  private:
    noc::NocReqOut& inner_;
    FlitLog& log_;
    const uint64_t& now_;
};

class RspOutProbe : public noc::NocRspOut {
  public:
    RspOutProbe(noc::NocRspOut& inner, FlitLog& log, const uint64_t& now)
        : inner_(inner), log_(log), now_(now) {}
    bool push_flit(const Flit& f) override {
        const bool ok = inner_.push_flit(f);
        if (ok) log_.record(f, now_);
        return ok;
    }
    bool credit_avail(uint8_t vc) const override { return inner_.credit_avail(vc); }

  private:
    noc::NocRspOut& inner_;
    FlitLog& log_;
    const uint64_t& now_;
};

// RouterLink push is always accepted (credit guarantees space); record every push.
class LinkProbe : public noc::RouterLink {
  public:
    LinkProbe(noc::RouterLink& inner, FlitLog& log, const uint64_t& now)
        : inner_(inner), log_(log), now_(now) {}
    void push_flit(const Flit& f) override {
        log_.record(f, now_);
        inner_.push_flit(f);
    }

  private:
    noc::RouterLink& inner_;
    FlitLog& log_;
    const uint64_t& now_;
};

// Pop-side decorators: record only when a flit is actually returned.
class ReqInProbe : public noc::NocReqIn {
  public:
    ReqInProbe(noc::NocReqIn& inner, FlitLog& log, const uint64_t& now)
        : inner_(inner), log_(log), now_(now) {}
    std::optional<Flit> pop_flit() override {
        auto f = inner_.pop_flit();
        if (f) log_.record(*f, now_);
        return f;
    }

  private:
    noc::NocReqIn& inner_;
    FlitLog& log_;
    const uint64_t& now_;
};

class RspInProbe : public noc::NocRspIn {
  public:
    RspInProbe(noc::NocRspIn& inner, FlitLog& log, const uint64_t& now)
        : inner_(inner), log_(log), now_(now) {}
    std::optional<Flit> pop_flit() override {
        auto f = inner_.pop_flit();
        if (f) log_.record(*f, now_);
        return f;
    }

  private:
    noc::NocRspIn& inner_;
    FlitLog& log_;
    const uint64_t& now_;
};

}  // namespace ni::cmodel::testing
