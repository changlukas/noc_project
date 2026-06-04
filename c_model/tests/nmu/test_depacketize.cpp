#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::Depacketize;
using ni::cmodel::testing::LoopbackNoc;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_b_flit(uint8_t bid, axi::Resp resp = axi::Resp::OKAY) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", 0x10);
    f.set_header_field("last", 1);
    f.set_payload_field("B", "bid", bid);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(resp));
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", 0x10);
    f.set_header_field("last", 1);
    f.set_payload_field("R", "rid", rid);
    f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
    return f;
}
}  // namespace

TEST(NmuDepacketize, PopBDecodesFromFlit) {
    SCENARIO("NMU Depacketize: B flit decodes to BBeat with id and resp from payload fields");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), /*b*/ 16, /*r*/ 16);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x05, axi::Resp::SLVERR)));
    depkt.tick();
    auto b = depkt.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05);
    EXPECT_EQ(b->resp, axi::Resp::SLVERR);
}

TEST(NmuDepacketize, DemuxMixedFlitsByAxiCh) {
    SCENARIO("NMU Depacketize: B/R interleaved flits demux to per-channel queues by axi_ch header");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x02, true)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x03)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_b()->id, 0x01);
    EXPECT_EQ(depkt.pop_r()->id, 0x02);
    EXPECT_EQ(depkt.pop_b()->id, 0x03);
}

TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR) {
    SCENARIO("NMU Depacketize: HoL B-queue full holds pending B; R behind blocked until B drained");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), /*b cap=*/1, /*r cap=*/16);
    // Queue order: B, B, R
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x02)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x03, true)));
    depkt.tick();
    // First B fits; second B holds pending; R behind cannot progress
    EXPECT_TRUE(depkt.pop_b().has_value());   // 0x01
    EXPECT_FALSE(depkt.pop_r().has_value());  // R blocked behind pending B
    depkt.tick();                             // pending B (0x02) now placed
    EXPECT_TRUE(depkt.pop_b().has_value());   // 0x02
    depkt.tick();                             // R (0x03) now placed
    EXPECT_TRUE(depkt.pop_r().has_value());
}

TEST(NmuDepacketize, PopBEmptyReturnsNullopt) {
    SCENARIO("NMU Depacketize: pop_b/pop_r on empty queues return nullopt (no spurious values)");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_FALSE(depkt.pop_b().has_value());
    EXPECT_FALSE(depkt.pop_r().has_value());
}

TEST(NmuDepacketize, PopAwAssertFalse) {
    SCENARIO("NMU Depacketize: pop_aw asserts false (AW is request-direction only, not response)");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH(depkt.pop_aw(), ".*");
}

TEST(NmuDepacketize, BFifoOrderPreserved) {
    SCENARIO("NMU Depacketize: B queue preserves NoC arrival order across 5 sequential B flits");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    for (uint8_t i = 0; i < 5; ++i) ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(i)));
    depkt.tick();
    for (uint8_t i = 0; i < 5; ++i) EXPECT_EQ(depkt.pop_b()->id, i);
}

TEST(NmuDepacketize, RPayloadBytesDecoded) {
    SCENARIO("NMU Depacketize: R flit payload bytes (rdata, 32B) decode bit-perfect to RBeat.data");
    LoopbackNoc noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", 0x10);
    f.set_payload_field("R", "rid", 0x07);
    f.set_payload_field("R", "rlast", 1);
    std::array<uint8_t, 32> data;
    for (int i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(0xE0 + i);
    f.set_payload_bytes("R", "rdata", data.data(), 256);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    auto r = depkt.pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 0x07);
    EXPECT_EQ(r->last, true);
    EXPECT_EQ(r->data, data);
}

TEST(NmuDepacketize, PopBWithMeta_ExtractsRobIdxAndRobReq) {
    SCENARIO(
        "NMU Depacketize: pop_b_with_meta returns rob_req/rob_idx from header for ROB routing");
    using namespace ni::cmodel;
    LoopbackNoc loopback(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(loopback.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("src_id", 0x10);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 5);
    f.set_payload_field("B", "bid", 0x42);
    f.set_payload_field("B", "bresp", 0);
    f.set_payload_field("B", "buser", 0);

    ASSERT_TRUE(loopback.rsp_out().push_flit(f));
    depkt.tick();

    auto opt = depkt.pop_b_with_meta();
    ASSERT_TRUE(opt.has_value());
    auto [b, meta] = *opt;
    EXPECT_EQ(b.id, 0x42u);
    EXPECT_EQ(meta.rob_idx, 5u);
    EXPECT_EQ(meta.rob_req, 1u);
}

TEST(NmuDepacketize, PopRWithMeta_ExtractsPerBeatRobIdx) {
    SCENARIO(
        "NMU Depacketize: pop_r_with_meta returns per-beat rob_idx (5,6,7,8) for 4-beat burst");
    using namespace ni::cmodel;
    LoopbackNoc loopback(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(loopback.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    for (uint8_t i = 0; i < 4; ++i) {
        Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("src_id", 0x10);
        f.set_header_field("dst_id", 0x01);
        f.set_header_field("vc_id", 0);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", 5 + i);
        f.set_payload_field("R", "rid", 0x42);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "ruser", 0);
        f.set_payload_field("R", "rlast", (i == 3) ? 1u : 0u);
        std::array<uint8_t, 32> data{};
        data[0] = static_cast<uint8_t>(0xA0 + i);
        f.set_payload_bytes("R", "rdata", data.data(), 256);
        ASSERT_TRUE(loopback.rsp_out().push_flit(f));
    }
    depkt.tick();

    for (uint8_t i = 0; i < 4; ++i) {
        auto opt = depkt.pop_r_with_meta();
        ASSERT_TRUE(opt.has_value()) << "beat " << static_cast<int>(i);
        auto [r, meta] = *opt;
        EXPECT_EQ(meta.rob_idx, 5u + i);
        EXPECT_EQ(meta.rob_req, 1u);
        EXPECT_EQ(r.last, i == 3);
        EXPECT_EQ(r.data[0], 0xA0u + i);
    }
}
