// c_model/tests/wrap/test_cmodel_dpi.cpp
// Single ordered TEST_F walking the DPI session state machine
// (Uninitialized → Initialized → Finalized) + per-instance handle guards.
// Each negative assertion calls check_and_clear_error() to drain the latch.
//
// Also covers the DPI marshal round-trip (dpi_marshal.hpp): beat-exact
// verification at the wire boundary (S2 T2c) — per-lane-distinct data bytes
// and walking-1 WSTRB, at the widths current today (633-bit flit, 512-bit
// data bus). A word swap, lane slip, tail-bit leak, or strb truncation
// changes a compared byte.
#include "cmodel_dpi.h"
#include "dpi_marshal.hpp"
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
    const char* topology = CONFIG_DIR "/mesh_2x2.yml";
    unsigned long long nmu_a = cmodel_nmu_create("nmu_a", 0, /*num_vc=*/1, topology);
    unsigned long long nmu_b = cmodel_nmu_create("nmu_b", 0, /*num_vc=*/1, topology);
    ASSERT_NE(nmu_a, 0ull);
    ASSERT_NE(nmu_b, 0ull);
    EXPECT_NE(nmu_a, nmu_b);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: create with no topology → no handle, and the reason reaches the
    // latch the SV side polls. The wrap used to invent a 16x16 / 4 GB SAM
    // here, so a testbench that dropped its config_path ran to completion
    // against a map nothing in the tree ships.
    EXPECT_EQ(cmodel_nmu_create("nmu_no_sam", 0, /*num_vc=*/1, /*config_path=*/nullptr), 0ull);
    check_and_clear_error(CMODEL_DPI_ERR_GENERIC);

    // Case: type-guard — an NMU handle passed to cmodel_nsu_tick (WrapType
    // mismatch) → HERMETIC_VIOLATION; the type tag rejects the wrong wrap.
    cmodel_nsu_tick(nmu_a);
    check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

    // === NSU case ===

    // Case: nsu_create after init succeeds.
    unsigned long long nsu_handle = cmodel_nsu_create("nsu_test", 0, /*num_vc=*/1,
                                                      /*max_unique_ids=*/1,
                                                      /*max_outstanding=*/32, /*port_id=*/0,
                                                      /*config_path=*/"");
    ASSERT_NE(nsu_handle, 0ull);
    check_and_clear_error(CMODEL_DPI_OK);

    // Case: port_id 3 → no handle. The field is 2 b, so 3 fits on the wire but
    // names no endpoint; the wrap's guard must reach the latch the SV side
    // polls rather than a silently created fourth port.
    EXPECT_EQ(cmodel_nsu_create("nsu_bad_port", 0, /*num_vc=*/1, /*max_unique_ids=*/1,
                                /*max_outstanding=*/32, /*port_id=*/3, /*config_path=*/""),
              0ull);
    check_and_clear_error(CMODEL_DPI_ERR_GENERIC);

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

// === DPI marshal round-trip (dpi_marshal.hpp) ===
//
// Direct unit coverage of the pack/unpack helpers, independent of any NMU/NSU
// pipeline: a word swap, lane slip, tail-bit leak, or strb truncation changes
// a compared byte here exactly as it would through a full co-sim readback.

using namespace ni::cmodel::wrap;

TEST(DpiMarshalTest, PackUnpackFlit_RoundTrip_PerByteDistinctPattern) {
    // Every flit byte gets a distinct value; the last byte's padding bits
    // (beyond FLIT_WIDTH) are left at 0, as a real Flit::raw() produces.
    constexpr int kLastByteValidBits = ni::FLIT_WIDTH - (FLIT_BYTES - 1) * 8;  // 633-632=1
    FlitBytes b{};
    for (int i = 0; i < FLIT_BYTES; ++i) b[i] = static_cast<uint8_t>(i * 7 + 3);
    b[FLIT_BYTES - 1] &= static_cast<uint8_t>((1u << kLastByteValidBits) - 1u);

    svBitVecVal vec[FLIT_VEC_WORDS];
    pack_flit(b, vec);
    FlitBytes rt = unpack_flit(vec);
    EXPECT_EQ(rt, b);
}

TEST(DpiMarshalTest, PackFlit_TailWordExplicitlyMasked_RegardlessOfInputPadding) {
    // Every bit up to FLIT_WIDTH set, padding above it left 0 (what a real
    // Flit::raw() produces). pack_flit must drive every real bit position of
    // the tail word and no padding bit.
    FlitBytes b{};
    b.fill(0xFF);
    constexpr int kLastByteValidBits = ni::FLIT_WIDTH - (FLIT_BYTES - 1) * 8;
    b[FLIT_BYTES - 1] = static_cast<uint8_t>((1u << kLastByteValidBits) - 1u);

    svBitVecVal vec[FLIT_VEC_WORDS];
    pack_flit(b, vec);
    const uint32_t tail = vec[FLIT_VEC_WORDS - 1];
    EXPECT_EQ(tail & ~FLIT_TAIL_MASK, 0u)
        << "tail word carries bits past FLIT_WIDTH; explicit mask did not fire";
    EXPECT_EQ(tail & FLIT_TAIL_MASK, FLIT_TAIL_MASK) << "tail mask over-masked real flit bits";
}

TEST(DpiMarshalTest, Fits_RejectsFlitWiderThanTheNetworkWire) {
    // The wire silently drops every bit at or above its network's width, so a
    // flit carrying live payload up there is lost with no symptom until the AXI
    // data comes back wrong. fits() is what makes that loud (pack asserts on it).
    FlitBytes narrow_ok{};
    for (int bit = 0; bit < ni::NOC_RSP_FLIT_WIDTH; ++bit)
        narrow_ok[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    EXPECT_TRUE(RspFlitMarshal::fits(narrow_ok));
    EXPECT_TRUE(ReqFlitMarshal::fits(narrow_ok)) << "RSP width < REQ width, must still fit";

    // One bit just above the REQ wire width — the shape of a data-class payload
    // riding a narrow network.
    FlitBytes one_over = narrow_ok;
    one_over[ni::NOC_REQ_FLIT_WIDTH / 8] |=
        static_cast<uint8_t>(1u << (ni::NOC_REQ_FLIT_WIDTH % 8));
    EXPECT_FALSE(ReqFlitMarshal::fits(one_over));
    EXPECT_TRUE(DatFlitMarshal::fits(one_over));
}

TEST(DpiMarshalTest, PackUnpackAxiData_RoundTrip_PerLaneDistinctBytes) {
    std::array<uint8_t, ni::cmodel::axi::DATA_BYTES> data{};
    for (int i = 0; i < ni::cmodel::axi::DATA_BYTES; ++i) data[i] = static_cast<uint8_t>(i);

    svBitVecVal vec[DATA_VEC_WORDS];
    pack_axi_data(data, vec);
    EXPECT_EQ(unpack_axi_data(vec), data);
}

TEST(DpiMarshalTest, PackUnpackWstrb_RoundTrip_WalkingOne) {
    for (int lane = 0; lane < ni::cmodel::axi::DATA_BYTES; ++lane) {
        const uint64_t strb = uint64_t{1} << lane;
        svBitVecVal vec[WSTRB_VEC_WORDS];
        pack_wstrb(strb, vec);
        EXPECT_EQ(unpack_wstrb(vec), strb) << "lane " << lane;
    }
}

TEST(DpiMarshalTest, PackUnpackWstrb_RoundTrip_FullStrobe) {
    const uint64_t full = ni::cmodel::axi::kFullStrbMask;
    svBitVecVal vec[WSTRB_VEC_WORDS];
    pack_wstrb(full, vec);
    EXPECT_EQ(unpack_wstrb(vec), full);
}

}  // namespace
