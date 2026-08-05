// Two-node NI/router chain at the wrap layer — the shape noc_fabric_*.sv
// wires: NmuWrap + NsuWrap + DatMergeWrap + RouterWrap per node, ni<->router
// LOCAL crossing, EAST/WEST inter-router link, one AXI write + readback from
// node 0 to node 1.
//
// Fills the gap that let S3a T5 ship broken: every other wrap test drives one
// wrap against mocks, so nothing exercised the four of them wired together and
// co-sim was the only end-to-end check. Keep the wiring here mirroring
// gen_tb_top.py's emit_fabric() connection list.
#include "wrap/dat_merge_wrap.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nsu_wrap.hpp"
#include "wrap/router_wrap.hpp"

#include <gtest/gtest.h>

using namespace ni::cmodel::wrap;

namespace {

constexpr std::size_t LOCAL = 0, EAST = 2, WEST = 4;

struct Node {
    NmuWrap nmu;
    NsuWrap nsu;
    DatMergeWrap merge;
    RouterWrap router;

    NmuInputs nmu_in{};
    NmuOutputs nmu_out{};
    NsuInputs nsu_in{};
    NsuOutputs nsu_out{};
    DatMergeInputs merge_in{};
    DatMergeOutputs merge_out{};
    RouterInputs rtr_in{};
    RouterOutputs rtr_out{};
};

// Minimal AXI slave behind each NSU: accepts AW/W, stores one 64-byte line per
// address, returns B; accepts AR, returns R. Zero wait states.
struct MemSlave {
    std::map<uint64_t, std::array<uint8_t, 64>> mem;
    std::deque<std::pair<uint8_t, uint8_t>> b_q;  // (id, resp)
    std::deque<std::tuple<uint8_t, std::array<uint8_t, 64>, bool>> r_q;
    uint64_t aw_addr = 0;
    uint8_t aw_id = 0;
    bool aw_open = false;

    // Combinational-ish: called after NSU outputs are known for this cycle.
    void step(const NsuOutputs& o, NsuInputs& i) {
        i.awready = true;
        i.wready = true;
        i.arready = true;
        if (o.awvalid) {
            aw_addr = o.awaddr;
            aw_id = o.awid;
            aw_open = true;
        }
        if (o.wvalid && aw_open) {
            mem[aw_addr] = o.wdata;
            if (o.wlast) {
                b_q.emplace_back(aw_id, 0);
                aw_open = false;
            }
        }
        if (o.arvalid) {
            auto it = mem.find(o.araddr);
            std::array<uint8_t, 64> d{};
            if (it != mem.end()) d = it->second;
            r_q.emplace_back(o.arid, d, true);
        }
        i.bvalid = false;
        i.rvalid = false;
        if (!b_q.empty()) {
            i.bvalid = true;
            i.bid = b_q.front().first;
            i.bresp = b_q.front().second;
            if (o.bready) b_q.pop_front();
        }
        if (!r_q.empty()) {
            i.rvalid = true;
            i.rid = std::get<0>(r_q.front());
            i.rdata = std::get<1>(r_q.front());
            i.rlast = std::get<2>(r_q.front());
            i.rresp = 0;
            if (o.rready) r_q.pop_front();
        }
    }
};

}  // namespace

static void run_chain(bool* ok_data) {
    Node n[2];
    MemSlave slave[2];

    for (int k = 0; k < 2; ++k) {
        n[k].nmu.init(/*src_id=*/static_cast<uint8_t>(k), /*dat_num_vc=*/1);
        n[k].nsu.init(/*src_id=*/static_cast<uint8_t>(k), /*dat_num_vc=*/1);
        n[k].merge.init(1);
        n[k].router.init(/*x=*/static_cast<uint8_t>(k), /*y=*/0, /*mesh_x=*/2, /*mesh_y=*/2,
                         /*dat_num_vc=*/1);
    }

    // Registered wire state = previous cycle's outputs (all wraps register).
    NmuOutputs nmu_q[2]{};
    NsuOutputs nsu_q[2]{};
    DatMergeOutputs merge_q[2]{};
    RouterOutputs rtr_q[2]{};

    // AXI master stimulus at node 0: write 1 beat to node 1, then read it back.
    const uint64_t addr = 0x1'0000'0000ull;  // dst tile 1 (SAM uniform 4 GB/tile)
    std::array<uint8_t, 64> wdata{};
    for (int b = 0; b < 64; ++b) wdata[b] = static_cast<uint8_t>(0xA0 + b);

    bool aw_sent = false, w_sent = false, b_seen = false;
    bool ar_sent = false, r_seen = false;
    std::array<uint8_t, 64> rdata_seen{};

    int first_req_flit_cycle = -1;
    int first_local_ready_cycle = -1;

    for (int cyc = 0; cyc < 400; ++cyc) {
        // ---------------- comb wiring from registered outputs ----------------
        for (int k = 0; k < 2; ++k) {
            // NI LOCAL crossing (gen_tb_top.py: ni.tx_* -> router rx_*[LOCAL]).
            n[k].rtr_in.rx_req_valid[LOCAL] = nmu_q[k].tx_req_valid;
            n[k].rtr_in.rx_req_flit[LOCAL] = nmu_q[k].tx_req_flit;
            n[k].rtr_in.tx_req_ready[LOCAL] = nsu_q[k].rx_req_ready;
            n[k].nmu_in.tx_req_ready = rtr_q[k].rx_req_ready[LOCAL];
            n[k].nsu_in.rx_req_valid = rtr_q[k].tx_req_valid[LOCAL];
            n[k].nsu_in.rx_req_flit = rtr_q[k].tx_req_flit[LOCAL];

            n[k].rtr_in.rx_rsp_valid[LOCAL] = nsu_q[k].tx_rsp_valid;
            n[k].rtr_in.rx_rsp_flit[LOCAL] = nsu_q[k].tx_rsp_flit;
            n[k].rtr_in.tx_rsp_ready[LOCAL] = nmu_q[k].rx_rsp_ready;
            n[k].nsu_in.tx_rsp_ready = rtr_q[k].rx_rsp_ready[LOCAL];
            n[k].nmu_in.rx_rsp_valid = rtr_q[k].tx_rsp_valid[LOCAL];
            n[k].nmu_in.rx_rsp_flit = rtr_q[k].tx_rsp_flit[LOCAL];

            // DAT via merge.
            n[k].rtr_in.rx_dat_valid[LOCAL] = merge_q[k].tx_dat_valid;
            n[k].rtr_in.rx_dat_flit[LOCAL] = merge_q[k].tx_dat_flit;
            n[k].rtr_in.tx_dat_crdvalid[LOCAL] = merge_q[k].rx_dat_crdvalid;
            n[k].merge_in.rx_dat_valid = rtr_q[k].tx_dat_valid[LOCAL];
            n[k].merge_in.rx_dat_flit = rtr_q[k].tx_dat_flit[LOCAL];
            n[k].merge_in.tx_dat_crdvalid = rtr_q[k].rx_dat_crdvalid[LOCAL];
            n[k].merge_in.nmu_tx_dat_valid = nmu_q[k].tx_dat_valid;
            n[k].merge_in.nmu_tx_dat_flit = nmu_q[k].tx_dat_flit;
            n[k].merge_in.nsu_tx_dat_valid = nsu_q[k].tx_dat_valid;
            n[k].merge_in.nsu_tx_dat_flit = nsu_q[k].tx_dat_flit;
            n[k].nmu_in.rx_dat_valid = merge_q[k].nmu_rx_dat_valid;
            n[k].nmu_in.rx_dat_flit = merge_q[k].nmu_rx_dat_flit;
            n[k].nmu_in.tx_dat_crdvalid = {};
            n[k].nmu_in.tx_dat_crdvalid[0] = merge_q[k].nmu_tx_dat_crdvalid[0];
            n[k].nsu_in.rx_dat_valid = merge_q[k].nsu_rx_dat_valid;
            n[k].nsu_in.rx_dat_flit = merge_q[k].nsu_rx_dat_flit;
            n[k].nsu_in.tx_dat_crdvalid = {};
            n[k].nsu_in.tx_dat_crdvalid[0] = merge_q[k].nsu_tx_dat_crdvalid[0];
        }
        // Inter-node link: node0 EAST <-> node1 WEST.
        n[0].rtr_in.rx_req_valid[EAST] = rtr_q[1].tx_req_valid[WEST];
        n[0].rtr_in.rx_req_flit[EAST] = rtr_q[1].tx_req_flit[WEST];
        n[0].rtr_in.tx_req_ready[EAST] = rtr_q[1].rx_req_ready[WEST];
        n[1].rtr_in.rx_req_valid[WEST] = rtr_q[0].tx_req_valid[EAST];
        n[1].rtr_in.rx_req_flit[WEST] = rtr_q[0].tx_req_flit[EAST];
        n[1].rtr_in.tx_req_ready[WEST] = rtr_q[0].rx_req_ready[EAST];

        n[0].rtr_in.rx_rsp_valid[EAST] = rtr_q[1].tx_rsp_valid[WEST];
        n[0].rtr_in.rx_rsp_flit[EAST] = rtr_q[1].tx_rsp_flit[WEST];
        n[0].rtr_in.tx_rsp_ready[EAST] = rtr_q[1].rx_rsp_ready[WEST];
        n[1].rtr_in.rx_rsp_valid[WEST] = rtr_q[0].tx_rsp_valid[EAST];
        n[1].rtr_in.rx_rsp_flit[WEST] = rtr_q[0].tx_rsp_flit[EAST];
        n[1].rtr_in.tx_rsp_ready[WEST] = rtr_q[0].rx_rsp_ready[EAST];

        n[0].rtr_in.rx_dat_valid[EAST] = rtr_q[1].tx_dat_valid[WEST];
        n[0].rtr_in.rx_dat_flit[EAST] = rtr_q[1].tx_dat_flit[WEST];
        n[0].rtr_in.tx_dat_crdvalid[EAST] = rtr_q[1].rx_dat_crdvalid[WEST];
        n[1].rtr_in.rx_dat_valid[WEST] = rtr_q[0].tx_dat_valid[EAST];
        n[1].rtr_in.rx_dat_flit[WEST] = rtr_q[0].tx_dat_flit[EAST];
        n[1].rtr_in.tx_dat_crdvalid[WEST] = rtr_q[0].rx_dat_crdvalid[EAST];

        // ---------------- AXI master stimulus (node 0) ----------------
        n[0].nmu_in.awvalid = aw_sent ? false : true;
        n[0].nmu_in.awid = 3;
        n[0].nmu_in.awaddr = addr;
        n[0].nmu_in.awlen = 0;
        n[0].nmu_in.awsize = 5;  // 32 B/beat
        n[0].nmu_in.awburst = 1;
        n[0].nmu_in.wvalid = (!w_sent && aw_sent);
        n[0].nmu_in.wdata = wdata;
        n[0].nmu_in.wstrb = ~0ull;
        n[0].nmu_in.wlast = true;
        n[0].nmu_in.bready = true;
        n[0].nmu_in.arvalid = (b_seen && !ar_sent);
        n[0].nmu_in.arid = 3;
        n[0].nmu_in.araddr = addr;
        n[0].nmu_in.arlen = 0;
        n[0].nmu_in.arsize = 5;
        n[0].nmu_in.arburst = 1;
        n[0].nmu_in.rready = true;
        // node 1's master face idle
        n[1].nmu_in.awvalid = false;
        n[1].nmu_in.wvalid = false;
        n[1].nmu_in.arvalid = false;
        n[1].nmu_in.bready = true;
        n[1].nmu_in.rready = true;

        // ---------------- AXI slave stimulus (both nodes) ----------------
        for (int k = 0; k < 2; ++k) slave[k].step(nsu_q[k], n[k].nsu_in);

        // ---------------- posedge: set_inputs / tick / get_outputs ----------------
        for (int k = 0; k < 2; ++k) {
            n[k].nmu.set_inputs(n[k].nmu_in);
            n[k].nmu.tick();
            n[k].nmu.get_outputs(n[k].nmu_out);
            n[k].nsu.set_inputs(n[k].nsu_in);
            n[k].nsu.tick();
            n[k].nsu.get_outputs(n[k].nsu_out);
            n[k].merge.set_inputs(n[k].merge_in);
            n[k].merge.tick();
            n[k].merge.get_outputs(n[k].merge_out);
            n[k].router.set_inputs(n[k].rtr_in);
            n[k].router.tick();
            n[k].router.get_outputs(n[k].rtr_out);
        }

        // AXI handshake bookkeeping against the wire values THIS cycle
        // (registered outputs from the previous tick).
        if (!aw_sent && n[0].nmu_in.awvalid && nmu_q[0].awready) aw_sent = true;
        if (!w_sent && n[0].nmu_in.wvalid && nmu_q[0].wready) w_sent = true;
        if (!b_seen && nmu_q[0].bvalid) b_seen = true;
        if (!ar_sent && n[0].nmu_in.arvalid && nmu_q[0].arready) ar_sent = true;
        if (!r_seen && nmu_q[0].rvalid) {
            r_seen = true;
            rdata_seen = nmu_q[0].rdata;
        }
        if (first_req_flit_cycle < 0 && nmu_q[0].tx_req_valid) first_req_flit_cycle = cyc;
        if (first_local_ready_cycle < 0 && rtr_q[0].rx_req_ready[LOCAL])
            first_local_ready_cycle = cyc;

        // ---------------- register ----------------
        for (int k = 0; k < 2; ++k) {
            nmu_q[k] = n[k].nmu_out;
            nsu_q[k] = n[k].nsu_out;
            merge_q[k] = n[k].merge_out;
            rtr_q[k] = n[k].rtr_out;
        }
    }

    // Staged expectations: each one names the link in the chain that broke, so a
    // failure points at a stage instead of just "no data".
    EXPECT_GE(first_local_ready_cycle, 0) << "router LOCAL rx_req_ready never asserted";
    EXPECT_GE(first_req_flit_cycle, 0) << "NMU never emitted a REQ flit";
    EXPECT_TRUE(b_seen) << "no B response";
    EXPECT_TRUE(r_seen) << "no R response";
    *ok_data = r_seen && (rdata_seen == wdata);
}

// One AXI write then a readback of the same address, node 0 -> node 1 (one XY
// hop east): the flit has to leave the NMU, cross two routers, reach the NSU's
// AXI master face, and the response has to come back the same way.
TEST(NiRouterChain, TwoNodeWriteThenReadbackMatches) {
    bool ok = false;
    run_chain(&ok);
    EXPECT_TRUE(ok) << "readback data does not match what was written";
}

// Negative control for the test above: with REQ egress ready held low and no
// RSP/DAT ingress flit ever delivered, the NMU must produce no B and no R. If
// it does, a passing chain test proves nothing about the fabric.
TEST(NiRouterChain, DeadFabricProducesNoResponses) {
    NmuWrap nmu;
    nmu.init(/*src_id=*/0, /*dat_num_vc=*/1);

    NmuInputs in{};
    NmuOutputs q{};  // registered outputs (previous cycle)
    NmuOutputs out{};

    std::array<uint8_t, 64> wdata{};
    for (int b = 0; b < 64; ++b) wdata[b] = static_cast<uint8_t>(0xA0 + b);

    bool aw_sent = false, w_sent = false, ar_sent = false;
    int b_count = 0, r_count = 0, awready_count = 0;

    for (int cyc = 0; cyc < 400; ++cyc) {
        in = NmuInputs{};
        in.tx_req_ready = false;  // router LOCAL ready stuck 0 (the co-sim symptom)
        in.rx_rsp_valid = false;  // no RSP flit ever arrives
        in.rx_dat_valid = false;  // no DAT flit ever arrives
        in.awvalid = !aw_sent;
        in.awid = 3;
        in.awaddr = 0x1'0000'0000ull;
        in.awlen = 0;
        in.awsize = 5;
        in.awburst = 1;
        in.wvalid = (aw_sent && !w_sent);
        in.wdata = wdata;
        in.wstrb = ~0ull;
        in.wlast = true;
        in.bready = true;
        in.arvalid = !ar_sent;
        in.arid = 4;
        in.araddr = 0x1'0000'0000ull;
        in.arlen = 0;
        in.arsize = 5;
        in.arburst = 1;
        in.rready = true;

        nmu.set_inputs(in);
        nmu.tick();
        nmu.get_outputs(out);

        if (in.awvalid && q.awready) aw_sent = true;
        if (in.wvalid && q.wready) w_sent = true;
        if (in.arvalid && q.arready) ar_sent = true;
        if (q.awready) ++awready_count;
        if (q.bvalid) ++b_count;
        if (q.rvalid) ++r_count;
        q = out;
    }

    EXPECT_GT(awready_count, 0) << "AW never accepted — the stimulus itself did not run";
    EXPECT_EQ(b_count, 0) << "NMU produced a B response with no NoC response flit";
    EXPECT_EQ(r_count, 0) << "NMU produced an R response with no NoC response flit";
}
