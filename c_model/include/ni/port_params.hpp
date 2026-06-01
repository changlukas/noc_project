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
  std::size_t aw_queue_depth;
  std::size_t w_queue_depth;
  std::size_t ar_queue_depth;
  std::size_t b_queue_depth;
  std::size_t r_queue_depth;
};

// Load PortParams from c_model/config/port_params.yaml.
//
// The current YAML schema is a single flat set of 5 keys shared by both
// sides (NMU AxiSlavePort and NSU AxiMasterPort use the same depths). The
// `side` parameter is reserved for a future schema that splits per-side
// blocks (e.g. `nmu:` / `nsu:` sub-maps); today it is unused. Callers should
// still pass "nmu" or "nsu" so a future split is a header-only change.
inline PortParams load_port_params_yaml(const std::string& path,
                                        const std::string& /*side*/) {
  auto root = YAML::LoadFile(path);
  PortParams p{};
  p.aw_queue_depth = root["aw_queue_depth"].as<std::size_t>();
  p.w_queue_depth  = root["w_queue_depth"].as<std::size_t>();
  p.ar_queue_depth = root["ar_queue_depth"].as<std::size_t>();
  p.b_queue_depth  = root["b_queue_depth"].as<std::size_t>();
  p.r_queue_depth  = root["r_queue_depth"].as<std::size_t>();
  return p;
}

}  // namespace ni::cmodel
