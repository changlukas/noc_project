// c_model/tests/cosim/test_cmodel_dpi.cpp
// Single ordered TEST_F walking the DPI session state machine
// (Uninitialized → Initialized → Finalized phases).
// Each negative assertion calls check_and_clear_error() to drain the latch.
#include "cmodel_dpi.h"
#include "handle_block.hpp"
#include <atomic>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace ni::cmodel::cosim {
extern std::atomic<int> g_dpi_error_code;
extern std::string g_dpi_error_msg;
}  // namespace ni::cmodel::cosim

namespace {

void check_and_clear_error(int expected_code) {
    const char* msg = nullptr;
    int code = cmodel_check_error(&msg);
    EXPECT_EQ(code, expected_code) << "msg: " << (msg ? msg : "<null>");
    ni::cmodel::cosim::g_dpi_error_code.store(CMODEL_DPI_OK);
    ni::cmodel::cosim::g_dpi_error_msg.clear();
}

class CmodelDpiLifecycleTest : public ::testing::Test {};

TEST_F(CmodelDpiLifecycleTest, walk_session_state_machine) {
    // Case: cmodel_finalize from UNINITIALIZED → no-op (no error, state unchanged).
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: *_create before init → ERR_NOT_INITIALIZED.
    void* h0 = cmodel_channel_model_create("cm_pre_init");
    EXPECT_EQ(h0, nullptr);
    check_and_clear_error(CMODEL_DPI_ERR_NOT_INITIALIZED);

    // Case: cmodel_init on bad YAML → ERR_GENERIC, state stays UNINITIALIZED.
    cmodel_init("/nonexistent/path/to/scenario.yaml");
    check_and_clear_error(CMODEL_DPI_ERR_GENERIC);

    // Case: cmodel_init with good YAML → succeeds.
    const char* good_yaml = std::getenv("CMODEL_TEST_SCENARIO_YAML");
    ASSERT_NE(good_yaml, nullptr) << "set CMODEL_TEST_SCENARIO_YAML to a valid scenario";
    cmodel_init(good_yaml);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cmodel_init called twice (both successful) → second rejected.
    cmodel_init(good_yaml);
    check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);

    // === ChannelModel cases (T5) ===

    // Case: channel_model_create after init succeeds.
    void* cm_handle = cmodel_channel_model_create("cm_test");
    ASSERT_NE(cm_handle, nullptr);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: garbage void* (non-registry) → registry-membership guard, no SIGSEGV.
    void* garbage = reinterpret_cast<void*>(0xDEADBEEFCAFEull);
    cmodel_channel_model_tick(garbage);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // === Master cases (T6) ===

    // Case: master_create after init succeeds; scoreboard callbacks wired.
    void* master_handle = cmodel_master_create("master_test");
    ASSERT_NE(master_handle, nullptr);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: type mismatch — channel_model handle passed to master_tick.
    cmodel_master_tick(cm_handle);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // === Slave case (T7) ===

    // Case: slave_create after init succeeds.
    void* slave_handle = cmodel_slave_create("slave_test");
    ASSERT_NE(slave_handle, nullptr);
    check_and_clear_error(CMODEL_DPI_OK);

    // === NMU multi-instance independence (T8) ===

    // Case: create 2 NMU adapters — distinct void* + both validate as live.
    void* nmu_a = cmodel_nmu_create("nmu_a");
    void* nmu_b = cmodel_nmu_create("nmu_b");
    ASSERT_NE(nmu_a, nullptr);
    ASSERT_NE(nmu_b, nullptr);
    EXPECT_NE(nmu_a, nmu_b);
    check_and_clear_error(CMODEL_DPI_OK);

    // === NSU case (T9 — last per-shell) ===

    // Case: nsu_create after init succeeds.
    void* nsu_handle = cmodel_nsu_create("nsu_test");
    ASSERT_NE(nsu_handle, nullptr);
    check_and_clear_error(CMODEL_DPI_OK);

    // === Lifecycle aggregation + FINALIZED phase (T10) ===

    // Case: cmodel_done with master created but not driven → 0.
    //   ever_created_master ≥ 1 (T6 created master_handle) but master.done()
    //   is false until scenario completes. Exercises BOTH the non-vacuous
    //   guard AND the "any master not done → return 0" path.
    EXPECT_EQ(cmodel_done(), 0);

    // Case: finalize from INITIALIZED → registry destroyed, state = FINALIZED.
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cycle op on stale ctx after finalize → registry-miss → HERMETIC_VIOLATION.
    cmodel_channel_model_tick(cm_handle);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // Case: finalize twice → second is no-op.
    cmodel_finalize();
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: cmodel_init after finalize → REINIT_FORBIDDEN (terminal state).
    cmodel_init(good_yaml);
    check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);
}

}  // namespace
