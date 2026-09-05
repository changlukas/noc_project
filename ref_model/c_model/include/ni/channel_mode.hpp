#pragma once
#include <cstdint>

namespace ni::cmodel::ni {

enum class ChannelMode : uint8_t {
    Native = 0,
    TwoChannel64 = 2,
    ThreeChannel64 = 3,
};

}  // namespace ni::cmodel::ni
