#include "register_file.hpp"
#include <gtest/gtest.h>

using ni::cmodel::AbiResult;
using ni::cmodel::RegisterFile;

TEST(RegisterFile, ReadUnmappedReturnsDecErr) {
    RegisterFile rf;
    auto r = rf.read32(0xFFFC);  // unmapped offset
    EXPECT_EQ(r.status, AbiResult::DecErr);
    EXPECT_EQ(r.data, 0u);
}

TEST(RegisterFile, WriteMisalignedReturnsSlvErr) {
    RegisterFile rf;
    auto r = rf.write32(ni::regs::PKT_PROBE_EN_OFFSET + 1, 0xDEADBEEF);
    EXPECT_EQ(r.status, AbiResult::SlvErr);  // per csr_policy: misaligned=slverr
}

TEST(RegisterFile, SubWordWriteReturnsSlvErr) {
    RegisterFile rf;
    auto r = rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0xDEADBEEF, /*wstrb=*/0b0001);
    EXPECT_EQ(r.status, AbiResult::SlvErr);  // per csr_policy: sub_word_write=slverr
}

TEST(RegisterFile, WriteFollowedByRead) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0x12345678);
    auto r = rf.read32(ni::regs::PKT_PROBE_EN_OFFSET);
    EXPECT_EQ(r.status, AbiResult::Ok);
    EXPECT_EQ(r.data, 0x12345678u);
}

TEST(RegisterFile, ReadFieldMasks) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0x000000F0);
    uint32_t v = rf.read_field(ni::regs::PKT_PROBE_EN_OFFSET, 0x000000F0);
    EXPECT_EQ(v, 0x0Fu);
}

TEST(RegisterFile, WriteFieldDoesNotTouchOtherBits) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0xFFFFFFFF);
    rf.write_field(ni::regs::PKT_PROBE_EN_OFFSET, 0x000000F0, 0x0);
    auto r = rf.read32(ni::regs::PKT_PROBE_EN_OFFSET);
    EXPECT_EQ(r.data, 0xFFFFFF0Fu);
}

TEST(RegisterFile, LastWriteIrqInitiallyFalse) {
    RegisterFile rf;
    EXPECT_FALSE(rf.last_write_triggered_irq());
}

TEST(RegisterFile, LastWriteRw1cInitiallyFalse) {
    RegisterFile rf;
    EXPECT_FALSE(rf.last_write_cleared_rw1c_field());
}

TEST(RegisterFile, WriteFieldThenReadField) {
    RegisterFile rf;
    rf.write_field(ni::regs::PKT_PROBE_EN_OFFSET, 0x000000FF, 0xA5);
    uint32_t v = rf.read_field(ni::regs::PKT_PROBE_EN_OFFSET, 0x000000FF);
    EXPECT_EQ(v, 0xA5u);
}

TEST(RegisterFile, MultipleRegistersAreIndependent) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0xAAAA);
    rf.write32(ni::regs::PKT_PROBE_MODE_OFFSET, 0xBBBB);
    EXPECT_EQ(rf.read32(ni::regs::PKT_PROBE_EN_OFFSET).data, 0xAAAAu);
    EXPECT_EQ(rf.read32(ni::regs::PKT_PROBE_MODE_OFFSET).data, 0xBBBBu);
}

TEST(RegisterFile, ResetClearsAllStorage) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0xDEAD);
    rf.reset();
    EXPECT_EQ(rf.read32(ni::regs::PKT_PROBE_EN_OFFSET).data, 0u);
}

TEST(RegisterFile, TxnMinLatencyResetIsNonZeroFromCodegen) {
    RegisterFile rf;
    auto r = rf.read32(ni::regs::TXN_MIN_LATENCY_OFFSET);
    EXPECT_EQ(r.status, AbiResult::Ok);
    EXPECT_EQ(r.data, ni::regs::TXN_MIN_LATENCY_RESET);  // codegen says 0xFFFF
    EXPECT_EQ(r.data, 0xFFFFu);
}

TEST(RegisterFile, KnownOffsetsMatchCodegenAllOffsets) {
    RegisterFile rf;
    for (std::size_t i = 0; i < ni::regs::ALL_OFFSETS_COUNT; ++i) {
        auto r = rf.read32(ni::regs::ALL_OFFSETS[i]);
        EXPECT_EQ(r.status, AbiResult::Ok)
            << "offset " << std::hex << ni::regs::ALL_OFFSETS[i] << " not mapped";
    }
    auto r = rf.read32(0xFFFC);
    EXPECT_EQ(r.status, AbiResult::DecErr);
}

TEST(RegisterFile, WriteOneToRW1CClearsBit) {
    RegisterFile rf;
    rf.write_field(ni::regs::ERR_STATUS_OFFSET, 0xFFFFFFFF, 0x7);  // bits 0, 1, 2 set
    auto r = rf.write32(ni::regs::ERR_STATUS_OFFSET, 0x3);
    EXPECT_EQ(r.status, AbiResult::Ok);
    auto read = rf.read32(ni::regs::ERR_STATUS_OFFSET);
    EXPECT_EQ(read.data & 0x7u, 0x4u);
    EXPECT_TRUE(rf.last_write_cleared_rw1c_field());
}

TEST(RegisterFile, WriteToROIsSilentlyIgnored) {
    RegisterFile rf;
    uint32_t before = rf.read32(ni::regs::PKT_BYTE_COUNT_OFFSET).data;
    auto r = rf.write32(ni::regs::PKT_BYTE_COUNT_OFFSET, 0xDEADBEEF);
    EXPECT_EQ(r.status, AbiResult::Ok);
    uint32_t after = rf.read32(ni::regs::PKT_BYTE_COUNT_OFFSET).data;
    EXPECT_EQ(after, before);
}

TEST(RegisterFile, ReadFromWOReturnsZero) {
    RegisterFile rf;
    auto w = rf.write32(ni::regs::EXCLUSIVE_MONITOR_CTRL_OFFSET, 0x12345678);
    EXPECT_EQ(w.status, AbiResult::Ok);
    auto r = rf.read32(ni::regs::EXCLUSIVE_MONITOR_CTRL_OFFSET);
    EXPECT_EQ(r.status, AbiResult::Ok);
    EXPECT_EQ(r.data, 0u);
}

TEST(RegisterFile, LastWriteFlagResetOnEachWrite) {
    RegisterFile rf;
    rf.write32(ni::regs::PKT_PROBE_EN_OFFSET, 0x1);
    EXPECT_FALSE(rf.last_write_cleared_rw1c_field());
    rf.write_field(ni::regs::ERR_STATUS_OFFSET, 0xFFFFFFFF, 0x1);
    rf.write32(ni::regs::ERR_STATUS_OFFSET, 0x1);
    EXPECT_TRUE(rf.last_write_cleared_rw1c_field());
}
