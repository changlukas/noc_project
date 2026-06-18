#ifndef NI_CMODEL_NOC_PIPELINE_STAGE_HPP
#define NI_CMODEL_NOC_PIPELINE_STAGE_HPP
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
namespace ni::cmodel::noc {
// One-token stage register; the staged-NI building block. Mirrors Router::landing_
// (router.hpp:157) + overwrite assert (router.hpp:185). Reverse-order tick drains
// (take) before upstream fills (accept), so a token advances one stage/tick.
template <class Token>
class PipelineStage {
  public:
    bool full() const noexcept { return slot_.has_value(); }
    bool ready() const noexcept { return !slot_.has_value(); }
    std::size_t occupancy() const noexcept { return slot_ ? 1u : 0u; }
    const Token& peek() const {
        assert(slot_ && "PipelineStage: peek empty");
        return *slot_;
    }
    void accept(Token t) {
        assert(!slot_ && "PipelineStage: overwrite (>1/cycle)");
        slot_ = std::move(t);
    }
    Token take() {
        assert(slot_ && "PipelineStage: take empty");
        Token t = std::move(*slot_);
        slot_.reset();
        return t;
    }
    void clear() noexcept { slot_.reset(); }

  private:
    std::optional<Token> slot_;
};
}  // namespace ni::cmodel::noc
#endif
