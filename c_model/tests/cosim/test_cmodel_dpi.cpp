// Single ordered TEST_F walking the DPI session state machine.
// Each negative assertion calls check_and_clear_error() to drain the latch.
// Body extended by Tasks 3-10; EXCLUDE_FROM_ALL removed by Task 5.
#include "cmodel_dpi.h"
#include "handle_block.hpp"
#include <atomic>
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
    // Case: *_create before init → ERR_NOT_INITIALIZED.
    void* h0 = cmodel_channel_model_create("cm_pre_init");
    EXPECT_EQ(h0, nullptr);
    check_and_clear_error(CMODEL_DPI_ERR_NOT_INITIALIZED);

    // Body extended by Tasks 3-10.
}

}  // namespace
