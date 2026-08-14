#include "axi/scoreboard.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

// Under lane-positioned bus semantics, WriteResult/ReadResult carry
// size/len/burst so the scoreboard can re-derive per-beat byte_lane. Tests
// pass size=5, len=0, burst=INCR (1-beat, full-bus) unless the case requires
// otherwise — that combination matches the original full-width aligned geometry.
TEST(Scoreboard, NoUpdateOnDecerr) {
    axi::Scoreboard sb;
    std::vector<uint64_t> strb1(1, 0xFFFF'FFFFu);
    sb.handle_write_completed(axi::WriteResult{0x100,
                                               /*size*/ 5,
                                               /*len*/ 0,
                                               axi::Burst::INCR,
                                               axi::LockType::Normal,
                                               {0xAB, 0xCD, 0xEF, 0x12},
                                               strb1,
                                               axi::Resp::DECERR,
                                               1,
                                               1},
                              std::vector<uint8_t>{0xAB, 0xCD, 0xEF, 0x12}, strb1);
    sb.handle_read_observed(axi::ReadResult{
        0x100, /*size*/ 5, /*len*/ 0, axi::Burst::INCR, {0x00, 0x00}, axi::Resp::OKAY, 1, 2});
    EXPECT_EQ(sb.mismatch_count(), 0u);
}

TEST(Scoreboard, MismatchDetected) {
    axi::Scoreboard sb;
    std::vector<uint64_t> strb1(1, 0xFFFF'FFFFu);
    std::vector<uint8_t> wdata(axi::DATA_BYTES, 0x00u);
    wdata[0] = 0xAB;
    wdata[1] = 0xCD;
    wdata[2] = 0xEF;
    wdata[3] = 0x12;
    sb.handle_write_completed(
        axi::WriteResult{0x200, /*size*/ 5, /*len*/ 0, axi::Burst::INCR, axi::LockType::Normal,
                         wdata, strb1, axi::Resp::OKAY, 1, 1},
        wdata, strb1);
    sb.handle_read_observed(axi::ReadResult{0x200,
                                            /*size*/ 5,
                                            /*len*/ 0,
                                            axi::Burst::INCR,
                                            {0xAB, 0xCD, 0xEE, 0x12},
                                            axi::Resp::OKAY,
                                            1,
                                            2});
    EXPECT_EQ(sb.mismatch_count(), 1u);
    EXPECT_FALSE(sb.mismatch_report().empty());
}

TEST(Scoreboard, MatchPassesSilent) {
    axi::Scoreboard sb;
    std::vector<uint64_t> strb1(1, 0xFFFF'FFFFu);
    std::vector<uint8_t> wdata(axi::DATA_BYTES, 0x00u);
    wdata[0] = 0xDE;
    wdata[1] = 0xAD;
    wdata[2] = 0xBE;
    wdata[3] = 0xEF;
    sb.handle_write_completed(
        axi::WriteResult{0x300, /*size*/ 5, /*len*/ 0, axi::Burst::INCR, axi::LockType::Normal,
                         wdata, strb1, axi::Resp::OKAY, 1, 1},
        wdata, strb1);
    sb.handle_read_observed(axi::ReadResult{0x300,
                                            /*size*/ 5,
                                            /*len*/ 0,
                                            axi::Burst::INCR,
                                            {0xDE, 0xAD, 0xBE, 0xEF},
                                            axi::Resp::OKAY,
                                            1,
                                            2});
    EXPECT_EQ(sb.mismatch_count(), 0u);
    EXPECT_EQ(sb.reads_checked(), 1u);
}

TEST(Scoreboard, ReadFromUnwrittenAddrReturnsFillDefault) {
    axi::Scoreboard sb;
    sb.handle_read_observed(axi::ReadResult{0x400,
                                            /*size*/ 5,
                                            /*len*/ 0,
                                            axi::Burst::INCR,
                                            {0x00, 0x00, 0x00, 0x00},
                                            axi::Resp::OKAY,
                                            1,
                                            1});
    EXPECT_EQ(sb.mismatch_count(), 0u);
}

// IHI 0022 A3.4.1: unaligned-write masking must mirror axi_master.hpp's first-beat WSTRB mask --
// this is the test that would have caught the scoreboard/master divergence when the two were fixed
// separately.
TEST(Scoreboard, UnalignedWriteBeat0PrefixNotCommitted) {
    axi::Scoreboard sb;
    std::vector<uint8_t> data(axi::DATA_BYTES, 0x00u);
    for (int i = 0; i < axi::DATA_BYTES; ++i) data[i] = static_cast<uint8_t>(i + 1);  // never 0x00
    // Raw, un-masked strb_per_beat -- exactly what WriteResult::strb_per_beat carries
    // (op.strb_per_beat is beat-relative and is NOT re-masked before reaching the
    // scoreboard; the wire-level prefix mask lives only in axi_master.hpp's W-loop).
    std::vector<uint64_t> strb{axi::kFullStrbMask};
    axi::WriteResult wr{
        0x1003,          /*size*/ 5, /*len*/ 0, axi::Burst::INCR, axi::LockType::Normal, data, strb,
        axi::Resp::OKAY, 1,          1};
    sb.handle_write_completed(wr, data, strb);

    // Aligned-window readback [0x1000, 0x1020): the slave never wrote the first 3
    // bytes (prefix, masked off on the wire), so they read as fill-default 0x00;
    // the true write range [0x1003, 0x1020) reads back data[3..31].
    std::vector<uint8_t> observed(32, 0x00u);
    for (int j = 3; j < 32; ++j)
        observed[static_cast<std::size_t>(j)] = data[static_cast<std::size_t>(j)];
    axi::ReadResult rr{0x1000,   /*size*/ 5,      /*len*/ 0, axi::Burst::INCR,
                       observed, axi::Resp::OKAY, 1,         2};
    sb.handle_read_observed(rr);
    EXPECT_EQ(sb.mismatch_count(), 0u);
}

TEST(Scoreboard, SparseWstrbByteMerge) {
    // 1-beat write with strb=0x0F: only byte lanes 0-3 land in expected_;
    // the remaining bytes stay at the default-fill (0x00). A subsequent read
    // observing 0xAA in lanes 0-3 and 0x00 elsewhere must produce zero mismatches.
    // Under lane-positioned bus, addr 0x100 has byte_lane=0, so strb bits 0..3
    // map to memory addrs 0x100..0x103.
    axi::Scoreboard sb;
    std::vector<uint8_t> data(axi::DATA_BYTES, 0xAAu);
    std::vector<uint64_t> strb{0x0000000Fu};
    axi::WriteResult wr{
        0x100,           /*size*/ 5, /*len*/ 0, axi::Burst::INCR, axi::LockType::Normal, data, strb,
        axi::Resp::OKAY, 1,          1};
    sb.handle_write_completed(wr, data, strb);

    std::vector<uint8_t> read_data(axi::DATA_BYTES, 0x00u);
    for (int i = 0; i < 4; ++i) read_data[i] = 0xAAu;
    axi::ReadResult rr{0x100,     /*size*/ 5,      /*len*/ 0, axi::Burst::INCR,
                       read_data, axi::Resp::OKAY, 1,         2};
    sb.handle_read_observed(rr);
    EXPECT_EQ(sb.mismatch_count(), 0u);
}
