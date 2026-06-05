// Stage 5b DPI bridge — lifecycle handlers + global error state.
// Per-shell {set_inputs,tick,get_outputs} handler bodies added by Tasks 7-11.

#include "cmodel_dpi.h"
#include "dpi_boundary_macros.h"
#include "cosim2/loopback_noc_shell_adapter.hpp"
#include "cosim2/master_shell_adapter.hpp"
// Tasks 9-11 add includes for their adapters here.

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
std::unique_ptr<MasterShellAdapter>      g_master_adapter;
// Task 9 adds g_slave_adapter
// Task 10 adds g_nmu_adapter
// Task 11 adds g_nsu_adapter

}  // namespace ni::cmodel::cosim2

using namespace ni::cmodel::cosim2;

extern "C" void cmodel_init(const char* scenario_yaml_path) {
    DPI_BOUNDARY_BEGIN(cmodel_init) {
        // Reset all existing singletons + error state (idempotent per spec §5.3)
        g_loopback_adapter.reset();
        g_master_adapter.reset();
        g_dpi_error_code.store(CMODEL_DPI_OK);
        g_dpi_error_msg.clear();

        // Parse scenario (validates +inject mode if present)
        auto scenario = ni::cmodel::axi::load_scenario(std::string(scenario_yaml_path));

        // Construct fresh adapters into local unique_ptrs (strong exception guarantee)
        auto loop = std::make_unique<LoopbackNocShellAdapter>();
        loop->init();

        auto master = std::make_unique<MasterShellAdapter>();
        master->init(std::string(scenario_yaml_path));
        master->configure_inject(scenario.config.inject);
        // Tasks 9-11: build 3 more local unique_ptr adapters

        // Commit (all-or-nothing)
        g_loopback_adapter = std::move(loop);
        g_master_adapter   = std::move(master);
        // Tasks 9-11: g_xxx_adapter = std::move(xxx);
    }
    DPI_BOUNDARY_END(cmodel_init);
}

extern "C" void cmodel_finalize(void) {
    DPI_BOUNDARY_BEGIN(cmodel_finalize) {
        g_loopback_adapter.reset();
        g_master_adapter.reset();
        // Tasks 9-11: reset other 3 singletons
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

// Tasks 9-11 append their handler bodies.

// AxiMaster DPI handlers — Task 8.
//
// Packing convention (multi-bit fields, little-endian word order):
//   8-bit  id/attr  : word[0] low byte
//   64-bit addr     : word[0] = bits[31:0], word[1] = bits[63:32]
//   256-bit data    : words[0..7] (32 bytes, 8 x uint32_t)
//   32-bit wstrb    : word[0]
//   2-bit resp/attr : word[0] low 2 bits

using ni::cmodel::cosim2::MasterInputs;
using ni::cmodel::cosim2::MasterOutputs;
using ni::cmodel::cosim2::AXI_DATA_BYTES;

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

extern "C" void cmodel_master_set_inputs(svBit awready, svBit wready, svBit arready,
                                          svBit bvalid, svBitVecVal* bid, svBitVecVal* bresp,
                                          svBit rvalid, svBitVecVal* rid, svBitVecVal* rdata,
                                          svBitVecVal* rresp, svBit rlast) {
    DPI_BOUNDARY_BEGIN(cmodel_master_set_inputs) {
        if (!g_master_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_master_set_inputs: g_master_adapter null";
            return;
        }
        MasterInputs in{};
        in.awready = static_cast<bool>(awready);
        in.wready  = static_cast<bool>(wready);
        in.arready = static_cast<bool>(arready);
        in.bvalid  = static_cast<bool>(bvalid);
        in.bid     = static_cast<uint8_t>(bid[0] & 0xFF);
        in.bresp   = static_cast<uint8_t>(bresp[0] & 0x3);
        in.rvalid  = static_cast<bool>(rvalid);
        in.rid     = static_cast<uint8_t>(rid[0] & 0xFF);
        in.rdata   = unpack_data256(rdata);
        in.rresp   = static_cast<uint8_t>(rresp[0] & 0x3);
        in.rlast   = static_cast<bool>(rlast);
        g_master_adapter->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_master_set_inputs);
}

extern "C" void cmodel_master_tick(void) {
    DPI_BOUNDARY_BEGIN(cmodel_master_tick) {
        if (!g_master_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_master_tick: g_master_adapter null";
            return;
        }
        g_master_adapter->tick();
    }
    DPI_BOUNDARY_END(cmodel_master_tick);
}

extern "C" void cmodel_master_get_outputs(svBit*       awvalid, svBitVecVal* awid,
                                           svBitVecVal* awaddr,  svBitVecVal* awlen,
                                           svBitVecVal* awsize,  svBitVecVal* awburst,
                                           svBitVecVal* awlock,  svBitVecVal* awcache,
                                           svBitVecVal* awprot,  svBitVecVal* awqos,
                                           svBit*       wvalid,  svBitVecVal* wdata,
                                           svBitVecVal* wstrb,   svBit*       wlast,
                                           svBit*       bready,
                                           svBit*       arvalid, svBitVecVal* arid,
                                           svBitVecVal* araddr,  svBitVecVal* arlen,
                                           svBitVecVal* arsize,  svBitVecVal* arburst,
                                           svBitVecVal* arlock,  svBitVecVal* arcache,
                                           svBitVecVal* arprot,  svBitVecVal* arqos,
                                           svBit*       rready) {
    DPI_BOUNDARY_BEGIN(cmodel_master_get_outputs) {
        if (!g_master_adapter) {
            g_dpi_error_code.store(CMODEL_DPI_ERR_NOT_INITIALIZED);
            g_dpi_error_msg = "cmodel_master_get_outputs: g_master_adapter null";
            return;
        }
        MasterOutputs out{};
        g_master_adapter->get_outputs(out);

        *awvalid    = static_cast<svBit>(out.awvalid);
        awid[0]     = out.awid;
        pack_addr64(out.awaddr, awaddr);
        awlen[0]    = out.awlen;
        awsize[0]   = out.awsize;
        awburst[0]  = out.awburst;
        awlock[0]   = out.awlock;
        awcache[0]  = out.awcache;
        awprot[0]   = out.awprot;
        awqos[0]    = out.awqos;

        *wvalid     = static_cast<svBit>(out.wvalid);
        pack_data256(out.wdata, wdata);
        wstrb[0]    = out.wstrb;
        *wlast      = static_cast<svBit>(out.wlast);

        *bready     = static_cast<svBit>(out.bready);

        *arvalid    = static_cast<svBit>(out.arvalid);
        arid[0]     = out.arid;
        pack_addr64(out.araddr, araddr);
        arlen[0]    = out.arlen;
        arsize[0]   = out.arsize;
        arburst[0]  = out.arburst;
        arlock[0]   = out.arlock;
        arcache[0]  = out.arcache;
        arprot[0]   = out.arprot;
        arqos[0]    = out.arqos;

        *rready     = static_cast<svBit>(out.rready);
    }
    DPI_BOUNDARY_END(cmodel_master_get_outputs);
}
