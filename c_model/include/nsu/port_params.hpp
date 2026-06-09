#pragma once
// NSU-side port-pair parameters. Source: c_model/config/port_params.yaml
// `nsu:` top-level block. Fields that NMU does not consume live in
// nmu/port_params.hpp; shared 5 AXI-channel depths are duplicated by
// design (independent NMU/NSU evolution, no spec-mandated symmetry).
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::nsu {

struct PortParams {
    // 5 AXI-channel master-port internal FIFO depths.
    std::size_t aw_queue_depth;
    std::size_t w_queue_depth;
    std::size_t ar_queue_depth;
    std::size_t b_queue_depth;
    std::size_t r_queue_depth;
    // NSU Depacketize internal demux FIFO depths (NSU consumes AW/W/AR).
    std::size_t depkt_aw_q_depth;
    std::size_t depkt_w_q_depth;
    std::size_t depkt_ar_q_depth;
    // NSU MetaBuffer per-AXI-ID outstanding-request depth.
    std::size_t meta_buffer_per_id_depth;
};

inline PortParams load_nsu_port_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto nsu = root["nsu"];
    if (!nsu) throw std::runtime_error("port_params.yaml: missing 'nsu:' block");
    auto q = nsu["queues"];
    if (!q) throw std::runtime_error("port_params.yaml: missing 'nsu.queues:' block");
    auto d = nsu["depacketize"];
    if (!d) throw std::runtime_error("port_params.yaml: missing 'nsu.depacketize:' block");
    auto m = nsu["meta_buffer"];
    if (!m) throw std::runtime_error("port_params.yaml: missing 'nsu.meta_buffer:' block");
    PortParams p{};
    p.aw_queue_depth = q["aw_queue_depth"].as<std::size_t>();
    p.w_queue_depth = q["w_queue_depth"].as<std::size_t>();
    p.ar_queue_depth = q["ar_queue_depth"].as<std::size_t>();
    p.b_queue_depth = q["b_queue_depth"].as<std::size_t>();
    p.r_queue_depth = q["r_queue_depth"].as<std::size_t>();
    p.depkt_aw_q_depth = d["aw_q_depth"].as<std::size_t>();
    p.depkt_w_q_depth = d["w_q_depth"].as<std::size_t>();
    p.depkt_ar_q_depth = d["ar_q_depth"].as<std::size_t>();
    p.meta_buffer_per_id_depth = m["per_id_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::nsu
