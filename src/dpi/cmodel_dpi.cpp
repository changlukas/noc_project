// DPI bridge — lifecycle handlers + global error state.
// Per-wrap {set_inputs,tick,get_outputs} handlers + per-instance *_create
// lifecycle (longint-handle ABI; chandle was rejected by VCS as a module
// port). Handle validation via REQUIRE_HANDLE.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "dpi_marshal.hpp"
#include "handle_block.hpp"
#include "ni_params.h"
#include "wrap/dat_merge_wrap.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nsu_wrap.hpp"
#include "wrap/router_wrap.hpp"

#include "wrap/perf_collector.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// DPI marshalling word counts (<net>FlitMarshal::VEC_WORDS, DATA_VEC_WORDS,
// WSTRB_VEC_WORDS) and the flit tail mask are derived from
// ni::NOC_{REQ,RSP,DAT}_FLIT_WIDTH / axi::DATA_WIDTH in dpi_marshal.hpp, not
// pinned to today's values. Widening any of those constants does not require
// editing the pack/unpack helpers below.
// ---------------------------------------------------------------------------

namespace ni::cmodel::wrap {

std::atomic<int> g_dpi_error_code{CMODEL_DPI_OK};
std::string g_dpi_error_msg;

// Session state machine — Uninitialized on startup.
// Session state transitions: cmodel_init → Initialized;
// cmodel_finalize → Finalized; both REINIT_FORBIDDEN-guarded.
enum class SessionState { Uninitialized, Initialized, Finalized };
SessionState g_session_state = SessionState::Uninitialized;

// Process-wide handle registry — definition (declaration is in handle_block.hpp).
// Every live HandleBlock* is inserted here at *_create time and erased at
// *_destroy or cmodel_finalize time.
std::unordered_set<HandleBlock*> g_handle_registry;

// validate_handle — resolves unsigned long long ctx to a typed HandleBlock* with 5 guards:
//   1. Session state: Uninitialized → ERR_NOT_INITIALIZED.
//   2. Registry membership: unknown pointer → ERR_HERMETIC_VIOLATION.
//   3. Magic sentinel match: bit-flip / aliased ptr → ERR_HERMETIC_VIOLATION.
//   4. Type tag match: wrong wrap type → ERR_HERMETIC_VIOLATION.
//   5. Handle liveness: Closed handle (post-destroy) → ERR_HERMETIC_VIOLATION.
// Returns nullptr and sets the error latch on any failure; returns the typed
// block on success.
HandleBlock* validate_handle(unsigned long long ctx, WrapType expected, const char* fn_name) {
    HandleBlock* _ctx_ptr = reinterpret_cast<HandleBlock*>(static_cast<uintptr_t>(ctx));
    // Guard 1 — state-first per spec state-transition table.
    if (g_session_state == SessionState::Uninitialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             std::string(fn_name) + ": session not initialized");
        return nullptr;
    }
    // Guard 2 — registry membership avoids garbage void* deref (SIGSEGV).
    // Post-finalize handles also fail here (registry emptied by finalize) →
    // ERR_HERMETIC_VIOLATION, consistent with the spec test matrix.
    if (!g_handle_registry.count(_ctx_ptr)) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": ctx not in registry");
        return nullptr;
    }
    auto* h = _ctx_ptr;
    // Guard 3 — magic sentinel: magic must equal the stored type tag.
    // Detects memory stomp where the magic field is corrupted but the type
    // field is intact (or vice versa).
    if (h->magic != static_cast<uint32_t>(h->type)) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": magic does not match stored type");
        return nullptr;
    }
    // Guard 4 — type tag: stored type must equal what the handler expected.
    // Detects passing a handle for wrap A to a handler for wrap B.
    if (h->type != expected) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": type mismatch");
        return nullptr;
    }
    // Guard 5 — liveness.
    if (h->state != HandleState::Live) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": handle not live");
        return nullptr;
    }
    return h;
}

// Perf collector — reset on cmodel_init; populated via cmodel_perf_* DPI calls.
static ni::cmodel::wrap::PerfCollector g_perf;

}  // namespace ni::cmodel::wrap

using namespace ni::cmodel::wrap;

extern "C" void cmodel_init(void) {
    // Session state machine guard.
    if (g_session_state == SessionState::Initialized ||
        g_session_state == SessionState::Finalized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_REINIT_FORBIDDEN,
                             "cmodel_init: session already initialized or finalized");
        return;
    }
    // Retry from UNINITIALIZED: clear prior latch.
    g_dpi_error_code.store(CMODEL_DPI_OK);
    g_dpi_error_msg.clear();

    DPI_BOUNDARY_BEGIN(cmodel_init) {
        g_perf = ni::cmodel::wrap::PerfCollector{};
        g_session_state = SessionState::Initialized;
    }
    DPI_BOUNDARY_END(cmodel_init);
}

extern "C" void cmodel_finalize(void) {
    DPI_BOUNDARY_BEGIN(cmodel_finalize) {
        if (g_session_state != SessionState::Initialized) {
            return;  // no-op from UNINITIALIZED or FINALIZED (idempotent)
        }
        // Destroy each handle block. unique_ptr<void, deleter> in HandleBlock
        // ensures the type-erased adapter is properly deleted.
        for (HandleBlock* h : g_handle_registry) {
            delete h;
        }
        g_handle_registry.clear();

        g_session_state = SessionState::Finalized;
    }
    DPI_BOUNDARY_END(cmodel_finalize);
}

extern "C" int cmodel_check_error(const char** msg) {
    // No try/catch — this IS the error reporting boundary
    *msg = g_dpi_error_msg.c_str();
    return g_dpi_error_code.load();
}

// Flit marshalling helpers (ReqFlitMarshal/RspFlitMarshal/DatFlitMarshal) —
// shared by NMU/NSU/Router DPI handlers, defined in dpi_marshal.hpp (word
// count + tail mask derived from ni::NOC_{REQ,RSP,DAT}_FLIT_WIDTH, not pinned
// here).

namespace {

// Unpack a per-VC credit word (bit vc = pulse on VC vc) into a VcCreditVec.
// Only [0 .. num_vc) are read; the rest stay false.
template <typename CreditVec>
CreditVec unpack_vc_credit(const svBitVecVal* word, uint8_t num_vc) {
    CreditVec v{};
    for (uint8_t vc = 0; vc < num_vc; ++vc) {
        v[vc] = ((word[0] >> vc) & 0x1u) != 0;
    }
    return v;
}

// Pack a VcCreditVec into a single per-VC credit word (bit vc = pulse on VC vc).
template <typename CreditVec>
void pack_vc_credit(const CreditVec& v, uint8_t num_vc, svBitVecVal* word) {
    word[0] = 0;
    for (uint8_t vc = 0; vc < num_vc; ++vc) {
        if (v[vc]) word[0] |= (1u << vc);
    }
}

}  // namespace

// Router DPI handlers — per-node.
// One RouterWrap owns ONE node's REQ + RSP + DAT routers at (x_coord,
// y_coord) in an NxM mesh. Every network's pins are ONE uniform per-PORT
// array (ROUTER_LINK_PORTS = LOCAL + N/E/S/W).

using ni::cmodel::wrap::ROUTER_LINK_PORTS;
using ni::cmodel::wrap::RouterInputs;
using ni::cmodel::wrap::RouterOutputs;
using ni::cmodel::wrap::RouterWrap;
using ni::cmodel::wrap::VcCreditVec;

extern "C" unsigned long long cmodel_router_create(const char* name, int x_coord, int y_coord,
                                                   int mesh_x_dim, int mesh_y_dim, int dat_num_vc) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_router_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_router_create, 0ull) {
        auto adapter = std::make_unique<RouterWrap>();
        adapter->init(static_cast<uint8_t>(x_coord), static_cast<uint8_t>(y_coord),
                      static_cast<uint8_t>(mesh_x_dim), static_cast<uint8_t>(mesh_y_dim),
                      static_cast<uint8_t>(dat_num_vc));
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Router), WrapType::Router, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<RouterWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_router_create);
}

// Split one-call-per-network (S3a T5 debug finding — see cmodel_dpi.h). Each
// of these six marshals exactly one flit width; no DPI signature here mixes
// REQ/RSP/DAT unpacked-array element widths anymore.

extern "C" void cmodel_router_req_set_inputs(unsigned long long ctx, svBitVecVal* rx_req_valid,
                                             svBitVecVal* rx_req_flit, svBitVecVal* tx_req_ready) {
    DPI_BOUNDARY_BEGIN(cmodel_router_req_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_req_set_inputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        PerPort<bool> valid{}, ready{};
        PerPort<FlitBytes> flit{};
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            valid[p] = ((rx_req_valid[0] >> p) & 0x1u) != 0;
            ready[p] = ((tx_req_ready[0] >> p) & 0x1u) != 0;
            flit[p] = ReqFlitMarshal::unpack(rx_req_flit + p * ReqFlitMarshal::VEC_WORDS);
        }
        r->set_req_inputs(valid, flit, ready);
    }
    DPI_BOUNDARY_END(cmodel_router_req_set_inputs);
}

extern "C" void cmodel_router_rsp_set_inputs(unsigned long long ctx, svBitVecVal* rx_rsp_valid,
                                             svBitVecVal* rx_rsp_flit, svBitVecVal* tx_rsp_ready) {
    DPI_BOUNDARY_BEGIN(cmodel_router_rsp_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_rsp_set_inputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        PerPort<bool> valid{}, ready{};
        PerPort<FlitBytes> flit{};
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            valid[p] = ((rx_rsp_valid[0] >> p) & 0x1u) != 0;
            ready[p] = ((tx_rsp_ready[0] >> p) & 0x1u) != 0;
            flit[p] = RspFlitMarshal::unpack(rx_rsp_flit + p * RspFlitMarshal::VEC_WORDS);
        }
        r->set_rsp_inputs(valid, flit, ready);
    }
    DPI_BOUNDARY_END(cmodel_router_rsp_set_inputs);
}

extern "C" void cmodel_router_dat_set_inputs(unsigned long long ctx, svBitVecVal* rx_dat_valid,
                                             svBitVecVal* rx_dat_flit,
                                             svBitVecVal* tx_dat_crdvalid) {
    DPI_BOUNDARY_BEGIN(cmodel_router_dat_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_dat_set_inputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        const uint8_t nvc = r->num_vc();
        PerPort<bool> valid{};
        PerPort<FlitBytes> flit{};
        PerPort<VcCreditVec> crdvalid{};
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            valid[p] = ((rx_dat_valid[0] >> p) & 0x1u) != 0;
            flit[p] = DatFlitMarshal::unpack(rx_dat_flit + p * DatFlitMarshal::VEC_WORDS);
            crdvalid[p] = unpack_vc_credit<VcCreditVec>(tx_dat_crdvalid + p, nvc);
        }
        r->set_dat_inputs(valid, flit, crdvalid);
    }
    DPI_BOUNDARY_END(cmodel_router_dat_set_inputs);
}

extern "C" void cmodel_router_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_router_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_tick");
        static_cast<RouterWrap*>(_h->adapter.get())->tick();
    }
    DPI_BOUNDARY_END(cmodel_router_tick);
}

extern "C" void cmodel_router_req_get_outputs(unsigned long long ctx, svBitVecVal* tx_req_valid,
                                              svBitVecVal* tx_req_flit, svBitVecVal* rx_req_ready) {
    DPI_BOUNDARY_BEGIN(cmodel_router_req_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_req_get_outputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        PerPort<bool> valid{}, ready{};
        PerPort<FlitBytes> flit{};
        r->get_req_outputs(valid, flit, ready);
        tx_req_valid[0] = 0;
        rx_req_ready[0] = 0;
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (valid[p]) tx_req_valid[0] |= (1u << p);
            if (ready[p]) rx_req_ready[0] |= (1u << p);
            ReqFlitMarshal::pack(flit[p], tx_req_flit + p * ReqFlitMarshal::VEC_WORDS);
        }
    }
    DPI_BOUNDARY_END(cmodel_router_req_get_outputs);
}

extern "C" void cmodel_router_rsp_get_outputs(unsigned long long ctx, svBitVecVal* tx_rsp_valid,
                                              svBitVecVal* tx_rsp_flit, svBitVecVal* rx_rsp_ready) {
    DPI_BOUNDARY_BEGIN(cmodel_router_rsp_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_rsp_get_outputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        PerPort<bool> valid{}, ready{};
        PerPort<FlitBytes> flit{};
        r->get_rsp_outputs(valid, flit, ready);
        tx_rsp_valid[0] = 0;
        rx_rsp_ready[0] = 0;
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (valid[p]) tx_rsp_valid[0] |= (1u << p);
            if (ready[p]) rx_rsp_ready[0] |= (1u << p);
            RspFlitMarshal::pack(flit[p], tx_rsp_flit + p * RspFlitMarshal::VEC_WORDS);
        }
    }
    DPI_BOUNDARY_END(cmodel_router_rsp_get_outputs);
}

extern "C" void cmodel_router_dat_get_outputs(unsigned long long ctx, svBitVecVal* tx_dat_valid,
                                              svBitVecVal* tx_dat_flit,
                                              svBitVecVal* rx_dat_crdvalid) {
    DPI_BOUNDARY_BEGIN(cmodel_router_dat_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_dat_get_outputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        const uint8_t nvc = r->num_vc();
        PerPort<bool> valid{};
        PerPort<FlitBytes> flit{};
        PerPort<VcCreditVec> crdvalid{};
        r->get_dat_outputs(valid, flit, crdvalid);
        tx_dat_valid[0] = 0;
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (valid[p]) tx_dat_valid[0] |= (1u << p);
            DatFlitMarshal::pack(flit[p], tx_dat_flit + p * DatFlitMarshal::VEC_WORDS);
            pack_vc_credit(crdvalid[p], nvc, rx_dat_crdvalid + p);
        }
    }
    DPI_BOUNDARY_END(cmodel_router_dat_get_outputs);
}

// DatMerge DPI handlers — NI-level DAT LOCAL-port merge point (S3a T5,
// controller ruling). See wrap/dat_merge_wrap.hpp.

using ni::cmodel::wrap::DatMergeInputs;
using ni::cmodel::wrap::DatMergeOutputs;
using ni::cmodel::wrap::DatMergeWrap;

extern "C" unsigned long long cmodel_dat_merge_create(const char* name, int dat_num_vc) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_dat_merge_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_dat_merge_create, 0ull) {
        auto adapter = std::make_unique<DatMergeWrap>();
        adapter->init(static_cast<uint8_t>(dat_num_vc));
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::DatMerge), WrapType::DatMerge, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<DatMergeWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_dat_merge_create);
}

extern "C" void cmodel_dat_merge_set_inputs(unsigned long long ctx, svBit nmu_tx_dat_valid,
                                            svBitVecVal* nmu_tx_dat_flit, svBit nsu_tx_dat_valid,
                                            svBitVecVal* nsu_tx_dat_flit,
                                            svBitVecVal* tx_dat_crdvalid, svBit rx_dat_valid,
                                            svBitVecVal* rx_dat_flit) {
    DPI_BOUNDARY_BEGIN(cmodel_dat_merge_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::DatMerge, "cmodel_dat_merge_set_inputs");
        auto* m = static_cast<DatMergeWrap*>(_h->adapter.get());
        DatMergeInputs in{};
        in.nmu_tx_dat_valid = static_cast<bool>(nmu_tx_dat_valid);
        in.nmu_tx_dat_flit = DatFlitMarshal::unpack(nmu_tx_dat_flit);
        in.nsu_tx_dat_valid = static_cast<bool>(nsu_tx_dat_valid);
        in.nsu_tx_dat_flit = DatFlitMarshal::unpack(nsu_tx_dat_flit);
        in.tx_dat_crdvalid = unpack_vc_credit<VcCreditVec>(tx_dat_crdvalid, m->num_vc());
        in.rx_dat_valid = static_cast<bool>(rx_dat_valid);
        in.rx_dat_flit = DatFlitMarshal::unpack(rx_dat_flit);
        m->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_dat_merge_set_inputs);
}

extern "C" void cmodel_dat_merge_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_dat_merge_tick) {
        REQUIRE_HANDLE(ctx, WrapType::DatMerge, "cmodel_dat_merge_tick");
        static_cast<DatMergeWrap*>(_h->adapter.get())->tick();
    }
    DPI_BOUNDARY_END(cmodel_dat_merge_tick);
}

extern "C" void cmodel_dat_merge_get_outputs(unsigned long long ctx,
                                             svBitVecVal* nmu_tx_dat_crdvalid,
                                             svBit* nmu_rx_dat_valid, svBitVecVal* nmu_rx_dat_flit,
                                             svBitVecVal* nsu_tx_dat_crdvalid,
                                             svBit* nsu_rx_dat_valid, svBitVecVal* nsu_rx_dat_flit,
                                             svBit* tx_dat_valid, svBitVecVal* tx_dat_flit,
                                             svBitVecVal* rx_dat_crdvalid) {
    DPI_BOUNDARY_BEGIN(cmodel_dat_merge_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::DatMerge, "cmodel_dat_merge_get_outputs");
        auto* m = static_cast<DatMergeWrap*>(_h->adapter.get());
        const uint8_t nvc = m->num_vc();
        DatMergeOutputs out{};
        m->get_outputs(out);
        pack_vc_credit(out.nmu_tx_dat_crdvalid, nvc, nmu_tx_dat_crdvalid);
        *nmu_rx_dat_valid = static_cast<svBit>(out.nmu_rx_dat_valid);
        DatFlitMarshal::pack(out.nmu_rx_dat_flit, nmu_rx_dat_flit);
        pack_vc_credit(out.nsu_tx_dat_crdvalid, nvc, nsu_tx_dat_crdvalid);
        *nsu_rx_dat_valid = static_cast<svBit>(out.nsu_rx_dat_valid);
        DatFlitMarshal::pack(out.nsu_rx_dat_flit, nsu_rx_dat_flit);
        *tx_dat_valid = static_cast<svBit>(out.tx_dat_valid);
        DatFlitMarshal::pack(out.tx_dat_flit, tx_dat_flit);
        pack_vc_credit(out.rx_dat_crdvalid, nvc, rx_dat_crdvalid);
    }
    DPI_BOUNDARY_END(cmodel_dat_merge_get_outputs);
}

// Shared AXI beat marshalling helpers (unpack_axi_data/pack_axi_data,
// unpack_wstrb/pack_wstrb, pack_addr64) — defined in dpi_marshal.hpp, word
// counts derived from axi::DATA_WIDTH / axi::DATA_BYTES, not pinned here.

// Nmu DPI handlers.
//
// Packing conventions (little-endian word order):
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : 2 words fixed (pack_addr64; ADDR_WIDTH unchanged this stage)
//   data bus        : DATA_VEC_WORDS words (axi::DATA_BYTES bytes; unpack_axi_data/pack_axi_data)
//   wstrb           : WSTRB_VEC_WORDS words (axi::DATA_BYTES bits; unpack_wstrb/pack_wstrb)
//   REQ/RSP flit    : Req/RspFlitMarshal::VEC_WORDS words, tail-masked
//   DAT flit        : DatFlitMarshal::VEC_WORDS words, tail-masked

using ni::cmodel::wrap::NmuInputs;
using ni::cmodel::wrap::NmuOutputs;

static unsigned long long nmu_create_impl(const char* name, int src_id, int dat_num_vc,
                                          ni::cmodel::nmu::RobMode rob_mode,
                                          const char* config_path, std::size_t b_rob_depth,
                                          std::size_t r_rob_depth, std::size_t max_txns_per_id,
                                          std::size_t outstanding_depth) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nmu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(nmu_create_impl, 0ull) {
        auto adapter = std::make_unique<NmuWrap>();
        adapter->init(config_path, static_cast<uint8_t>(src_id), static_cast<uint8_t>(dat_num_vc),
                      ni::NMU_QUEUE_DEPTH, rob_mode, b_rob_depth, r_rob_depth, max_txns_per_id,
                      outstanding_depth);
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Nmu), WrapType::Nmu, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<NmuWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(nmu_create_impl);
}

extern "C" unsigned long long cmodel_nmu_create(const char* name, int src_id, int dat_num_vc,
                                                const char* config_path) {
    return nmu_create_impl(name, src_id, dat_num_vc, ni::cmodel::nmu::RobMode::Disabled,
                           config_path, ni::NMU_ROB_B_DEPTH, ni::NMU_ROB_R_DEPTH,
                           ni::NMU_MAX_TXNS_PER_ID, ni::NMU_OUTSTANDING_DEPTH);
}

extern "C" unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int dat_num_vc,
                                                   int rob_enabled, int b_rob_depth,
                                                   int r_rob_depth, int max_txns_per_id,
                                                   int outstanding_depth, const char* config_path) {
    return nmu_create_impl(
        name, src_id, dat_num_vc,
        rob_enabled ? ni::cmodel::nmu::RobMode::Enabled : ni::cmodel::nmu::RobMode::Disabled,
        config_path, static_cast<std::size_t>(b_rob_depth), static_cast<std::size_t>(r_rob_depth),
        static_cast<std::size_t>(max_txns_per_id), static_cast<std::size_t>(outstanding_depth));
}

extern "C" void cmodel_nmu_set_inputs(unsigned long long ctx, svBit awvalid, svBitVecVal* awid,
                                      svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
                                      svBitVecVal* awburst, svBit awlock, svBitVecVal* awcache,
                                      svBitVecVal* awprot, svBitVecVal* awqos, svBitVecVal* awuser,
                                      svBit wvalid, svBitVecVal* wdata, svBitVecVal* wstrb,
                                      svBit wlast, svBit bready, svBit arvalid, svBitVecVal* arid,
                                      svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
                                      svBitVecVal* arburst, svBit arlock, svBitVecVal* arcache,
                                      svBitVecVal* arprot, svBitVecVal* arqos, svBit rready,
                                      svBit tx_req_ready, svBit rx_rsp_valid,
                                      svBitVecVal* rx_rsp_flit, svBit rx_dat_valid,
                                      svBitVecVal* rx_dat_flit, svBitVecVal* tx_dat_crdvalid) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nmu, "cmodel_nmu_set_inputs");
        auto* nmu = static_cast<NmuWrap*>(_h->adapter.get());
        NmuInputs in{};
        in.awvalid = static_cast<bool>(awvalid);
        in.awid = static_cast<uint8_t>(awid[0] & 0xFF);
        in.awaddr = static_cast<uint64_t>(awaddr[0]) | (static_cast<uint64_t>(awaddr[1]) << 32);
        in.awlen = static_cast<uint8_t>(awlen[0] & 0xFF);
        in.awsize = static_cast<uint8_t>(awsize[0] & 0x07);
        in.awburst = static_cast<uint8_t>(awburst[0] & 0x03);
        in.awlock = static_cast<uint8_t>(awlock & 0x01);
        in.awcache = static_cast<uint8_t>(awcache[0] & 0x0F);
        in.awprot = static_cast<uint8_t>(awprot[0] & 0x07);
        in.awqos = static_cast<uint8_t>(awqos[0] & 0x0F);
        // AWUSER, 2 words little-endian; mask to the field width so SV padding
        // bits above the field never reach axi::AwBeat::user.
        in.awuser = (static_cast<uint64_t>(awuser[0]) | (static_cast<uint64_t>(awuser[1]) << 32)) &
                    ((uint64_t{1} << ni::AXI_AWUSER_WIDTH) - 1);
        in.wvalid = static_cast<bool>(wvalid);
        in.wdata = unpack_axi_data(wdata);
        in.wstrb = unpack_wstrb(wstrb);
        in.wlast = static_cast<bool>(wlast);
        in.bready = static_cast<bool>(bready);
        in.arvalid = static_cast<bool>(arvalid);
        in.arid = static_cast<uint8_t>(arid[0] & 0xFF);
        in.araddr = static_cast<uint64_t>(araddr[0]) | (static_cast<uint64_t>(araddr[1]) << 32);
        in.arlen = static_cast<uint8_t>(arlen[0] & 0xFF);
        in.arsize = static_cast<uint8_t>(arsize[0] & 0x07);
        in.arburst = static_cast<uint8_t>(arburst[0] & 0x03);
        in.arlock = static_cast<uint8_t>(arlock & 0x01);
        in.arcache = static_cast<uint8_t>(arcache[0] & 0x0F);
        in.arprot = static_cast<uint8_t>(arprot[0] & 0x07);
        in.arqos = static_cast<uint8_t>(arqos[0] & 0x0F);
        in.rready = static_cast<bool>(rready);
        in.tx_req_ready = static_cast<bool>(tx_req_ready);
        in.rx_rsp_valid = static_cast<bool>(rx_rsp_valid);
        in.rx_rsp_flit = RspFlitMarshal::unpack(rx_rsp_flit);
        in.rx_dat_valid = static_cast<bool>(rx_dat_valid);
        in.rx_dat_flit = DatFlitMarshal::unpack(rx_dat_flit);
        in.tx_dat_crdvalid = unpack_vc_credit<NmuVcCreditVec>(tx_dat_crdvalid, nmu->num_vc());
        nmu->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_nmu_set_inputs);
}

extern "C" void cmodel_nmu_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Nmu, "cmodel_nmu_tick");
        auto* nmu = static_cast<NmuWrap*>(_h->adapter.get());
        nmu->tick();
    }
    DPI_BOUNDARY_END(cmodel_nmu_tick);
}

extern "C" void cmodel_nmu_get_outputs(unsigned long long ctx, svBit* awready, svBit* wready,
                                       svBit* arready, svBit* bvalid, svBitVecVal* bid,
                                       svBitVecVal* bresp, svBit* rvalid, svBitVecVal* rid,
                                       svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast,
                                       svBit* tx_req_valid, svBitVecVal* tx_req_flit,
                                       svBit* rx_rsp_ready, svBit* tx_dat_valid,
                                       svBitVecVal* tx_dat_flit, svBitVecVal* rx_dat_crdvalid) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nmu, "cmodel_nmu_get_outputs");
        auto* nmu = static_cast<NmuWrap*>(_h->adapter.get());
        NmuOutputs out{};
        nmu->get_outputs(out);

        *awready = static_cast<svBit>(out.awready);
        *wready = static_cast<svBit>(out.wready);
        *arready = static_cast<svBit>(out.arready);
        *bvalid = static_cast<svBit>(out.bvalid);
        bid[0] = out.bid;
        bresp[0] = out.bresp & 0x3u;
        *rvalid = static_cast<svBit>(out.rvalid);
        rid[0] = out.rid;
        pack_axi_data(out.rdata, rdata);
        rresp[0] = out.rresp & 0x3u;
        *rlast = static_cast<svBit>(out.rlast);
        *tx_req_valid = static_cast<svBit>(out.tx_req_valid);
        ReqFlitMarshal::pack(out.tx_req_flit, tx_req_flit);
        *rx_rsp_ready = static_cast<svBit>(out.rx_rsp_ready);
        *tx_dat_valid = static_cast<svBit>(out.tx_dat_valid);
        DatFlitMarshal::pack(out.tx_dat_flit, tx_dat_flit);
        pack_vc_credit(out.rx_dat_crdvalid, nmu->num_vc(), rx_dat_crdvalid);
    }
    DPI_BOUNDARY_END(cmodel_nmu_get_outputs);
}

// Peak R-RoB slot occupancy (Rob::read_slot_hwm) — sizing telemetry readout.
// 0 if the handle is invalid or RoB is Disabled.
extern "C" unsigned int cmodel_nmu_read_slot_hwm(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN_R(cmodel_nmu_read_slot_hwm, 0u) {
        auto* _h =
            ni::cmodel::wrap::validate_handle(ctx, WrapType::Nmu, "cmodel_nmu_read_slot_hwm");
        if (!_h) return 0u;
        auto* nmu = static_cast<NmuWrap*>(_h->adapter.get());
        auto* sa = nmu->standalone();
        if (!sa) return 0u;
        return static_cast<unsigned int>(sa->rob().read_slot_hwm());
    }
    DPI_BOUNDARY_END_R(cmodel_nmu_read_slot_hwm);
}

// Nsu DPI handlers.
//
// Direction inversion vs. Nmu:
//   set_inputs receives rx_req_* (REQ ingress) + AXI master ready signals / B/R.
//   get_outputs produces tx_rsp_* (RSP egress) + AXI master AW/W/AR beats.
// Packing conventions mirror cmodel_nmu_* (see the word-count comment above).

using ni::cmodel::wrap::NsuInputs;
using ni::cmodel::wrap::NsuOutputs;

extern "C" unsigned long long cmodel_nsu_create(const char* name, int src_id, int dat_num_vc,
                                                int max_unique_ids, int max_outstanding,
                                                const char* config_path) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nsu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_nsu_create, 0ull) {
        auto adapter = std::make_unique<NsuWrap>();
        adapter->init(static_cast<uint8_t>(src_id), static_cast<uint8_t>(dat_num_vc),
                      ni::NSU_QUEUE_DEPTH, static_cast<std::size_t>(max_unique_ids),
                      static_cast<std::size_t>(max_outstanding), config_path);
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Nsu), WrapType::Nsu, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<NsuWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_nsu_create);
}

extern "C" void cmodel_nsu_set_inputs(unsigned long long ctx, svBit rx_req_valid,
                                      svBitVecVal* rx_req_flit, svBit tx_rsp_ready,
                                      svBit rx_dat_valid, svBitVecVal* rx_dat_flit,
                                      svBitVecVal* tx_dat_crdvalid, svBit awready, svBit wready,
                                      svBit bvalid, svBitVecVal* bid, svBitVecVal* bresp,
                                      svBit arready, svBit rvalid, svBitVecVal* rid,
                                      svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_set_inputs");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        NsuInputs in{};
        in.rx_req_valid = static_cast<bool>(rx_req_valid);
        in.rx_req_flit = ReqFlitMarshal::unpack(rx_req_flit);
        in.tx_rsp_ready = static_cast<bool>(tx_rsp_ready);
        in.rx_dat_valid = static_cast<bool>(rx_dat_valid);
        in.rx_dat_flit = DatFlitMarshal::unpack(rx_dat_flit);
        in.tx_dat_crdvalid = unpack_vc_credit<NsuVcCreditVec>(tx_dat_crdvalid, nsu->num_vc());
        in.awready = static_cast<bool>(awready);
        in.wready = static_cast<bool>(wready);
        in.bvalid = static_cast<bool>(bvalid);
        in.bid = static_cast<uint8_t>(bid[0] & 0xFF);
        in.bresp = static_cast<uint8_t>(bresp[0] & 0x3);
        in.arready = static_cast<bool>(arready);
        in.rvalid = static_cast<bool>(rvalid);
        in.rid = static_cast<uint8_t>(rid[0] & 0xFF);
        in.rdata = unpack_axi_data(rdata);
        in.rresp = static_cast<uint8_t>(rresp[0] & 0x3);
        in.rlast = static_cast<bool>(rlast);
        nsu->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_nsu_set_inputs);
}

extern "C" void cmodel_nsu_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_tick");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        nsu->tick();
    }
    DPI_BOUNDARY_END(cmodel_nsu_tick);
}

extern "C" void cmodel_nsu_get_outputs(
    unsigned long long ctx, svBit* rx_req_ready, svBit* tx_rsp_valid, svBitVecVal* tx_rsp_flit,
    svBit* tx_dat_valid, svBitVecVal* tx_dat_flit, svBitVecVal* rx_dat_crdvalid, svBit* awvalid,
    svBitVecVal* awid, svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
    svBitVecVal* awburst, svBit* awlock, svBitVecVal* awcache, svBitVecVal* awprot,
    svBitVecVal* awqos, svBit* wvalid, svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast,
    svBit* bready, svBit* arvalid, svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen,
    svBitVecVal* arsize, svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache,
    svBitVecVal* arprot, svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_get_outputs");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        NsuOutputs out{};
        nsu->get_outputs(out);

        *rx_req_ready = static_cast<svBit>(out.rx_req_ready);
        *tx_rsp_valid = static_cast<svBit>(out.tx_rsp_valid);
        RspFlitMarshal::pack(out.tx_rsp_flit, tx_rsp_flit);
        *tx_dat_valid = static_cast<svBit>(out.tx_dat_valid);
        DatFlitMarshal::pack(out.tx_dat_flit, tx_dat_flit);
        pack_vc_credit(out.rx_dat_crdvalid, nsu->num_vc(), rx_dat_crdvalid);

        *awvalid = static_cast<svBit>(out.awvalid);
        awid[0] = out.awid;
        pack_addr64(out.awaddr, awaddr);
        awlen[0] = out.awlen;
        awsize[0] = out.awsize;
        awburst[0] = out.awburst;
        *awlock = static_cast<svBit>(out.awlock & 0x01u);
        awcache[0] = out.awcache;
        awprot[0] = out.awprot;
        awqos[0] = out.awqos;

        *wvalid = static_cast<svBit>(out.wvalid);
        pack_axi_data(out.wdata, wdata);
        pack_wstrb(out.wstrb, wstrb);
        *wlast = static_cast<svBit>(out.wlast);

        *bready = static_cast<svBit>(out.bready);

        *arvalid = static_cast<svBit>(out.arvalid);
        arid[0] = out.arid;
        pack_addr64(out.araddr, araddr);
        arlen[0] = out.arlen;
        arsize[0] = out.arsize;
        arburst[0] = out.arburst;
        *arlock = static_cast<svBit>(out.arlock & 0x01u);
        arcache[0] = out.arcache;
        arprot[0] = out.arprot;
        arqos[0] = out.arqos;

        *rready = static_cast<svBit>(out.rready);
    }
    DPI_BOUNDARY_END(cmodel_nsu_get_outputs);
}

// Perf DPI handlers — SV monitors push per-txn and end-of-run counters;
// cmodel_perf_sample_tick snapshots router occupancy once per clock cycle.

namespace {

// Generic occupancy sampler: shared shape between router::SimpleRouter
// (REQ/RSP, fixed num_vc=1) and router::Router (DAT, real num_vc) — both
// expose input_fifo_size(port,vc)/output_fifo_size(port).
template <typename RouterT>
void sample_one_router(const std::string& node, RouterT& r, const char* plane, uint8_t num_vc) {
    using ni::cmodel::router::ROUTER_PORT_COUNT;
    std::size_t in_occ = 0, out_occ = 0;
    for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
        out_occ += r.output_fifo_size(p);
        for (uint8_t vc = 0; vc < num_vc; ++vc) in_occ += r.input_fifo_size(p, vc);
    }
    g_perf.sample_router(std::string(plane) + "." + node, in_occ, out_occ);
}

}  // namespace

extern "C" void cmodel_perf_link(const char* name, long long flit_count, long long stall_cyc) {
    g_perf.set_link(name, static_cast<uint64_t>(flit_count), static_cast<uint64_t>(stall_cyc));
}

extern "C" void cmodel_perf_sample_tick() {
    using namespace ni::cmodel::wrap;
    for (HandleBlock* h : g_handle_registry) {
        if (h->type != WrapType::Router) continue;
        auto* r = static_cast<RouterWrap*>(h->adapter.get());
        sample_one_router(h->name, r->req_router(), "req", 1);
        sample_one_router(h->name, r->rsp_router(), "rsp", 1);
        sample_one_router(h->name, r->dat_router(), "dat", r->num_vc());
    }
}

extern "C" void cmodel_perf_dump(const char* path) {
    g_perf.dump(path);
}

extern "C" void cmodel_perf_set_run(const char* scenario, long long total_cyc) {
    g_perf.set_scenario(scenario);
    g_perf.set_window(0, static_cast<uint64_t>(total_cyc));
}

// ---------------------------------------------------------------------------
// Fabric state dump — watchdog forensics. Prints every non-idle piece of
// c_model state (router FIFO/wormhole/credit, NMU/NSU stage occupancy and
// in-flight trackers) so a deadlocked run localizes the stuck hop without
// waveforms. Read-only; the tb watchdog calls it once before $fatal.
// ---------------------------------------------------------------------------
namespace {

const char* kPortName[] = {"LOCAL", "N", "E", "S", "W"};

// REQ/RSP (SimpleRouter, ready/valid, fixed single VC): no credit counter to
// report — ready(port,0) is the live signal instead.
void dump_one_simple_router(const std::string& name, const char* net,
                            ni::cmodel::router::SimpleRouter& r) {
    for (std::size_t p = 0; p < ni::cmodel::router::ROUTER_PORT_COUNT; ++p) {
        const std::size_t occ = r.input_fifo_size(p, 0);
        if (occ > 0) {
            std::printf("[FABRIC-DUMP] %s.%s in_fifo[%s]=%zu ready=%d\n", name.c_str(), net,
                        kPortName[p], occ, r.ready(p, 0) ? 1 : 0);
        }
        const std::size_t out_occ = r.output_fifo_size(p);
        if (out_occ > 0) {
            std::printf("[FABRIC-DUMP] %s.%s out_fifo[%s]=%zu\n", name.c_str(), net, kPortName[p],
                        out_occ);
        }
        if (auto lock = r.wormhole_locked_input(p)) {
            std::printf("[FABRIC-DUMP] %s.%s wormhole[%s] locked_input=%s\n", name.c_str(), net,
                        kPortName[p], kPortName[*lock]);
        }
    }
}

// DAT (Router, credit): unchanged shape from pre-S3a.
void dump_one_credit_router(const std::string& name, const char* net,
                            ni::cmodel::router::Router& r) {
    const uint8_t nvc = r.num_vc();
    for (std::size_t p = 0; p < ni::cmodel::router::ROUTER_PORT_COUNT; ++p) {
        for (uint8_t vc = 0; vc < nvc; ++vc) {
            const std::size_t occ = r.input_fifo_size(p, vc);
            if (occ > 0) {
                std::printf("[FABRIC-DUMP] %s.%s in_fifo[%s][vc%u]=%zu/%zu\n", name.c_str(), net,
                            kPortName[p], vc, occ, r.vc_depth());
            }
            const std::size_t cr = r.credit(p, vc);
            if (cr < r.vc_depth()) {
                std::printf("[FABRIC-DUMP] %s.%s credit[%s][vc%u]=%zu/%zu\n", name.c_str(), net,
                            kPortName[p], vc, cr, r.vc_depth());
            }
        }
        const std::size_t out_occ = r.output_fifo_size(p);
        if (out_occ > 0) {
            std::printf("[FABRIC-DUMP] %s.%s out_fifo[%s]=%zu/%zu\n", name.c_str(), net,
                        kPortName[p], out_occ, r.output_fifo_depth());
        }
        if (auto lock = r.wormhole_locked_input(p)) {
            std::printf(
                "[FABRIC-DUMP] %s.%s wormhole[%s] locked_input=%s locked_input_vc=%u "
                "locked_output_vc=%u\n",
                name.c_str(), net, kPortName[p], kPortName[*lock],
                static_cast<unsigned>(r.wormhole_locked_input_vc(p).value_or(255)),
                static_cast<unsigned>(r.wormhole_locked_output_vc(p).value_or(255)));
        }
    }
}

void dump_one_router_wrap(const std::string& name, RouterWrap& rw) {
    dump_one_simple_router(name, "req", rw.req_router());
    dump_one_simple_router(name, "rsp", rw.rsp_router());
    dump_one_credit_router(name, "dat", rw.dat_router());
    const uint8_t nvc = rw.num_vc();
    for (std::size_t p = 0; p < ni::cmodel::router::ROUTER_PORT_COUNT; ++p) {
        const std::size_t dat_ej = rw.dat_eject_buffered(p);
        if (dat_ej > 0)
            std::printf("[FABRIC-DUMP] %s.dat eject[%s]=%zu\n", name.c_str(), kPortName[p], dat_ej);
        for (uint8_t vc = 0; vc < nvc; ++vc) {
            const std::size_t dat_cp = rw.dat_credit_pending(p, vc);
            if (dat_cp > 0)
                std::printf("[FABRIC-DUMP] %s.dat credit_pending[%s][vc%u]=%zu\n", name.c_str(),
                            kPortName[p], vc, dat_cp);
        }
    }
}

void dump_one_nmu(const std::string& name, NmuWrap& nw) {
    using ni::cmodel::NiPath;
    auto* sa = nw.standalone();
    if (!sa) return;
    auto& port = sa->axi_slave_port();
    std::printf(
        "[FABRIC-DUMP] %s port_q aw=%zu w=%zu ar=%zu b=%zu r=%zu held_b=%d held_r=%d "
        "w_expected=%u\n",
        name.c_str(), port.aw_q_size(), port.w_q_size(), port.ar_q_size(), port.b_q_size(),
        port.r_q_size(), nw.holding_b() ? 1 : 0, nw.holding_r() ? 1 : 0, nw.w_expected());
    std::printf(
        "[FABRIC-DUMP] %s req_stage s0[aw,w,ar]=%zu,%zu,%zu s1[aw,w,ar]=%zu,%zu,%zu s2=%zu\n",
        name.c_str(), sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_NarrowAw),
        sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_NarrowW),
        sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_NarrowAr),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_NarrowAw),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_NarrowW),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_NarrowAr),
        sa->stage_occupancy(NiPath::NmuReq, 2, 0));
    std::printf("[FABRIC-DUMP] %s rsp_stage s0[b,r]=%zu,%zu s1[b,r]=%zu,%zu s2[b,r]=%zu,%zu\n",
                name.c_str(), sa->stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_NarrowB),
                sa->stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_NarrowR),
                sa->stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_NarrowB),
                sa->stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_NarrowR),
                sa->stage_occupancy(NiPath::NmuRsp, 2, ni::AXI_CH_NarrowB),
                sa->stage_occupancy(NiPath::NmuRsp, 2, ni::AXI_CH_NarrowR));
    std::printf("[FABRIC-DUMP] %s rob read_outstanding=%zu req_credit_avail(ready)=%d",
                name.c_str(), sa->rob().read_occupancy(), sa->req_credit_avail() ? 1 : 0);
    for (uint8_t vc = 0; vc < nw.num_vc(); ++vc) {
        std::printf(" dat_req_credit_avail[vc%u]=%d", vc, sa->dat_req_credit_avail(vc) ? 1 : 0);
    }
    std::printf("\n");
}

void dump_one_nsu(const std::string& name, NsuWrap& nw) {
    using ni::cmodel::NiPath;
    auto* sa = nw.standalone();
    if (!sa) return;
    auto& port = sa->axi_master_port();
    std::printf(
        "[FABRIC-DUMP] %s port_q aw=%zu w=%zu ar=%zu b=%zu r=%zu held[aw,w,ar]=%d,%d,%d "
        "outstanding_w=%u expected_r_beats=%u w_pop_budget=%u\n",
        name.c_str(), port.aw_q_size(), port.w_q_size(), port.ar_q_size(), port.b_q_size(),
        port.r_q_size(), nw.holding_aw() ? 1 : 0, nw.holding_w() ? 1 : 0, nw.holding_ar() ? 1 : 0,
        nw.outstanding_w(), nw.expected_r_beats(), nw.w_pop_budget());
    std::printf("[FABRIC-DUMP] %s req_stage s0[aw,w,ar]=%zu,%zu,%zu s1[aw,w,ar]=%zu,%zu,%zu\n",
                name.c_str(), sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_NarrowAw),
                sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_NarrowW),
                sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_NarrowAr),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_NarrowAw),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_NarrowW),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_NarrowAr));
    std::printf("[FABRIC-DUMP] %s rsp_stage s0[b,r]=%zu,%zu s1[b,r]=%zu,%zu s2=%zu", name.c_str(),
                sa->stage_occupancy(NiPath::NsuRsp, 0, ni::AXI_CH_NarrowB),
                sa->stage_occupancy(NiPath::NsuRsp, 0, ni::AXI_CH_NarrowR),
                sa->stage_occupancy(NiPath::NsuRsp, 1, ni::AXI_CH_NarrowB),
                sa->stage_occupancy(NiPath::NsuRsp, 1, ni::AXI_CH_NarrowR),
                sa->stage_occupancy(NiPath::NsuRsp, 2, 0));
    std::printf(" rsp_credit_avail(ready)=%d", sa->rsp_credit_avail() ? 1 : 0);
    for (uint8_t vc = 0; vc < nw.num_vc(); ++vc) {
        std::printf(" dat_rsp_credit_avail[vc%u]=%d", vc, sa->dat_rsp_credit_avail(vc) ? 1 : 0);
    }
    std::printf("\n");
}

}  // namespace

extern "C" void cmodel_dump_fabric_state(void) {
    using namespace ni::cmodel::wrap;
    std::printf("[FABRIC-DUMP] ===== begin (non-idle state only for router lines) =====\n");
    for (HandleBlock* h : g_handle_registry) {
        switch (h->type) {
            case WrapType::Router:
                dump_one_router_wrap(h->name, *static_cast<RouterWrap*>(h->adapter.get()));
                break;
            case WrapType::Nmu:
                dump_one_nmu(h->name, *static_cast<NmuWrap*>(h->adapter.get()));
                break;
            case WrapType::Nsu:
                dump_one_nsu(h->name, *static_cast<NsuWrap*>(h->adapter.get()));
                break;
            default:
                break;
        }
    }
    std::printf("[FABRIC-DUMP] ===== end =====\n");
    std::fflush(stdout);
}
