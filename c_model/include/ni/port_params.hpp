#pragma once
// Shared NMU + NSU transparent transport port parameters.
//
// Both nmu::AxiSlavePort and nsu::AxiMasterPort hold per-channel internal
// FIFOs whose depths come from a single YAML source of truth
// (c_model/config/port_params.yaml). Before this header existed each side
// declared its own byte-identical 5-field struct and each test re-implemented
// the YAML loader — adding a new field meant ≥4 edits. One struct + one
// loader collapses those sites.
//
// Deliberately NO defaults on the struct fields: a default-constructed
// PortParams would carry zero-depth queues and reject every push, which
// surfaces missing YAML wiring as a hard test failure instead of silently
// inheriting a magic header default ("fail loud").
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <string>

namespace ni::cmodel {

struct PortParams {
    // Stage 3 port-pair: AxiSlavePort / AxiMasterPort per-channel queue depths.
    std::size_t aw_queue_depth;
    std::size_t w_queue_depth;
    std::size_t ar_queue_depth;
    std::size_t b_queue_depth;
    std::size_t r_queue_depth;

    // Stage 3 depacketize: per-channel internal demux FIFO depths used inside
    // the Depacketize stage. NMU only consumes (b, r); NSU only consumes
    // (aw, w, ar); the unused fields on each side are loaded but ignored.
    // Kept as a single struct to match the Packetizer/Depacketizer abstract
    // base shape (one base, both halves).
    std::size_t depkt_aw_q_depth;
    std::size_t depkt_w_q_depth;
    std::size_t depkt_ar_q_depth;
    std::size_t depkt_b_q_depth;
    std::size_t depkt_r_q_depth;

    // Stage 3 LoopbackNoc test fixture: per-direction in-flight flit deque
    // capacity. Sized large enough for the longest e2e test burst; not a
    // production parameter.
    std::size_t loopback_noc_req_depth;
    std::size_t loopback_noc_rsp_depth;

    // Stage 3 MetaBuffer (NSU side): per-AXI-ID FIFO depth tracking
    // outstanding request metadata while the response is in flight.
    std::size_t meta_buffer_per_id_depth;
};

// Load PortParams from c_model/config/port_params.yaml.
//
// The current YAML schema is a flat block of port-pair keys plus three
// sub-maps (depacketize / loopback_noc / meta_buffer); all values are shared
// by both sides (NMU AxiSlavePort and NSU AxiMasterPort load identical
// PortParams). The `side` parameter is reserved for a future schema that
// splits per-side blocks (e.g. `nmu:` / `nsu:` sub-maps); today it is
// unused. Callers should still pass "nmu" or "nsu" so a future split is a
// header-only change.
inline PortParams load_port_params_yaml(const std::string& path, const std::string& /*side*/) {
    auto root = YAML::LoadFile(path);
    PortParams p{};
    p.aw_queue_depth = root["aw_queue_depth"].as<std::size_t>();
    p.w_queue_depth = root["w_queue_depth"].as<std::size_t>();
    p.ar_queue_depth = root["ar_queue_depth"].as<std::size_t>();
    p.b_queue_depth = root["b_queue_depth"].as<std::size_t>();
    p.r_queue_depth = root["r_queue_depth"].as<std::size_t>();

    // Depacketize internal demux queues. Not side-specific in YAML; each
    // concrete Depacketize impl reads only the channels it owns.
    p.depkt_aw_q_depth = root["depacketize"]["aw_q_depth"].as<std::size_t>();
    p.depkt_w_q_depth = root["depacketize"]["w_q_depth"].as<std::size_t>();
    p.depkt_ar_q_depth = root["depacketize"]["ar_q_depth"].as<std::size_t>();
    p.depkt_b_q_depth = root["depacketize"]["b_q_depth"].as<std::size_t>();
    p.depkt_r_q_depth = root["depacketize"]["r_q_depth"].as<std::size_t>();

    // LoopbackNoc test fixture deque capacity (per direction).
    p.loopback_noc_req_depth = root["loopback_noc"]["req_depth"].as<std::size_t>();
    p.loopback_noc_rsp_depth = root["loopback_noc"]["rsp_depth"].as<std::size_t>();

    // MetaBuffer per-AXI-ID outstanding-request depth.
    p.meta_buffer_per_id_depth = root["meta_buffer"]["per_id_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel
