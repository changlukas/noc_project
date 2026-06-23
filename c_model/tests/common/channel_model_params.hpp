#pragma once
// ChannelModel test-fixture per-direction in-flight flit deque capacity.
// Not a production parameter — production ChannelModelWrap
// uses kPoCChannelModelDepth from wrap/poc_defaults.hpp directly.
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::testing {

struct ChannelModelParams {
    std::size_t req_depth;
    std::size_t rsp_depth;
};

inline ChannelModelParams load_channel_model_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto cm = root["channel_model"];
    if (!cm) throw std::runtime_error("port_params.yaml: missing 'channel_model:' block");
    ChannelModelParams p{};
    p.req_depth = cm["req_depth"].as<std::size_t>();
    p.rsp_depth = cm["rsp_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::testing
