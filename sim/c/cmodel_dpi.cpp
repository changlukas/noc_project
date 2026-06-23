// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-wrap {set_inputs,tick,get_outputs} handlers + per-instance *_create
// lifecycle (chandle ABI). Handle validation via REQUIRE_HANDLE.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "handle_block.hpp"
#include "wrap/channel_model_wrap.hpp"
#include "wrap/master_wrap.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nsu_wrap.hpp"
#include "wrap/router_wrap.hpp"
#include "wrap/slave_wrap.hpp"

#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "axi/types.hpp"  // ni::cmodel::axi::DATA_BYTES
#include "wrap/perf_collector.hpp"
#include "ni_flit_constants.h"  // ni::FLIT_WIDTH
#include <atomic>
#include <cstdint>
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

std::size_t g_ever_created_master = 0;  // bumped on each cmodel_master_create

// Cached scenario + original YAML path — read by *_create handlers in Tasks 6-9.
// Scenario struct lacks a `path` field so the literal path string is stashed
// separately. Both are immutable after cmodel_init success.
ni::cmodel::axi::Scenario g_scenario;
std::string g_scenario_yaml_path;

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

// Real scoreboard — wired to MasterWrap callbacks in cmodel_master_create.
std::unique_ptr<ni::cmodel::axi::Scoreboard> g_scoreboard;

// Perf collector — reset on cmodel_init; populated via cmodel_perf_* DPI calls.
static ni::cmodel::wrap::PerfCollector g_perf;

}  // namespace ni::cmodel::wrap

using namespace ni::cmodel::wrap;

extern "C" void cmodel_init(const char* scenario_yaml_path) {
    // Session state machine guard.
    if (g_session_state == SessionState::Initialized ||
        g_session_state == SessionState::Finalized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_REINIT_FORBIDDEN,
                             "cmodel_init: session already initialized or finalized");
        return;
    }
    // Retry from UNINITIALIZED: clear prior latch before parsing.
    g_dpi_error_code.store(CMODEL_DPI_OK);
    g_dpi_error_msg.clear();

    DPI_BOUNDARY_BEGIN(cmodel_init) {
        auto scenario = ni::cmodel::axi::load_scenario(std::string(scenario_yaml_path));
        g_scenario = std::move(scenario);
        g_scenario_yaml_path = scenario_yaml_path;
        g_scoreboard = std::make_unique<ni::cmodel::axi::Scoreboard>();
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

        g_scoreboard.reset();
        g_ever_created_master = 0;

        g_session_state = SessionState::Finalized;
    }
    DPI_BOUNDARY_END(cmodel_finalize);
}

extern "C" int cmodel_check_error(const char** msg) {
    // No try/catch — this IS the error reporting boundary
    *msg = g_dpi_error_msg.c_str();
    return g_dpi_error_code.load();
}

// cmodel_done — returns 1 when all live MasterWrap instances report
// done(). Returns 0 if no master was ever created (non-vacuous guard) or if
// any master has not yet completed all in-flight transactions.
extern "C" int cmodel_done(void) {
    using namespace ni::cmodel::wrap;
    if (g_session_state != SessionState::Initialized) return 0;
    if (g_ever_created_master == 0) return 0;
    for (HandleBlock* h : g_handle_registry) {
        if (h->type != WrapType::Master) continue;
        auto* m = static_cast<MasterWrap*>(h->adapter.get());
        if (!m->done()) return 0;
    }
    return 1;
}

// cmodel_scoreboard_clean — returns 1 when the scoreboard has no mismatches.
// g_scoreboard is wired via MasterWrap on_write_completed /
// on_read_observed callbacks in cmodel_master_create.
extern "C" int cmodel_scoreboard_clean(void) {
    if (!g_scoreboard) return 1;
    return (g_scoreboard->mismatch_count() == 0) ? 1 : 0;
}

// cmodel_dump_scoreboard — print scoreboard stats + mismatch log + per-master
// read-dump file path to stderr. Iterates g_handle_registry for all live
// Master handles. Safe to call multiple times; safe before init / after
// finalize (scoreboard pointer check guards the null case).
extern "C" void cmodel_dump_scoreboard(void) {
    using namespace ni::cmodel::wrap;
    DPI_BOUNDARY_BEGIN(cmodel_dump_scoreboard) {
        if (g_scoreboard) {
            std::fprintf(stderr, "[scoreboard] %zu reads checked, %zu mismatches\n",
                         g_scoreboard->reads_checked(), g_scoreboard->mismatch_count());
            for (const auto& msg : g_scoreboard->mismatch_report()) {
                std::fprintf(stderr, "  %s\n", msg.c_str());
            }
        }
        for (HandleBlock* h : g_handle_registry) {
            if (h->type != WrapType::Master) continue;
            auto* m = static_cast<MasterWrap*>(h->adapter.get());
            std::fprintf(stderr, "[dump] master=%s read-dump file: %s\n", h->name.c_str(),
                         m->read_dump_path().c_str());
        }
    }
    DPI_BOUNDARY_END(cmodel_dump_scoreboard);
}

// Master count + reads-checked, for tb_top's non-vacuous PASS guard.
extern "C" int cmodel_master_count(void) {
    return static_cast<int>(g_ever_created_master);
}
extern "C" int cmodel_reads_checked(void) {
    if (!g_scoreboard) return 0;
    return static_cast<int>(g_scoreboard->reads_checked());
}

// ChannelModel DPI handlers — Task 7.
//
// Flit packing convention: svBitVecVal[FLIT_VEC_WORDS] where FLIT_VEC_WORDS =
// ceil(FLIT_WIDTH / 32) = 13. Words are little-endian: word[0] carries bits
// [31:0], word[12] carries bits [407:384] in its low 24 bits.

using ni::cmodel::wrap::ChannelModelInputs;
using ni::cmodel::wrap::ChannelModelOutputs;
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

}  // namespace

extern "C" unsigned long long cmodel_channel_model_create(const char* name) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_channel_model_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_channel_model_create, 0ull) {
        auto adapter = std::make_unique<ChannelModelWrap>();
        adapter->init();
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::ChannelModel), WrapType::ChannelModel,
            HandleState::Live, std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<ChannelModelWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_channel_model_create);
}

extern "C" void cmodel_channel_model_set_inputs(unsigned long long ctx, svBit req_in_valid,
                                                svBitVecVal* req_in_flit,
                                                svBit req_in_credit_return, svBit rsp_in_valid,
                                                svBitVecVal* rsp_in_flit,
                                                svBit rsp_in_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::ChannelModel, "cmodel_channel_model_set_inputs");
        auto* cm = static_cast<ChannelModelWrap*>(_h->adapter.get());
        ChannelModelInputs in{};
        in.req_in_valid = static_cast<bool>(req_in_valid);
        in.req_in_flit = unpack_flit(req_in_flit);
        in.req_in_credit_return = static_cast<bool>(req_in_credit_return);
        in.rsp_in_valid = static_cast<bool>(rsp_in_valid);
        in.rsp_in_flit = unpack_flit(rsp_in_flit);
        in.rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return);
        cm->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_channel_model_set_inputs);
}

extern "C" void cmodel_channel_model_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_tick) {
        REQUIRE_HANDLE(ctx, WrapType::ChannelModel, "cmodel_channel_model_tick");
        auto* cm = static_cast<ChannelModelWrap*>(_h->adapter.get());
        cm->tick();
    }
    DPI_BOUNDARY_END(cmodel_channel_model_tick);
}

extern "C" void cmodel_channel_model_get_outputs(unsigned long long ctx, svBit* req_out_valid,
                                                 svBitVecVal* req_out_flit,
                                                 svBit* req_out_credit_return, svBit* rsp_out_valid,
                                                 svBitVecVal* rsp_out_flit,
                                                 svBit* rsp_out_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::ChannelModel, "cmodel_channel_model_get_outputs");
        auto* cm = static_cast<ChannelModelWrap*>(_h->adapter.get());
        ChannelModelOutputs out{};
        cm->get_outputs(out);
        *req_out_valid = static_cast<svBit>(out.req_out_valid);
        pack_flit(out.req_out_flit, req_out_flit);
        *req_out_credit_return = static_cast<svBit>(out.req_out_credit_return);
        *rsp_out_valid = static_cast<svBit>(out.rsp_out_valid);
        pack_flit(out.rsp_out_flit, rsp_out_flit);
        *rsp_out_credit_return = static_cast<svBit>(out.rsp_out_credit_return);
    }
    DPI_BOUNDARY_END(cmodel_channel_model_get_outputs);
}

// Router DPI handlers — per-node (Task 3, router-channel split).
// One RouterWrap owns ONE node's REQ+RSP routers at coordinate (x,0).
// Pins split into NMU/NSU-facing (NI edge) + per-network LINK (pulse credit).

using ni::cmodel::wrap::RouterInputs;
using ni::cmodel::wrap::RouterOutputs;
using ni::cmodel::wrap::RouterWrap;

extern "C" unsigned long long cmodel_router_create(const char* name, int x_coord) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_router_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_router_create, 0ull) {
        auto adapter = std::make_unique<RouterWrap>();
        adapter->init(static_cast<uint8_t>(x_coord));
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

extern "C" void cmodel_router_set_inputs(unsigned long long ctx, svBit req_in_valid,
                                         svBitVecVal* req_in_flit, svBit req_in_credit_return,
                                         svBit rsp_in_valid, svBitVecVal* rsp_in_flit,
                                         svBit rsp_in_credit_return, svBit link_req_out_credit,
                                         svBit link_req_in_valid, svBitVecVal* link_req_in_flit,
                                         svBit link_rsp_out_credit, svBit link_rsp_in_valid,
                                         svBitVecVal* link_rsp_in_flit) {
    DPI_BOUNDARY_BEGIN(cmodel_router_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_set_inputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        RouterInputs in{};
        in.req_in_valid = static_cast<bool>(req_in_valid);
        in.req_in_flit = unpack_flit(req_in_flit);
        in.req_in_credit_return = static_cast<bool>(req_in_credit_return);
        in.rsp_in_valid = static_cast<bool>(rsp_in_valid);
        in.rsp_in_flit = unpack_flit(rsp_in_flit);
        in.rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return);
        in.link_req_out_credit = static_cast<bool>(link_req_out_credit);
        in.link_req_in_valid = static_cast<bool>(link_req_in_valid);
        in.link_req_in_flit = unpack_flit(link_req_in_flit);
        in.link_rsp_out_credit = static_cast<bool>(link_rsp_out_credit);
        in.link_rsp_in_valid = static_cast<bool>(link_rsp_in_valid);
        in.link_rsp_in_flit = unpack_flit(link_rsp_in_flit);
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

extern "C" void cmodel_router_get_outputs(unsigned long long ctx, svBit* req_out_valid,
                                          svBitVecVal* req_out_flit, svBit* req_out_credit_return,
                                          svBit* rsp_out_valid, svBitVecVal* rsp_out_flit,
                                          svBit* rsp_out_credit_return, svBit* link_req_out_valid,
                                          svBitVecVal* link_req_out_flit, svBit* link_req_in_credit,
                                          svBit* link_rsp_out_valid, svBitVecVal* link_rsp_out_flit,
                                          svBit* link_rsp_in_credit) {
    DPI_BOUNDARY_BEGIN(cmodel_router_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Router, "cmodel_router_get_outputs");
        auto* r = static_cast<RouterWrap*>(_h->adapter.get());
        RouterOutputs out{};
        r->get_outputs(out);
        *req_out_valid = static_cast<svBit>(out.req_out_valid);
        pack_flit(out.req_out_flit, req_out_flit);
        *req_out_credit_return = static_cast<svBit>(out.req_out_credit_return);
        *rsp_out_valid = static_cast<svBit>(out.rsp_out_valid);
        pack_flit(out.rsp_out_flit, rsp_out_flit);
        *rsp_out_credit_return = static_cast<svBit>(out.rsp_out_credit_return);
        *link_req_out_valid = static_cast<svBit>(out.link_req_out_valid);
        pack_flit(out.link_req_out_flit, link_req_out_flit);
        *link_req_in_credit = static_cast<svBit>(out.link_req_in_credit);
        *link_rsp_out_valid = static_cast<svBit>(out.link_rsp_out_valid);
        pack_flit(out.link_rsp_out_flit, link_rsp_out_flit);
        *link_rsp_in_credit = static_cast<svBit>(out.link_rsp_in_credit);
    }
    DPI_BOUNDARY_END(cmodel_router_get_outputs);
}

// AxiMaster DPI handlers — Task 8.
//
// Packing convention (multi-bit fields, little-endian word order):
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   2-bit resp/attr : word[0] low 2 bits

using ni::cmodel::wrap::AXI_DATA_BYTES;
using ni::cmodel::wrap::MasterInputs;
using ni::cmodel::wrap::MasterOutputs;

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

extern "C" unsigned long long cmodel_master_create(const char* name, const char* scenario_path) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_master_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_master_create, 0ull) {
        const std::string dump_path = "master_wrap_read_dump_" + std::string(name) + ".txt";
        auto adapter = std::make_unique<MasterWrap>();
        adapter->init(std::string(scenario_path), dump_path,
                      g_scenario.config.max_outstanding_write,
                      g_scenario.config.max_outstanding_read);
        adapter->configure_inject(g_scenario.config.inject);

        // Wire scoreboard callbacks. g_scoreboard outlives all masters
        // (single global; created in cmodel_init, destroyed in cmodel_finalize).
        auto* sb_raw = g_scoreboard.get();
        auto resp_str = [](ni::cmodel::axi::Resp r) -> const char* {
            switch (r) {
                case ni::cmodel::axi::Resp::OKAY:
                    return "OKAY";
                case ni::cmodel::axi::Resp::EXOKAY:
                    return "EXOKAY";
                case ni::cmodel::axi::Resp::SLVERR:
                    return "SLVERR";
                case ni::cmodel::axi::Resp::DECERR:
                    return "DECERR";
            }
            return "?";
        };
        adapter->on_write_completed([sb_raw, resp_str](const ni::cmodel::axi::WriteResult& wr) {
            sb_raw->handle_write_completed(wr, wr.data, wr.strb_per_beat);
            std::fprintf(stderr, "[axi-w] id=0x%x addr=0x%llx len=%u size=%u resp=%s\n",
                         static_cast<unsigned>(wr.id), static_cast<unsigned long long>(wr.addr),
                         static_cast<unsigned>(wr.len), static_cast<unsigned>(wr.size),
                         resp_str(wr.resp));
        });
        adapter->on_read_observed([sb_raw, resp_str](const ni::cmodel::axi::ReadResult& rr) {
            sb_raw->handle_read_observed(rr);
            const uint8_t first_byte = rr.data.empty() ? 0 : rr.data[0];
            std::fprintf(stderr,
                         "[axi-r] id=0x%x addr=0x%llx len=%u size=%u resp=%s data[0]=0x%02x\n",
                         static_cast<unsigned>(rr.id), static_cast<unsigned long long>(rr.addr),
                         static_cast<unsigned>(rr.len), static_cast<unsigned>(rr.size),
                         resp_str(rr.resp), static_cast<unsigned>(first_byte));
        });

        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Master), WrapType::Master, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<MasterWrap*>(p); })};
        g_handle_registry.insert(h);
        ++g_ever_created_master;
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_master_create);
}

extern "C" void cmodel_master_set_inputs(unsigned long long ctx, svBit awready, svBit wready,
                                         svBit arready, svBit bvalid, svBitVecVal* bid,
                                         svBitVecVal* bresp, svBit rvalid, svBitVecVal* rid,
                                         svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_master_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Master, "cmodel_master_set_inputs");
        auto* master = static_cast<MasterWrap*>(_h->adapter.get());
        MasterInputs in{};
        in.awready = static_cast<bool>(awready);
        in.wready = static_cast<bool>(wready);
        in.arready = static_cast<bool>(arready);
        in.bvalid = static_cast<bool>(bvalid);
        in.bid = static_cast<uint8_t>(bid[0] & 0xFF);
        in.bresp = static_cast<uint8_t>(bresp[0] & 0x3);
        in.rvalid = static_cast<bool>(rvalid);
        in.rid = static_cast<uint8_t>(rid[0] & 0xFF);
        in.rdata = unpack_data256(rdata);
        in.rresp = static_cast<uint8_t>(rresp[0] & 0x3);
        in.rlast = static_cast<bool>(rlast);
        master->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_master_set_inputs);
}

extern "C" void cmodel_master_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_master_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Master, "cmodel_master_tick");
        auto* master = static_cast<MasterWrap*>(_h->adapter.get());
        master->tick();
    }
    DPI_BOUNDARY_END(cmodel_master_tick);
}

extern "C" void cmodel_master_get_outputs(unsigned long long ctx, svBit* awvalid, svBitVecVal* awid,
                                          svBitVecVal* awaddr, svBitVecVal* awlen,
                                          svBitVecVal* awsize, svBitVecVal* awburst, svBit* awlock,
                                          svBitVecVal* awcache, svBitVecVal* awprot,
                                          svBitVecVal* awqos, svBit* wvalid, svBitVecVal* wdata,
                                          svBitVecVal* wstrb, svBit* wlast, svBit* bready,
                                          svBit* arvalid, svBitVecVal* arid, svBitVecVal* araddr,
                                          svBitVecVal* arlen, svBitVecVal* arsize,
                                          svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache,
                                          svBitVecVal* arprot, svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_master_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Master, "cmodel_master_get_outputs");
        auto* master = static_cast<MasterWrap*>(_h->adapter.get());
        MasterOutputs out{};
        master->get_outputs(out);

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
    DPI_BOUNDARY_END(cmodel_master_get_outputs);
}

// AxiSlave DPI handlers — Task 9.
//
// Packing convention mirrors cmodel_master_*:
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   2-bit resp/attr : word[0] low 2 bits

using ni::cmodel::wrap::SlaveInputs;
using ni::cmodel::wrap::SlaveOutputs;

// unpack_data256 and pack_addr64 are defined in the master block above (same
// anonymous namespace); they are reused here for the slave handlers.

extern "C" unsigned long long cmodel_slave_create(const char* name, const char* scenario_path) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_slave_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_slave_create, 0ull) {
        auto variant = ni::cmodel::axi::load_scenario(std::string(scenario_path));
        auto adapter = std::make_unique<SlaveWrap>();
        adapter->init(variant.config.memory_base, variant.config.memory_size,
                      g_scenario.config.write_latency, g_scenario.config.read_latency);
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Slave), WrapType::Slave, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<SlaveWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_slave_create);
}

extern "C" void cmodel_slave_set_inputs(
    unsigned long long ctx, svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
    svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst, svBit awlock,
    svBitVecVal* awcache, svBitVecVal* awprot, svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata,
    svBitVecVal* wstrb, svBit wlast, svBit arvalid, svBitVecVal* arid, svBitVecVal* araddr,
    svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst, svBit arlock,
    svBitVecVal* arcache, svBitVecVal* arprot, svBitVecVal* arqos, svBit bready, svBit rready) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_set_inputs) {
        REQUIRE_HANDLE(ctx, WrapType::Slave, "cmodel_slave_set_inputs");
        auto* slave = static_cast<SlaveWrap*>(_h->adapter.get());
        SlaveInputs in{};
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
        in.bready = static_cast<bool>(bready);
        in.rready = static_cast<bool>(rready);
        slave->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_slave_set_inputs);
}

extern "C" void cmodel_slave_tick(unsigned long long ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_tick) {
        REQUIRE_HANDLE(ctx, WrapType::Slave, "cmodel_slave_tick");
        auto* slave = static_cast<SlaveWrap*>(_h->adapter.get());
        slave->tick();
    }
    DPI_BOUNDARY_END(cmodel_slave_tick);
}

extern "C" void cmodel_slave_get_outputs(unsigned long long ctx, svBit* awready, svBit* wready,
                                         svBit* arready, svBit* bvalid, svBitVecVal* bid,
                                         svBitVecVal* bresp, svBit* rvalid, svBitVecVal* rid,
                                         svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Slave, "cmodel_slave_get_outputs");
        auto* slave = static_cast<SlaveWrap*>(_h->adapter.get());
        SlaveOutputs out{};
        slave->get_outputs(out);

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
    }
    DPI_BOUNDARY_END(cmodel_slave_get_outputs);
}

// Nmu DPI handlers — Task 8.
//
// Packing conventions mirror cmodel_slave_*:
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   408-bit flit    : words[0..12] (51 bytes; unpack_flit/pack_flit defined above)

using ni::cmodel::wrap::NmuInputs;
using ni::cmodel::wrap::NmuOutputs;

extern "C" unsigned long long cmodel_nmu_create(const char* name, int src_id) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nmu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_nmu_create, 0ull) {
        auto adapter = std::make_unique<NmuWrap>();
        adapter->init(static_cast<uint8_t>(src_id));
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Nmu), WrapType::Nmu, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<NmuWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(cmodel_nmu_create);
}

extern "C" void cmodel_nmu_set_inputs(
    unsigned long long ctx, svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
    svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst, svBit awlock,
    svBitVecVal* awcache, svBitVecVal* awprot, svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata,
    svBitVecVal* wstrb, svBit wlast, svBit bready, svBit arvalid, svBitVecVal* arid,
    svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst,
    svBit arlock, svBitVecVal* arcache, svBitVecVal* arprot, svBitVecVal* arqos, svBit rready,
    svBit noc_rsp_valid, svBitVecVal* noc_rsp_flit, svBit noc_req_credit_return) {
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
        in.noc_req_credit_return = static_cast<bool>(noc_req_credit_return);
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
                                       svBit* noc_rsp_credit_return) {
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
        *noc_rsp_credit_return = static_cast<svBit>(out.noc_rsp_credit_return);
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

extern "C" unsigned long long cmodel_nsu_create(const char* name, int src_id) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nsu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_nsu_create, 0ull) {
        auto adapter = std::make_unique<NsuWrap>();
        adapter->init(static_cast<uint8_t>(src_id));
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
                                      svBitVecVal* noc_req_flit, svBit noc_rsp_credit_return,
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
        in.noc_rsp_credit_return = static_cast<bool>(noc_rsp_credit_return);
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

extern "C" void cmodel_nsu_get_outputs(unsigned long long ctx, svBit* noc_rsp_valid,
                                       svBitVecVal* noc_rsp_flit, svBit* noc_req_credit_return,
                                       svBit* awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
                                       svBitVecVal* awlen, svBitVecVal* awsize,
                                       svBitVecVal* awburst, svBit* awlock, svBitVecVal* awcache,
                                       svBitVecVal* awprot, svBitVecVal* awqos, svBit* wvalid,
                                       svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast,
                                       svBit* bready, svBit* arvalid, svBitVecVal* arid,
                                       svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
                                       svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache,
                                       svBitVecVal* arprot, svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_get_outputs) {
        REQUIRE_HANDLE(ctx, WrapType::Nsu, "cmodel_nsu_get_outputs");
        auto* nsu = static_cast<NsuWrap*>(_h->adapter.get());
        NsuOutputs out{};
        nsu->get_outputs(out);

        *noc_rsp_valid = static_cast<svBit>(out.noc_rsp_valid);
        pack_flit(out.noc_rsp_flit, noc_rsp_flit);
        *noc_req_credit_return = static_cast<svBit>(out.noc_req_credit_return);

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

extern "C" void cmodel_perf_axi_txn(const char* slot, int id, int is_write, long long addr, int len,
                                    int size, long long accept_cyc, long long complete_cyc) {
    g_perf.add_txn(slot, static_cast<uint32_t>(id), is_write != 0, static_cast<uint64_t>(addr),
                   static_cast<uint32_t>(len), static_cast<uint32_t>(size),
                   static_cast<uint64_t>(accept_cyc), static_cast<uint64_t>(complete_cyc));
}

extern "C" void cmodel_perf_axi_backpressure(const char* slot, long long slave_write_idle_cyc,
                                             long long master_read_idle_cyc) {
    g_perf.set_slot_backpressure(slot, static_cast<uint64_t>(slave_write_idle_cyc),
                                 static_cast<uint64_t>(master_read_idle_cyc));
}

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
