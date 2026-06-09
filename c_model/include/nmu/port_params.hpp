#pragma once
// NMU-side port-pair parameters. Source: c_model/config/port_params.yaml
// `nmu:` top-level block. Fields that NSU does not consume live in
// nsu/port_params.hpp; shared 5 AXI-channel depths are duplicated by
// design (independent NMU/NSU evolution, no spec-mandated symmetry).
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::nmu {

struct PortParams {
    // 5 AXI-channel slave-port internal FIFO depths.
    std::size_t aw_queue_depth;
    std::size_t w_queue_depth;
    std::size_t ar_queue_depth;
    std::size_t b_queue_depth;
    std::size_t r_queue_depth;
    // NMU Depacketize internal demux FIFO depths (NMU consumes B/R).
    std::size_t depkt_b_q_depth;
    std::size_t depkt_r_q_depth;
};

inline PortParams load_nmu_port_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto nmu = root["nmu"];
    if (!nmu) throw std::runtime_error("port_params.yaml: missing 'nmu:' block");
    auto q = nmu["queues"];
    if (!q) throw std::runtime_error("port_params.yaml: missing 'nmu.queues:' block");
    auto d = nmu["depacketize"];
    if (!d) throw std::runtime_error("port_params.yaml: missing 'nmu.depacketize:' block");
    PortParams p{};
    p.aw_queue_depth = q["aw_queue_depth"].as<std::size_t>();
    p.w_queue_depth = q["w_queue_depth"].as<std::size_t>();
    p.ar_queue_depth = q["ar_queue_depth"].as<std::size_t>();
    p.b_queue_depth = q["b_queue_depth"].as<std::size_t>();
    p.r_queue_depth = q["r_queue_depth"].as<std::size_t>();
    p.depkt_b_q_depth = d["b_q_depth"].as<std::size_t>();
    p.depkt_r_q_depth = d["r_q_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::nmu
