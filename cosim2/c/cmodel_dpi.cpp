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

// Task 7 appends LoopbackNoc handler bodies here.
// Tasks 8-11 append their handler bodies.
