#include "common/component_dwell_observer.hpp"
#include "common/flit_link_probe.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::testing::FlitLog;
using ni::cmodel::testing::OccupancyPeak;
using ni::cmodel::testing::SegmentDwell;

namespace {
Flit ch_flit(uint8_t axi_ch, uint8_t vc) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("vc_id", vc);
    f.set_header_field("src_id", 0);
    f.set_header_field("dst_id", 0);
    return f;
}
}  // namespace

TEST(SegmentDwell, PairsFifoOrderPerVcAndKeysByChannel) {
    FlitLog entry("c.in");
    FlitLog exit("c.out");
    // Two AW flits (axi_ch=0) on vc 0: entry@10/exit@13 (dwell 3), entry@11/exit@17 (dwell 6).
    entry.record(ch_flit(0, 0), 10);
    entry.record(ch_flit(0, 0), 11);
    exit.record(ch_flit(0, 0), 13);
    exit.record(ch_flit(0, 0), 17);

    SegmentDwell d;
    d.pair(entry, exit);
    const auto& aw = d.by_channel(0);
    EXPECT_EQ(aw.count(), 2u);
    EXPECT_EQ(aw.min(), 3u);
    EXPECT_EQ(aw.max(), 6u);
    EXPECT_DOUBLE_EQ(aw.mean(), 4.5);
    EXPECT_EQ(d.all().count(), 2u);
}

TEST(SegmentDwell, SeparatesVcStreams) {
    FlitLog entry("c.in");
    FlitLog exit("c.out");
    // vc0 entry@0 exit@2 (dwell 2); vc1 entry@0 exit@5 (dwell 5). Interleaved
    // entry order vc0,vc1; exit order vc1,vc0 — FIFO-per-VC must still pair right.
    entry.record(ch_flit(1, 0), 0);  // W on vc0
    entry.record(ch_flit(1, 1), 0);  // W on vc1
    exit.record(ch_flit(1, 1), 5);   // vc1 first out
    exit.record(ch_flit(1, 0), 2);   // vc0 second out

    SegmentDwell d;
    d.pair(entry, exit);
    const auto& w = d.by_channel(1);
    EXPECT_EQ(w.count(), 2u);
    EXPECT_EQ(w.min(), 2u);
    EXPECT_EQ(w.max(), 5u);
}

TEST(OccupancyPeak, TracksMaxFillAgainstCapacity) {
    OccupancyPeak occ(/*capacity=*/4);
    occ.sample(1);
    occ.sample(3);
    occ.sample(2);
    EXPECT_EQ(occ.peak(), 3u);
    EXPECT_EQ(occ.capacity(), 4u);
}
