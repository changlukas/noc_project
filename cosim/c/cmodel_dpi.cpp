// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-shell {set_inputs,tick,get_outputs} handler bodies added by Tasks 7-11.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "handle_block.hpp"
#include "cosim/channel_model_shell_adapter.hpp"
#include "cosim/master_shell_adapter.hpp"
#include "cosim/nmu_shell_adapter.hpp"
#include "cosim/nsu_shell_adapter.hpp"
#include "cosim/slave_shell_adapter.hpp"

#include "axi/scenario_parser.hpp"
#include "axi/scoreboard.hpp"
#include "axi/types.hpp"        // ni::cmodel::axi::DATA_BYTES
#include "ni_flit_constants.h"  // ni::FLIT_WIDTH
#include <atomic>
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

namespace ni::cmodel::cosim {

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

// validate_handle — resolves void* ctx to a typed HandleBlock* with 5 guards:
//   1. Session state: Uninitialized → ERR_NOT_INITIALIZED.
//   2. Registry membership: unknown pointer → ERR_HERMETIC_VIOLATION.
//   3. Magic sentinel match: bit-flip / aliased ptr → ERR_HERMETIC_VIOLATION.
//   4. Type tag match: wrong shell type → ERR_HERMETIC_VIOLATION.
//   5. Handle liveness: Closed handle (post-destroy) → ERR_HERMETIC_VIOLATION.
// Returns nullptr and sets the error latch on any failure; returns the typed
// block on success.
HandleBlock* validate_handle(void* ctx, ShellType expected, const char* fn_name) {
    // Guard 1 — state-first per spec state-transition table.
    if (g_session_state == SessionState::Uninitialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             std::string(fn_name) + ": session not initialized");
        return nullptr;
    }
    // Guard 2 — registry membership avoids garbage void* deref (SIGSEGV).
    // Post-finalize handles also fail here (registry emptied by finalize) →
    // ERR_HERMETIC_VIOLATION, consistent with the spec test matrix.
    if (!g_handle_registry.count(static_cast<HandleBlock*>(ctx))) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": ctx not in registry");
        return nullptr;
    }
    auto* h = static_cast<HandleBlock*>(ctx);
    // Guard 3 — magic sentinel: magic must equal the stored type tag.
    // Detects memory stomp where the magic field is corrupted but the type
    // field is intact (or vice versa).
    if (h->magic != static_cast<uint32_t>(h->type)) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": magic does not match stored type");
        return nullptr;
    }
    // Guard 4 — type tag: stored type must equal what the handler expected.
    // Detects passing a handle for shell A to a handler for shell B.
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

// 5 singleton ShellAdapter pointers — populated by cmodel_init.
// Hermetic: each handler accesses ONLY its own singleton.
std::unique_ptr<ChannelModelShellAdapter> g_channel_adapter;
std::unique_ptr<MasterShellAdapter> g_master_adapter;
std::unique_ptr<SlaveShellAdapter> g_slave_adapter;
std::unique_ptr<NmuShellAdapter> g_nmu_adapter;
std::unique_ptr<NsuShellAdapter> g_nsu_adapter;

// Real scoreboard — wired to MasterShellAdapter callbacks in cmodel_init.
std::unique_ptr<ni::cmodel::axi::Scoreboard> g_scoreboard;

}  // namespace ni::cmodel::cosim

using namespace ni::cmodel::cosim;

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

        // ====== Preserved singleton-construction block (removed in Task 10) ======
        // Construct fresh adapters into local unique_ptrs (strong exception guarantee)
        auto channel = std::make_unique<ChannelModelShellAdapter>();
        channel->init();

        auto master = std::make_unique<MasterShellAdapter>();
        master->init(g_scenario_yaml_path, "", g_scenario.config.max_outstanding_write,
                     g_scenario.config.max_outstanding_read);
        master->configure_inject(g_scenario.config.inject);

        auto slave = std::make_unique<SlaveShellAdapter>();
        slave->init(g_scenario.config.memory_base, g_scenario.config.memory_size,
                    g_scenario.config.write_latency, g_scenario.config.read_latency);

        auto nmu = std::make_unique<NmuShellAdapter>();
        nmu->init();

        auto nsu = std::make_unique<NsuShellAdapter>();
        nsu->init();

        // Wire real scoreboard to master callbacks.
        // Each callback also prints a one-line transaction summary to stderr so
        // co-sim runs surface per-AXI-transaction activity at runtime, matching
        // the visibility c_model standalone tests give.
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
        master->on_write_completed([sb_raw, resp_str](const ni::cmodel::axi::WriteResult& wr) {
            sb_raw->handle_write_completed(wr, wr.data, wr.strb_per_beat);
            std::fprintf(stderr, "[axi-w] id=0x%x addr=0x%llx len=%u size=%u resp=%s\n",
                         static_cast<unsigned>(wr.id), static_cast<unsigned long long>(wr.addr),
                         static_cast<unsigned>(wr.len), static_cast<unsigned>(wr.size),
                         resp_str(wr.resp));
        });
        master->on_read_observed([sb_raw, resp_str](const ni::cmodel::axi::ReadResult& rr) {
            sb_raw->handle_read_observed(rr);
            // Print first user-data byte as a quick pattern check; ReadResult.data
            // is the packed user-byte buffer (not per-beat).
            const uint8_t first_byte = rr.data.empty() ? 0 : rr.data[0];
            std::fprintf(stderr,
                         "[axi-r] id=0x%x addr=0x%llx len=%u size=%u resp=%s data[0]=0x%02x\n",
                         static_cast<unsigned>(rr.id), static_cast<unsigned long long>(rr.addr),
                         static_cast<unsigned>(rr.len), static_cast<unsigned>(rr.size),
                         resp_str(rr.resp), static_cast<unsigned>(first_byte));
        });

        // Commit (all-or-nothing)
        g_channel_adapter = std::move(channel);
        g_master_adapter = std::move(master);
        g_slave_adapter = std::move(slave);
        g_nmu_adapter = std::move(nmu);
        g_nsu_adapter = std::move(nsu);
        // ====== End preserved block ======

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

        // Preserve existing singleton resets — removed by Task 10 once all
        // per-shell create handlers are in place.
        g_channel_adapter.reset();
        g_master_adapter.reset();
        g_slave_adapter.reset();
        g_nmu_adapter.reset();
        g_nsu_adapter.reset();
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

// cmodel_done — returns 1 when the AxiMaster has submitted all scenario
// transactions and all in-flight completions have been processed.
// Returns 0 if the master adapter is not yet initialised.
extern "C" int cmodel_done(void) {
    if (!g_master_adapter) return 0;
    return g_master_adapter->done() ? 1 : 0;
}

// cmodel_scoreboard_clean — returns 1 when the scoreboard has no mismatches.
// g_scoreboard is wired via MasterShellAdapter on_write_completed /
// on_read_observed callbacks in cmodel_init (T15).
extern "C" int cmodel_scoreboard_clean(void) {
    if (!g_scoreboard) return 1;
    return (g_scoreboard->mismatch_count() == 0) ? 1 : 0;
}

// cmodel_dump_scoreboard — print scoreboard stats + mismatch log + read-dump
// file path to stderr. Called by tb_top.sv before $finish / $fatal so the
// debug info is visible without having to inspect the tmp-dir dump file.
// Safe to call multiple times; safe before init / after finalize (no-ops).
extern "C" void cmodel_dump_scoreboard(void) {
    DPI_BOUNDARY_BEGIN(cmodel_dump_scoreboard) {
        if (g_scoreboard) {
            std::fprintf(stderr, "[scoreboard] %zu reads checked, %zu mismatches\n",
                         g_scoreboard->reads_checked(), g_scoreboard->mismatch_count());
            for (const auto& msg : g_scoreboard->mismatch_report()) {
                std::fprintf(stderr, "  %s\n", msg.c_str());
            }
        }
        if (g_master_adapter) {
            std::fprintf(stderr, "[dump] read-dump file: %s\n",
                         g_master_adapter->read_dump_path().c_str());
        }
    }
    DPI_BOUNDARY_END(cmodel_dump_scoreboard);
}

// ChannelModel DPI handlers — Task 7.
//
// Flit packing convention: svBitVecVal[FLIT_VEC_WORDS] where FLIT_VEC_WORDS =
// ceil(FLIT_WIDTH / 32) = 13. Words are little-endian: word[0] carries bits
// [31:0], word[12] carries bits [407:384] in its low 24 bits.

using ni::cmodel::cosim::ChannelModelInputs;
using ni::cmodel::cosim::ChannelModelOutputs;
using ni::cmodel::cosim::FLIT_BYTES;
using ni::cmodel::cosim::FLIT_VEC_WORDS;
using ni::cmodel::cosim::FlitBytes;

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

extern "C" void* cmodel_channel_model_create(const char* name) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_channel_model_create: not initialized");
        return nullptr;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_channel_model_create, nullptr) {
        auto adapter = std::make_unique<ChannelModelShellAdapter>();
        adapter->init();
        auto* h =
            new HandleBlock{static_cast<uint32_t>(ShellType::ChannelModel), ShellType::ChannelModel,
                            HandleState::Live, std::string(name),
                            std::unique_ptr<void, void (*)(void*)>(adapter.release(), [](void* p) {
                                delete static_cast<ChannelModelShellAdapter*>(p);
                            })};
        g_handle_registry.insert(h);
        return static_cast<void*>(h);
    }
    DPI_BOUNDARY_END_R(cmodel_channel_model_create);
}

extern "C" void cmodel_channel_model_set_inputs(void* ctx, svBit req_in_valid,
                                                svBitVecVal* req_in_flit,
                                                svBit req_in_credit_return, svBit rsp_in_valid,
                                                svBitVecVal* rsp_in_flit,
                                                svBit rsp_in_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_set_inputs) {
        REQUIRE_HANDLE(ctx, ShellType::ChannelModel, "cmodel_channel_model_set_inputs");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
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

extern "C" void cmodel_channel_model_tick(void* ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_tick) {
        REQUIRE_HANDLE(ctx, ShellType::ChannelModel, "cmodel_channel_model_tick");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
        cm->tick();
    }
    DPI_BOUNDARY_END(cmodel_channel_model_tick);
}

extern "C" void cmodel_channel_model_get_outputs(void* ctx, svBit* req_out_valid,
                                                 svBitVecVal* req_out_flit,
                                                 svBit* req_out_credit_return, svBit* rsp_out_valid,
                                                 svBitVecVal* rsp_out_flit,
                                                 svBit* rsp_out_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_get_outputs) {
        REQUIRE_HANDLE(ctx, ShellType::ChannelModel, "cmodel_channel_model_get_outputs");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
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

// Tasks 9-11 append their handler bodies.

// AxiMaster DPI handlers — Task 8.
//
// Packing convention (multi-bit fields, little-endian word order):
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   2-bit resp/attr : word[0] low 2 bits

using ni::cmodel::cosim::AXI_DATA_BYTES;
using ni::cmodel::cosim::MasterInputs;
using ni::cmodel::cosim::MasterOutputs;

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

extern "C" void cmodel_master_set_inputs(svBit awready, svBit wready, svBit arready, svBit bvalid,
                                         svBitVecVal* bid, svBitVecVal* bresp, svBit rvalid,
                                         svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp,
                                         svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_master_set_inputs) {
        REQUIRE_ADAPTER(g_master_adapter, "cmodel_master_set_inputs");
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
        g_master_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_master_set_inputs);
}

extern "C" void cmodel_master_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_master_tick) {
        REQUIRE_ADAPTER(g_master_adapter, "cmodel_master_tick");
        g_master_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_master_tick);
}

extern "C" void cmodel_master_get_outputs(
    svBit* awvalid, svBitVecVal* awid, svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
    svBitVecVal* awburst, svBit* awlock, svBitVecVal* awcache, svBitVecVal* awprot,
    svBitVecVal* awqos, svBit* wvalid, svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast,
    svBit* bready, svBit* arvalid, svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen,
    svBitVecVal* arsize, svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache,
    svBitVecVal* arprot, svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_master_get_outputs) {
        REQUIRE_ADAPTER(g_master_adapter, "cmodel_master_get_outputs");
        MasterOutputs out{};
        g_master_adapter->get_outputs(out);

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

using ni::cmodel::cosim::SlaveInputs;
using ni::cmodel::cosim::SlaveOutputs;

// unpack_data256 and pack_addr64 are defined in the master block above (same
// anonymous namespace); they are reused here for the slave handlers.

extern "C" void cmodel_slave_set_inputs(
    svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
    svBitVecVal* awburst, svBit awlock, svBitVecVal* awcache, svBitVecVal* awprot,
    svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata, svBitVecVal* wstrb, svBit wlast,
    svBit arvalid, svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
    svBitVecVal* arburst, svBit arlock, svBitVecVal* arcache, svBitVecVal* arprot,
    svBitVecVal* arqos, svBit bready, svBit rready) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_set_inputs) {
        REQUIRE_ADAPTER(g_slave_adapter, "cmodel_slave_set_inputs");
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
        g_slave_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_slave_set_inputs);
}

extern "C" void cmodel_slave_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_tick) {
        REQUIRE_ADAPTER(g_slave_adapter, "cmodel_slave_tick");
        g_slave_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_slave_tick);
}

extern "C" void cmodel_slave_get_outputs(svBit* awready, svBit* wready, svBit* arready,
                                         svBit* bvalid, svBitVecVal* bid, svBitVecVal* bresp,
                                         svBit* rvalid, svBitVecVal* rid, svBitVecVal* rdata,
                                         svBitVecVal* rresp, svBit* rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_slave_get_outputs) {
        REQUIRE_ADAPTER(g_slave_adapter, "cmodel_slave_get_outputs");
        SlaveOutputs out{};
        g_slave_adapter->get_outputs(out);

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

// Nmu DPI handlers — Task 10.
//
// Packing conventions mirror cmodel_slave_*:
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   408-bit flit    : words[0..12] (51 bytes; unpack_flit/pack_flit defined above)

using ni::cmodel::cosim::NmuInputs;
using ni::cmodel::cosim::NmuOutputs;

extern "C" void cmodel_nmu_set_inputs(svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
                                      svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst,
                                      svBit awlock, svBitVecVal* awcache, svBitVecVal* awprot,
                                      svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata,
                                      svBitVecVal* wstrb, svBit wlast, svBit bready, svBit arvalid,
                                      svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen,
                                      svBitVecVal* arsize, svBitVecVal* arburst, svBit arlock,
                                      svBitVecVal* arcache, svBitVecVal* arprot, svBitVecVal* arqos,
                                      svBit rready, svBit noc_rsp_valid, svBitVecVal* noc_rsp_flit,
                                      svBit noc_req_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_set_inputs) {
        REQUIRE_ADAPTER(g_nmu_adapter, "cmodel_nmu_set_inputs");
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
        g_nmu_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_nmu_set_inputs);
}

extern "C" void cmodel_nmu_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_tick) {
        REQUIRE_ADAPTER(g_nmu_adapter, "cmodel_nmu_tick");
        g_nmu_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_nmu_tick);
}

extern "C" void cmodel_nmu_get_outputs(svBit* awready, svBit* wready, svBit* arready, svBit* bvalid,
                                       svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid,
                                       svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp,
                                       svBit* rlast, svBit* noc_req_valid,
                                       svBitVecVal* noc_req_flit, svBit* noc_rsp_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_nmu_get_outputs) {
        REQUIRE_ADAPTER(g_nmu_adapter, "cmodel_nmu_get_outputs");
        NmuOutputs out{};
        g_nmu_adapter->get_outputs(out);

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

// Nsu DPI handlers — Task 11.
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

using ni::cmodel::cosim::NsuInputs;
using ni::cmodel::cosim::NsuOutputs;

extern "C" void cmodel_nsu_set_inputs(svBit noc_req_valid, svBitVecVal* noc_req_flit,
                                      svBit noc_rsp_credit_return, svBit awready, svBit wready,
                                      svBit bvalid, svBitVecVal* bid, svBitVecVal* bresp,
                                      svBit arready, svBit rvalid, svBitVecVal* rid,
                                      svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_set_inputs) {
        REQUIRE_ADAPTER(g_nsu_adapter, "cmodel_nsu_set_inputs");
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
        g_nsu_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_nsu_set_inputs);
}

extern "C" void cmodel_nsu_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_tick) {
        REQUIRE_ADAPTER(g_nsu_adapter, "cmodel_nsu_tick");
        g_nsu_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_nsu_tick);
}

extern "C" void cmodel_nsu_get_outputs(
    svBit* noc_rsp_valid, svBitVecVal* noc_rsp_flit, svBit* noc_req_credit_return, svBit* awvalid,
    svBitVecVal* awid, svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
    svBitVecVal* awburst, svBit* awlock, svBitVecVal* awcache, svBitVecVal* awprot,
    svBitVecVal* awqos, svBit* wvalid, svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast,
    svBit* bready, svBit* arvalid, svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen,
    svBitVecVal* arsize, svBitVecVal* arburst, svBit* arlock, svBitVecVal* arcache,
    svBitVecVal* arprot, svBitVecVal* arqos, svBit* rready) {
    DPI_BOUNDARY_BEGIN(cmodel_nsu_get_outputs) {
        REQUIRE_ADAPTER(g_nsu_adapter, "cmodel_nsu_get_outputs");
        NsuOutputs out{};
        g_nsu_adapter->get_outputs(out);

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
