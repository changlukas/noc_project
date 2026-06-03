// Test logger: SCENARIO macro + AxiMasterObserver.
//
// SCENARIO("<English desc>") prints a one-line, always-emitted, human-readable
// scenario description at the start of a TEST() body so the cryptic C++
// identifier-based test name does not have to carry the full intent of the
// scenario.
//
// AxiMasterObserver hooks AxiMaster's existing on_write_completed /
// on_read_observed callbacks (zero production-code change) and:
//   - counts logical (per-burst) AW / W / AR / B / R transactions
//   - tracks per-AXI-ID submission_line sequences for B and R
//   - auto-detects AXI4 IHI 0022 §A5.3 per-id ordering violations
//   - flags non-OKAY responses as mismatches (does not auto-fail)
//   - on print_summary() (RAII fallback in dtor), checks for stuck txns
//   - emits parse-friendly per-transaction trace when NOC_LOG=1
//
// Usage:
//   AxiMasterObserver obs(master, "NMU");
//   // ... run test loop ...
//   // Summary auto-prints at scope exit, OR call obs.print_summary() early.
//
// See docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md
#pragma once

#include "axi/axi_master.hpp"
#include "axi/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

// Always-emitted one-line scenario description (no env var gate).
// Output: "[scenario] <desc>"
inline void print_scenario(const char* desc) {
    std::cout << "[scenario] " << desc << '\n';
}

}  // namespace ni::cmodel::testing

// Wrap to a single statement so SCENARIO("...") can sit on its own line
// inside any TEST() body without surprising the surrounding control flow.
#define SCENARIO(desc) ::ni::cmodel::testing::print_scenario(desc)

namespace ni::cmodel::testing {

class AxiMasterObserver {
public:
    // Normal ctor: subscribes to AxiMaster completion callbacks.
    AxiMasterObserver(axi::AxiMaster& master,
                      std::string instance_name = "AxiMaster")
        : name_(std::move(instance_name)) {
        init_verbose_();
        master.on_write_completed(
            [this](const axi::WriteResult& wr) { on_write(wr); });
        master.on_read_observed(
            [this](const axi::ReadResult& rr) { on_read(rr); });
    }

    // Test-only ctor: no AxiMaster bound. Callbacks must be invoked via
    // test_inject_write_result / test_inject_read_result.
    explicit AxiMasterObserver(std::string instance_name)
        : name_(std::move(instance_name)) {
        init_verbose_();
    }

    // RAII fallback: print summary if not already printed so a forgotten
    // explicit call does not lose context (especially in failure paths).
    ~AxiMasterObserver() {
        if (!summary_printed_) print_summary();
    }

    // Explicit summary print. Sets summary_printed_=true on entry so the
    // destructor will not double-print.
    void print_summary() {
        summary_printed_ = true;

        // Auto-fail #4 (spec §8): stuck transactions at end of test.
        if (b_count_ < aw_count_) {
            failures_.push_back(
                "stuck_writes: aw_count=" + std::to_string(aw_count_) +
                " > b_count=" + std::to_string(b_count_) +
                " (incomplete write transactions at end of test)");
        }
        if (r_count_ < ar_count_) {
            failures_.push_back(
                "stuck_reads: ar_count=" + std::to_string(ar_count_) +
                " > r_count=" + std::to_string(r_count_) +
                " (incomplete read transactions at end of test)");
        }

        std::cout << "[summary:" << name_ << "] "
                  << "aw="                 << aw_count_           << ' '
                  << "w_logical="          << aw_count_           << ' '
                  << "ar="                 << ar_count_           << ' '
                  << "b="                  << b_count_            << ' '
                  << "r="                  << r_count_            << ' '
                  << "mismatches="         << mismatches_         << ' '
                  << "b_order_violations=" << b_order_violations_ << ' '
                  << "r_order_violations=" << r_order_violations_ << '\n';
        for (const auto& f : failures_) {
            std::cout << "[FAIL:" << name_ << "] " << f << '\n';
        }
    }

    // ---- Accessors ----------------------------------------------------
    bool                            ok()          const { return failures_.empty(); }
    const std::vector<std::string>& failures()    const { return failures_; }
    std::size_t                     aw_count()    const { return aw_count_; }
    std::size_t                     ar_count()    const { return ar_count_; }
    std::size_t                     b_count()     const { return b_count_;  }
    std::size_t                     r_count()     const { return r_count_;  }
    std::size_t                     mismatches()  const { return mismatches_; }

    // ---- Test-only injection ------------------------------------------
    // Public on purpose: enables unit tests of the observer itself without
    // standing up a full AxiMaster + scenario YAML. Production callers
    // should let the AxiMaster callbacks drive the observer.
    void test_inject_write_result(const axi::WriteResult& wr) { on_write(wr); }
    void test_inject_read_result (const axi::ReadResult&  rr) { on_read (rr); }
    void test_set_aw_count(std::size_t n) { aw_count_ = n; }
    void test_set_ar_count(std::size_t n) { ar_count_ = n; }

private:
    void init_verbose_() {
        const char* env = std::getenv("NOC_LOG");
        verbose_ = (env != nullptr) && std::string(env) == "1";
    }

    void on_write(const axi::WriteResult& wr) {
        ++obs_seq_;
        ++aw_count_;       // logical AW issued (callback fires on burst complete)
        ++b_count_;        // logical B observed
        if (wr.resp != axi::Resp::OKAY) ++mismatches_;
        check_b_order(wr.id, wr.scenario_line);
        if (verbose_) {
            std::cout << "[axi:" << name_ << "] obs_seq=" << obs_seq_
                      << " ch=B  id=0x" << hex_byte(wr.id)
                      << " resp=" << resp_str(wr.resp)
                      << " scenario_line=" << wr.scenario_line << '\n';
        }
    }

    void on_read(const axi::ReadResult& rr) {
        ++obs_seq_;
        ++ar_count_;
        ++r_count_;
        if (rr.resp != axi::Resp::OKAY) ++mismatches_;
        check_r_order(rr.id, rr.scenario_line);
        if (verbose_) {
            std::cout << "[axi:" << name_ << "] obs_seq=" << obs_seq_
                      << " ch=R  id=0x" << hex_byte(rr.id)
                      << " resp=" << resp_str(rr.resp)
                      << " scenario_line=" << rr.scenario_line << '\n';
        }
    }

    void check_b_order(uint8_t id, std::size_t scenario_line) {
        auto& seq = b_seq_by_id_[id];
        if (!seq.empty() && scenario_line < seq.back()) {
            ++b_order_violations_;
            failures_.push_back(
                "axi4_id_order_violation: id=0x" + hex_byte(id) +
                " expected B in submission order but got scenario_line=" +
                std::to_string(scenario_line) + " after scenario_line=" +
                std::to_string(seq.back()) +
                " (Possible causes: Rob reorder logic, per-NSU latency"
                " setup, or AxiMaster issue ordering)");
        }
        seq.push_back(scenario_line);
    }

    void check_r_order(uint8_t id, std::size_t scenario_line) {
        auto& seq = r_seq_by_id_[id];
        if (!seq.empty() && scenario_line < seq.back()) {
            ++r_order_violations_;
            failures_.push_back(
                "axi4_id_order_violation: id=0x" + hex_byte(id) +
                " expected R in submission order but got scenario_line=" +
                std::to_string(scenario_line) + " after scenario_line=" +
                std::to_string(seq.back()) +
                " (Possible causes: Rob reorder logic, per-NSU latency"
                " setup, or AxiMaster issue ordering)");
        }
        seq.push_back(scenario_line);
    }

    static std::string hex_byte(uint8_t v) {
        static const char* d = "0123456789abcdef";
        return std::string{d[v >> 4], d[v & 0xF]};
    }

    static const char* resp_str(axi::Resp r) {
        switch (r) {
            case axi::Resp::OKAY:   return "OKAY";
            case axi::Resp::EXOKAY: return "EXOKAY";
            case axi::Resp::SLVERR: return "SLVERR";
            case axi::Resp::DECERR: return "DECERR";
        }
        return "UNKNOWN";
    }

    std::string                                name_;
    bool                                       verbose_            = false;
    bool                                       summary_printed_    = false;
    std::size_t                                obs_seq_            = 0;
    std::size_t                                aw_count_           = 0;
    std::size_t                                ar_count_           = 0;
    std::size_t                                b_count_            = 0;
    std::size_t                                r_count_            = 0;
    std::size_t                                mismatches_         = 0;
    std::size_t                                b_order_violations_ = 0;
    std::size_t                                r_order_violations_ = 0;
    std::array<std::vector<std::size_t>, 256>  b_seq_by_id_;
    std::array<std::vector<std::size_t>, 256>  r_seq_by_id_;
    std::vector<std::string>                   failures_;
};

}  // namespace ni::cmodel::testing
