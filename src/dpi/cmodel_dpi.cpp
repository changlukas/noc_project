// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-wrap {set_inputs,tick,get_outputs} handlers + per-instance *_create
// lifecycle (chandle ABI). Handle validation via REQUIRE_HANDLE.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "handle_block.hpp"
#include "wrap/flit_bytes.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nsu_wrap.hpp"
#include "wrap/router_wrap.hpp"

#include "axi/types.hpp"  // ni::cmodel::axi::DATA_BYTES
#include "wrap/perf_collector.hpp"
#include "ni_flit_constants.h"  // ni::FLIT_WIDTH
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// DPI marshalling assumes a fixed wire format. The packing/unpacking helpers
// below (unpack_flit, pack_flit, unpack_data256, pack_data256, pack_addr64)
// hardcode word counts and bit shifts for the current spec defaults:
//   FLIT_WIDTH = 408 bits → svBitVecVal[13] words
//   DATA_BYTES = 32       → svBitVecVal[8]  words (256-bit data bus)
//   ADDR_WIDTH = 64 bits  → svBitVecVal[2]  words (pack_addr64)
// If a future constants.yaml change widens any of these, compile fails here
// and the DPI pack/unpack must be parameterized before the build can proceed.
// ---------------------------------------------------------------------------
static_assert(::ni::FLIT_WIDTH == 408,
              "cmodel_dpi pack/unpack assumes FLIT_WIDTH = 408 bits "
              "(svBitVecVal[13]); reparameterize unpack_flit/pack_flit if widened");
static_assert(::ni::cmodel::axi::DATA_BYTES == 32,
              "cmodel_dpi pack/unpack assumes 256-bit data bus (DATA_BYTES = 32, "
              "svBitVecVal[8]); reparameterize unpack_data256/pack_data256 if widened");
// ADDR_WIDTH=64 → 2 svBitVecVal words. pack_addr64 hardcodes the 32/32 split.
static_assert(::ni::width::AXI_ADDR_WIDTH == 64,
              "cmodel_dpi pack_addr64 assumes ADDR_WIDTH = 64 bits "
              "(svBitVecVal[2]); reparameterize pack_addr64 if widened");

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

// Flit marshalling helpers — shared by NMU/NSU/Router DPI handlers.
//
// Flit packing convention: svBitVecVal[FLIT_VEC_WORDS] where FLIT_VEC_WORDS =
// ceil(FLIT_WIDTH / 32) = 13. Words are little-endian: word[0] carries bits
// [31:0], word[12] carries bits [407:384] in its low 24 bits.

using ni::cmodel::wrap::FLIT_BYTES;
using ni::cmodel::wrap::FLIT_VEC_WORDS;
using ni::cmodel::wrap::FlitBytes;

namespace {

// Unpack svBitVecVal[FLIT_VEC_WORDS] → FlitBytes (little-endian within each word).
FlitBytes unpack_flit(const svBitVecVal* vec) {
    FlitBytes b{};
    for (int w = 0; w < FLIT_VEC_WORDS; ++w) {
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < FLIT_BYTES) {
                b[idx] = static_cast<uint8_t>((vec[w] >> (byte * 8)) & 0xFF);
            }
        }
    }
    return b;
}

// Pack FlitBytes → svBitVecVal[FLIT_VEC_WORDS] (little-endian within each word).
void pack_flit(const FlitBytes& b, svBitVecVal* vec) {
    for (int w = 0; w < FLIT_VEC_WORDS; ++w) {
        vec[w] = 0;
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < FLIT_BYTES) {
                vec[w] |= static_cast<uint32_t>(b[idx]) << (byte * 8);
            }
        }
    }
}

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

// Router DPI handlers — per-node (Task 3, router-channel split).
// One RouterWrap owns ONE node's REQ+RSP routers at coordinate (x,0).
// Pins split into NMU/NSU-facing (NI edge) + per-network LINK (pulse credit).

using ni::cmodel::wrap::RouterInputs;
using ni::cmodel::wrap::RouterOutputs;
using ni::cmodel::wrap::RouterWrap;

extern "C" unsigned long long cmodel_router_create(const char* name, int x_coord, int y_coord,
                                                   int mesh_x_dim, int mesh_y_dim, int num_vc) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_router_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_router_create, 0ull) {
        auto adapter = std::make_unique<RouterWrap>();
        adapter->init(static_cast<uint8_t>(x_coord), static_cast<uint8_t>(y_coord),
                      static_cast<uint8_t>(mesh_x_dim), static_cast<uint8_t>(mesh_y_dim),
                      static_cast<uint8_t>(num_vc));
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

extern "C" void cmodel_router_set_inputs(
    unsigned long long ctx, svBit req_in_valid, svBitVecVal* req_in_flit,
    svBitVecVal* req_in_credit_return, svBit rsp_in_valid, svBitVecVal* rsp_in_flit,
    svBitVecVal* rsp_in_credit_return, svBitVecVal* link_req_out_credit,
    svBitVecVal* link_req_in_valid, svBitVecVal* link_req_in_flit, svBitVecVal* link_rsp_out_credit,
    svBitVecVal* link_rsp_in_valid, svBitVecVal* link_rsp_in_flit) {
    DPI_BOUNDARY_BEGIN(cmodel_router_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_set_inputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        const uint8_t nvc = r->num_vc();
        RouterInputs in{};
        in.req_in_valid = static_cast<bool>(req_in_valid);
        in.req_in_flit = unpack_flit(req_in_flit);
        in.req_in_credit_return = unpack_vc_credit<VcCreditVec>(req_in_credit_return, nvc);
        in.rsp_in_valid = static_cast<bool>(rsp_in_valid);
        in.rsp_in_flit = unpack_flit(rsp_in_flit);
        in.rsp_in_credit_return = unpack_vc_credit<VcCreditVec>(rsp_in_credit_return, nvc);
        // LINK face: per-direction arrays (port-major). valid = bit per port in
        // one word; flit = FLIT_VEC_WORDS per port; credit = one word per port.
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            in.link_req_in_valid[p] = ((link_req_in_valid[0] >> p) & 0x1u) != 0;
            in.link_rsp_in_valid[p] = ((link_rsp_in_valid[0] >> p) & 0x1u) != 0;
            in.link_req_in_flit[p] = unpack_flit(link_req_in_flit + p * FLIT_VEC_WORDS);
            in.link_rsp_in_flit[p] = unpack_flit(link_rsp_in_flit + p * FLIT_VEC_WORDS);
            in.link_req_out_credit[p] = unpack_vc_credit<VcCreditVec>(link_req_out_credit + p, nvc);
            in.link_rsp_out_credit[p] = unpack_vc_credit<VcCreditVec>(link_rsp_out_credit + p, nvc);
        }
        r->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_router_set_inputs);
}

extern "C" void cmodel_router_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_router_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_tick");
        static_cast<RouterWrap*>(_h->adapter.get())->tick();
    }
    DPI_BOUNDARY_END(cmodel_router_tick);
}

extern "C" void cmodel_router_get_outputs(
    unsigned long long ctx, svBit* req_out_valid, svBitVecVal* req_out_flit,
    svBitVecVal* req_out_credit_return, svBit* rsp_out_valid, svBitVecVal* rsp_out_flit,
    svBitVecVal* rsp_out_credit_return, svBitVecVal* link_req_out_valid,
    svBitVecVal* link_req_out_flit, svBitVecVal* link_req_in_credit,
    svBitVecVal* link_rsp_out_valid, svBitVecVal* link_rsp_out_flit,
    svBitVecVal* link_rsp_in_credit) {
    DPI_BOUNDARY_BEGIN(cmodel_router_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_get_outputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        const uint8_t nvc = r->num_vc();
        RouterOutputs out{};
        r->get_outputs(out);
        *req_out_valid = static_cast<svBit>(out.req_out_valid);
        pack_flit(out.req_out_flit, req_out_flit);
        pack_vc_credit(out.req_out_credit_return, nvc, req_out_credit_return);
        *rsp_out_valid = static_cast<svBit>(out.rsp_out_valid);
        pack_flit(out.rsp_out_flit, rsp_out_flit);
        pack_vc_credit(out.rsp_out_credit_return, nvc, rsp_out_credit_return);
        // LINK face: per-direction arrays (port-major). valid = bit per port in
        // one word; flit = FLIT_VEC_WORDS per port; credit = one word per port.
        link_req_out_valid[0] = 0;
        link_rsp_out_valid[0] = 0;
        for (std::size_t p = 0; p < ROUTER_LINK_PORTS; ++p) {
            if (out.link_req_out_valid[p]) link_req_out_valid[0] |= (1u << p);
            if (out.link_rsp_out_valid[p]) link_rsp_out_valid[0] |= (1u << p);
            pack_flit(out.link_req_out_flit[p], link_req_out_flit + p * FLIT_VEC_WORDS);
            pack_flit(out.link_rsp_out_flit[p], link_rsp_out_flit + p * FLIT_VEC_WORDS);
            pack_vc_credit(out.link_req_in_credit[p], nvc, link_req_in_credit + p);
            pack_vc_credit(out.link_rsp_in_credit[p], nvc, link_rsp_in_credit + p);
        }
    }
    DPI_BOUNDARY_END(cmodel_router_get_outputs);
}

// Shared AXI beat marshalling helpers — used by the NMU/NSU DPI handlers below.
//
// Packing convention (multi-bit fields, little-endian word order):
//   64-bit addr  : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data : words[0..7] (32 bytes, 8 x uint32_t)

namespace {

// Unpack 256-bit data bus: svBitVecVal[8] → std::array<uint8_t, 32>
std::array<uint8_t, 32> unpack_data256(const svBitVecVal* vec) {
    std::array<uint8_t, 32> out{};
    for (int w = 0; w < 8; ++w) {
        for (int b = 0; b < 4; ++b) {
            out[w * 4 + b] = static_cast<uint8_t>((vec[w] >> (b * 8)) & 0xFF);
        }
    }
    return out;
}

// Pack 256-bit data bus: std::array<uint8_t, 32> → svBitVecVal[8]
void pack_data256(const std::array<uint8_t, 32>& src, svBitVecVal* vec) {
    for (int w = 0; w < 8; ++w) {
        vec[w] = 0;
        for (int b = 0; b < 4; ++b) {
            vec[w] |= static_cast<uint32_t>(src[w * 4 + b]) << (b * 8);
        }
    }
}

// Pack 64-bit address: uint64_t → svBitVecVal[2]
void pack_addr64(uint64_t addr, svBitVecVal* vec) {
    vec[0] = static_cast<uint32_t>(addr & 0xFFFF'FFFFu);
    vec[1] = static_cast<uint32_t>((addr >> 32) & 0xFFFF'FFFFu);
}

}  // namespace

// Nmu DPI handlers — Task 8.
//
// Packing conventions (little-endian word order):
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   408-bit flit    : words[0..12] (51 bytes; unpack_flit/pack_flit defined above)

using ni::cmodel::wrap::NmuInputs;
using ni::cmodel::wrap::NmuOutputs;

static unsigned long long nmu_create_impl(const char* name, int src_id, int num_vc,
                                          ni::cmodel::nmu::RobMode rob_mode,
                                          const char* config_path) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nmu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(nmu_create_impl, 0ull) {
        auto adapter = std::make_unique<NmuWrap>();
        adapter->init(static_cast<uint8_t>(src_id), static_cast<uint8_t>(num_vc), kAxiQueueDepth,
                      rob_mode, config_path);
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

extern "C" unsigned long long cmodel_nmu_create(const char* name, int src_id, int num_vc,
                                                const char* config_path) {
    return nmu_create_impl(name, src_id, num_vc, ni::cmodel::nmu::RobMode::Disabled, config_path);
}

extern "C" unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int num_vc,
                                                   int rob_enabled, const char* config_path) {
    return nmu_create_impl(
        name, src_id, num_vc,
        rob_enabled ? ni::cmodel::nmu::RobMode::Enabled : ni::cmodel::nmu::RobMode::Disabled,
        config_path);
}

extern "C" void cmodel_nmu_set_inputs(
    unsigned long long ctx, svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
    svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst, svBit awlock,
    svBitVecVal* awcache, svBitVecVal* awprot, svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata,
    svBitVecVal* wstrb, svBit wlast, svBit bready, svBit arvalid, svBitVecVal* arid,
    svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst,
    svBit arlock, svBitVecVal* arcache, svBitVecVal* arprot, svBitVecVal* arqos, svBit rready,
    svBit noc_rsp_valid, svBitVecVal* noc_rsp_flit, svBitVecVal* noc_req_credit_return) {
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
        in.wvalid = static_cast<bool>(wvalid);
        in.wdata = unpack_data256(wdata);
        in.wstrb = wstrb[0];
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
        in.noc_rsp_valid = static_cast<bool>(noc_rsp_valid);
        in.noc_rsp_flit = unpack_flit(noc_rsp_flit);
        in.noc_req_credit_return =
            unpack_vc_credit<NmuVcCreditVec>(noc_req_credit_return, nmu->num_vc());
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
                                       svBit* noc_req_valid, svBitVecVal* noc_req_flit,
                                       svBitVecVal* noc_rsp_credit_return) {
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
        pack_data256(out.rdata, rdata);
        rresp[0] = out.rresp & 0x3u;
        *rlast = static_cast<svBit>(out.rlast);
        *noc_req_valid = static_cast<svBit>(out.noc_req_valid);
        pack_flit(out.noc_req_flit, noc_req_flit);
        pack_vc_credit(out.noc_rsp_credit_return, nmu->num_vc(), noc_rsp_credit_return);
    }
    DPI_BOUNDARY_END(cmodel_nmu_get_outputs);
}

// Nsu DPI handlers — Task 9.
//
// Direction inversion vs. Nmu:
//   set_inputs receives noc_req_flit (NoC consumer) + AXI master ready signals / B/R.
//   get_outputs produces noc_rsp_flit (NoC producer) + AXI master AW/W/AR beats.
// Packing conventions mirror cmodel_nmu_*:
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   408-bit flit    : words[0..12] (51 bytes; unpack_flit/pack_flit defined above)

using ni::cmodel::wrap::NsuInputs;
using ni::cmodel::wrap::NsuOutputs;

extern "C" unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nsu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_nsu_create, 0ull) {
        auto adapter = std::make_unique<NsuWrap>();
        adapter->init(static_cast<uint8_t>(src_id), static_cast<uint8_t>(num_vc));
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

extern "C" void cmodel_nsu_set_inputs(unsigned long long ctx, svBit noc_req_valid,
                                      svBitVecVal* noc_req_flit, svBitVecVal* noc_rsp_credit_return,
                                      svBit awready, svBit wready, svBit bvalid, svBitVecVal* bid,
                                      svBitVecVal* bresp, svBit arready, svBit rvalid,
                                      svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp,
                                      svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_set_inputs");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        NsuInputs in{};
        in.noc_req_valid = static_cast<bool>(noc_req_valid);
        in.noc_req_flit = unpack_flit(noc_req_flit);
        in.noc_rsp_credit_return =
            unpack_vc_credit<NsuVcCreditVec>(noc_rsp_credit_return, nsu->num_vc());
        in.awready = static_cast<bool>(awready);
        in.wready = static_cast<bool>(wready);
        in.bvalid = static_cast<bool>(bvalid);
        in.bid = static_cast<uint8_t>(bid[0] & 0xFF);
        in.bresp = static_cast<uint8_t>(bresp[0] & 0x3);
        in.arready = static_cast<bool>(arready);
        in.rvalid = static_cast<bool>(rvalid);
        in.rid = static_cast<uint8_t>(rid[0] & 0xFF);
        in.rdata = unpack_data256(rdata);
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
    unsigned long long ctx, svBit* noc_rsp_valid, svBitVecVal* noc_rsp_flit,
    svBitVecVal* noc_req_credit_return, svBit* awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
    svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst, svBit* awlock,
    svBitVecVal* awcache, svBitVecVal* awprot, svBitVecVal* awqos, svBit* wvalid,
    svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast, svBit* bready, svBit* arvalid,
    svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
    svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache, svBitVecVal* arprot,
    svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_get_outputs");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        NsuOutputs out{};
        nsu->get_outputs(out);

        *noc_rsp_valid = static_cast<svBit>(out.noc_rsp_valid);
        pack_flit(out.noc_rsp_flit, noc_rsp_flit);
        pack_vc_credit(out.noc_req_credit_return, nsu->num_vc(), noc_req_credit_return);

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
        pack_data256(out.wdata, wdata);
        wstrb[0] = out.wstrb;
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

void sample_one_router(const std::string& node, ni::cmodel::router::Router& r, const char* plane) {
    using ni::cmodel::router::ROUTER_PORT_COUNT;
    std::size_t in_occ = 0, out_occ = 0;
    for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
        out_occ += r.output_fifo_size(p);
        for (uint8_t vc = 0; vc < r.num_vc(); ++vc) in_occ += r.input_fifo_size(p, vc);
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
        sample_one_router(h->name, r->req_router(), "req");
        sample_one_router(h->name, r->rsp_router(), "rsp");
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
// c_model state (router FIFO/credit/wormhole, NMU/NSU stage occupancy and
// in-flight trackers) so a deadlocked run localizes the stuck hop without
// waveforms. Read-only; the tb watchdog calls it once before $fatal.
// ---------------------------------------------------------------------------
namespace {

const char* kPortName[] = {"LOCAL", "N", "E", "S", "W"};

void dump_one_router(const std::string& name, const char* net, ni::cmodel::router::Router& r) {
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
            std::printf("[FABRIC-DUMP] %s.%s wormhole[%s] locked_input=%s locked_vc=%u\n",
                        name.c_str(), net, kPortName[p], kPortName[*lock],
                        static_cast<unsigned>(r.wormhole_locked_vc(p).value_or(255)));
        }
    }
}

void dump_one_router_wrap(const std::string& name, RouterWrap& rw) {
    dump_one_router(name, "req", rw.req_router());
    dump_one_router(name, "rsp", rw.rsp_router());
    const uint8_t nvc = rw.num_vc();
    for (std::size_t p = 0; p < ni::cmodel::router::ROUTER_PORT_COUNT; ++p) {
        const std::size_t req_ej = rw.req_eject_buffered(p);
        const std::size_t rsp_ej = rw.rsp_eject_buffered(p);
        if (req_ej > 0)
            std::printf("[FABRIC-DUMP] %s.req eject[%s]=%zu\n", name.c_str(), kPortName[p], req_ej);
        if (rsp_ej > 0)
            std::printf("[FABRIC-DUMP] %s.rsp eject[%s]=%zu\n", name.c_str(), kPortName[p], rsp_ej);
        for (uint8_t vc = 0; vc < nvc; ++vc) {
            const std::size_t req_cp = rw.req_credit_pending(p, vc);
            const std::size_t rsp_cp = rw.rsp_credit_pending(p, vc);
            if (req_cp > 0)
                std::printf("[FABRIC-DUMP] %s.req credit_pending[%s][vc%u]=%zu\n", name.c_str(),
                            kPortName[p], vc, req_cp);
            if (rsp_cp > 0)
                std::printf("[FABRIC-DUMP] %s.rsp credit_pending[%s][vc%u]=%zu\n", name.c_str(),
                            kPortName[p], vc, rsp_cp);
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
        name.c_str(), sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_AW),
        sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_W),
        sa->stage_occupancy(NiPath::NmuReq, 0, ni::AXI_CH_AR),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_AW),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_W),
        sa->stage_occupancy(NiPath::NmuReq, 1, ni::AXI_CH_AR),
        sa->stage_occupancy(NiPath::NmuReq, 2, 0));
    std::printf("[FABRIC-DUMP] %s rsp_stage s0[b,r]=%zu,%zu s1[b,r]=%zu,%zu s2[b,r]=%zu,%zu\n",
                name.c_str(), sa->stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_B),
                sa->stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_R),
                sa->stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_B),
                sa->stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_R),
                sa->stage_occupancy(NiPath::NmuRsp, 2, ni::AXI_CH_B),
                sa->stage_occupancy(NiPath::NmuRsp, 2, ni::AXI_CH_R));
    std::printf("[FABRIC-DUMP] %s rob write_outstanding=%zu read_outstanding=%zu", name.c_str(),
                sa->rob().write_occupancy(), sa->rob().read_occupancy());
    for (uint8_t vc = 0; vc < nw.num_vc(); ++vc) {
        std::printf(" req_credit_avail[vc%u]=%d", vc, sa->req_credit_avail(vc) ? 1 : 0);
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
                name.c_str(), sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_AW),
                sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_W),
                sa->stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_AR),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_AW),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_W),
                sa->stage_occupancy(NiPath::NsuReq, 1, ni::AXI_CH_AR));
    std::printf("[FABRIC-DUMP] %s rsp_stage s0[b,r]=%zu,%zu s1[b,r]=%zu,%zu s2=%zu", name.c_str(),
                sa->stage_occupancy(NiPath::NsuRsp, 0, ni::AXI_CH_B),
                sa->stage_occupancy(NiPath::NsuRsp, 0, ni::AXI_CH_R),
                sa->stage_occupancy(NiPath::NsuRsp, 1, ni::AXI_CH_B),
                sa->stage_occupancy(NiPath::NsuRsp, 1, ni::AXI_CH_R),
                sa->stage_occupancy(NiPath::NsuRsp, 2, 0));
    for (uint8_t vc = 0; vc < nw.num_vc(); ++vc) {
        std::printf(" rsp_credit_avail[vc%u]=%d", vc, sa->rsp_credit_avail(vc) ? 1 : 0);
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
