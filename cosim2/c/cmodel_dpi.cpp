// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-shell {set_inputs,tick,get_outputs} handler bodies added by Tasks 7-11.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "cosim2/loopback_noc_shell_adapter.hpp"
// Tasks 8-11 add includes for their adapters here.

#include "axi/scenario_parser.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace ni::cmodel::cosim2 {

std::atomic<int> g_dpi_error_code{CMODEL_DPI_OK};
std::string      g_dpi_error_msg;

// 5 singleton ShellAdapter pointers — populated by cmodel_init.
// Hermetic: each handler accesses ONLY its own singleton.
std::unique_ptr<LoopbackNocShellAdapter> g_loopback_adapter;
// Task 8 adds g_master_adapter
// Task 9 adds g_slave_adapter
// Task 10 adds g_nmu_adapter
// Task 11 adds g_nsu_adapter

}  // namespace ni::cmodel::cosim2

using namespace ni::cmodel::cosim2;

extern "C" void cmodel_init(const char* scenario_yaml_path) {
    DPI_BOUNDARY_BEGIN(cmodel_init) {
        // Reset all existing singletons + error state (idempotent per spec §5.3)
        g_loopback_adapter.reset();
        g_dpi_error_code.store(CMODEL_DPI_OK);
        g_dpi_error_msg.clear();

        // Parse scenario (validates +inject mode if present)
        auto scenario = ni::cmodel::axi::load_scenario(std::string(scenario_yaml_path));
        (void)scenario;  // unused for now; Tasks 8-11 wire it into AxiMaster

        // Construct fresh adapters into local unique_ptrs (strong exception guarantee)
        auto loop = std::make_unique<LoopbackNocShellAdapter>();
        loop->init();
        // Tasks 8-11: build 4 more local unique_ptr adapters

        // Commit (all-or-nothing)
        g_loopback_adapter = std::move(loop);
        // Tasks 8-11: g_xxx_adapter = std::move(xxx);
    }
    DPI_BOUNDARY_END(cmodel_init);
}

extern "C" void cmodel_finalize(void) {
    DPI_BOUNDARY_BEGIN(cmodel_finalize) {
        g_loopback_adapter.reset();
        // Tasks 8-11: reset other 4 singletons
    }
    DPI_BOUNDARY_END(cmodel_finalize);
}

extern "C" int cmodel_check_error(const char** msg) {
    // No try/catch — this IS the error reporting boundary
    *msg = g_dpi_error_msg.c_str();
    return g_dpi_error_code.load();
}

// LoopbackNoc DPI handlers — Task 7.
//
// Flit packing convention: svBitVecVal[FLIT_VEC_WORDS] where FLIT_VEC_WORDS =
// ceil(FLIT_WIDTH / 32) = 13. Words are little-endian: word[0] carries bits
// [31:0], word[12] carries bits [407:384] in its low 24 bits.

using ni::cmodel::cosim2::FLIT_VEC_WORDS;
using ni::cmodel::cosim2::FLIT_BYTES;
using ni::cmodel::cosim2::FlitBytes;
using ni::cmodel::cosim2::LoopbackNocInputs;
using ni::cmodel::cosim2::LoopbackNocOutputs;

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

extern "C" void cmodel_loopback_noc_set_inputs(svBit req_in_valid, svBitVecVal* req_in_flit,
                                                svBit req_in_credit_return,
                                                svBit rsp_in_valid, svBitVecVal* rsp_in_flit,
                                                svBit rsp_in_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_set_inputs) {
        if (!g_loopback_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_loopback_noc_set_inputs: g_loopback_adapter null";
            return;
        }
        LoopbackNocInputs in{};
        in.req_in_valid         = static_cast<bool>(req_in_valid);
        in.req_in_flit          = unpack_flit(req_in_flit);
        in.req_in_credit_return = static_cast<bool>(req_in_credit_return);
        in.rsp_in_valid         = static_cast<bool>(rsp_in_valid);
        in.rsp_in_flit          = unpack_flit(rsp_in_flit);
        in.rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return);
        g_loopback_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_loopback_noc_set_inputs);
}

extern "C" void cmodel_loopback_noc_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_tick) {
        if (!g_loopback_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_loopback_noc_tick: g_loopback_adapter null";
            return;
        }
        g_loopback_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_loopback_noc_tick);
}

extern "C" void cmodel_loopback_noc_get_outputs(svBit* req_out_valid,
                                                 svBitVecVal* req_out_flit,
                                                 svBit* req_out_credit_return,
                                                 svBit* rsp_out_valid,
                                                 svBitVecVal* rsp_out_flit,
                                                 svBit* rsp_out_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_loopback_noc_get_outputs) {
        if (!g_loopback_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_loopback_noc_get_outputs: g_loopback_adapter null";
            return;
        }
        LoopbackNocOutputs out{};
        g_loopback_adapter->get_outputs(out);
        *req_out_valid         = static_cast<svBit>(out.req_out_valid);
        pack_flit(out.req_out_flit, req_out_flit);
        *req_out_credit_return = static_cast<svBit>(out.req_out_credit_return);
        *rsp_out_valid         = static_cast<svBit>(out.rsp_out_valid);
        pack_flit(out.rsp_out_flit, rsp_out_flit);
        *rsp_out_credit_return = static_cast<svBit>(out.rsp_out_credit_return);
    }
    DPI_BOUNDARY_END(cmodel_loopback_noc_get_outputs);
}

// Tasks 8-11 append their handler bodies.
