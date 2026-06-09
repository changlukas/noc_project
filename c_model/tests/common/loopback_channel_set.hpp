#pragma once
// loopback_channel_set.hpp — aggregate of both REQ and RSP channel sets, for
// integration tests that wire NMU and NSU through a single loopback.
// Plane-specific consumers can stay on loopback_request_io.hpp or
// loopback_response_io.hpp alone.
#include "common/loopback_request_io.hpp"
#include "common/loopback_response_io.hpp"

namespace ni::cmodel::testing {

struct LoopbackChannelSet {
    RequestChannelSet request;
    ResponseChannelSet response;
};

}  // namespace ni::cmodel::testing
