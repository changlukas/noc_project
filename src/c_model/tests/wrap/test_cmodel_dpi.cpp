// c_model/tests/wrap/test_cmodel_dpi.cpp
// Single ordered TEST_F walking the DPI session state machine
// (Uninitialized → Initialized → Finalized) + per-instance handle guards.
// Each negative assertion calls check_and_clear_error() to drain the latch.
#include "cmodel_dpi.h"
#include "handle_block.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <string>

namespace ni::cmodel::wrap {
extern std::atomic<int> g_dpi_error_code;
extern std::string g_dpi_error_msg;
}  // namespace ni::cmodel::wrap

namespace {

void check_and_clear_error(int expected_code) {
    const char* msg = nullptr;
    int code = cmodel_check_error(&msg);
    EXPECT_EQ(code, expected_code) << "msg: " << (msg ? msg : "<null>");
    ni::cmodel::wrap::g_dpi_error_code.store(CMODEL_DPI_OK);
    ni::cmodel::wrap::g_dpi_error_msg.clear();
}

class CmodelDpiLifecycleTest : public ::testing::Test {};

TEST_F(CmodelDpiLifecycleTest, walk_session_state_machine) {
    // Case: cmodel_finalize from UNINITIALIZED → no-op (no error, state unchanged).
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cmodel_init → succeeds (no-arg; no scenario load, perf reset only).
    cmodel_init();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cmodel_init called twice → second rejected (REINIT_FORBIDDEN).
    cmodel_init();
    check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);

    // Case: registry-miss guard — garbage void* (non-registry) on a cycle op →
    // membership guard fires, no SIGSEGV.
    unsigned long long garbage = 0xDEADBEEFCAFEull;
    cmodel_nmu_tick(garbage);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // === NMU multi-instance independence ===

    // Case: create 2 NMU adapters — distinct void* + both validate as live.
    unsigned long long nmu_a = cmodel_nmu_create("nmu_a", 0, /*num_vc=*/1, /*config_path=*/nullptr);
    unsigned long long nmu_b = cmodel_nmu_create("nmu_b", 0, /*num_vc=*/1, /*config_path=*/nullptr);
    ASSERT_NE(nmu_a, 0ull);
    ASSERT_NE(nmu_b, 0ull);
    EXPECT_NE(nmu_a, nmu_b);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: type-guard — an NMU handle passed to cmodel_nsu_tick (WrapType
    // mismatch) → HERMETIC_VIOLATION; the type tag rejects the wrong wrap.
    cmodel_nsu_tick(nmu_a);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // === NSU case ===

    // Case: nsu_create after init succeeds.
    unsigned long long nsu_handle = cmodel_nsu_create("nsu_test", 0, /*num_vc=*/1);
    ASSERT_NE(nsu_handle, 0ull);
    check_and_clear_error(CMODEL_DPI_OK);

    // === FINALIZED phase ===

    // Case: finalize from INITIALIZED → registry destroyed, state = FINALIZED.
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cycle op on stale ctx after finalize → registry-miss (registry
    // emptied by finalize) → HERMETIC_VIOLATION.
    cmodel_nmu_tick(nmu_a);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // Case: finalize twice → second is no-op.
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cmodel_init after finalize → REINIT_FORBIDDEN (terminal state).
    cmodel_init();
    check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);
}

}  // namespace
