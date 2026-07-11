#pragma once
// ChannelModel test-fixture per-direction in-flight flit deque capacity.
// ChannelModel is a test-only stub (c_model/tests/common/); it has no
// production wrap counterpart.
#include <cstddef>

namespace ni::cmodel::testing {

struct ChannelModelParams {
    std::size_t req_depth = 32;
    std::size_t rsp_depth = 32;
};

}  // namespace ni::cmodel::testing
