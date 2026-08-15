// Two-node NI/router chain at the wrap layer — the shape noc_fabric_*.sv
// wires: NmuWrap + NsuWrap + DatMergeWrap + RouterWrap per node, ni<->router
// crossing at LOCAL (tile) or at a face port (peripheral), EAST/WEST
// inter-router link, one AXI write + readback from node 0 to node 1.
//
// Fills the gap that let S3a T5 ship broken: every other wrap test drives one
// wrap against mocks, so nothing exercised the four of them wired together and
// co-sim was the only end-to-end check. Keep the wiring here mirroring
// gen_tb_top.py's emit_fabric() connection list.
#include "wrap/dat_merge_wrap.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nsu_wrap.hpp"
#include "wrap/router_wrap.hpp"
#include "common/tmp_path.hpp"

#include <fstream>
#include <gtest/gtest.h>

using namespace ni::cmodel::wrap;

namespace {

constexpr std::size_t LOCAL = 0, EAST = 2, WEST = 4;

// NmuWrap::init takes a topology YAML, no default map. The chain runs on a
// 2x2 mesh, so the shipped 2x2 is the map; tests needing a different shape
// write their own YAML below.
constexpr const char* kTopologyYaml = TOPOLOGY_DIR "/mesh_2x2_vc1.yaml";
// Same 2x2 plus a peripherals block. Declares { x: 0, y: 0, face: x }, which is
// port 1 at (0,0) -- the configuration a non-zero requester port needs.
constexpr const char* kPeriphTopologyYaml = TOPOLOGY_DIR "/mesh_2x2_vc1_periph.yaml";

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

static void run_chain(bool* ok_data, uint8_t requester_port_id = 0) {
    Node n[2];
    MemSlave slave[2];

    // A requester on a non-zero port is a peripheral, so its NI crosses at the
    // face port the port_id names rather than at LOCAL -- port 1 at (0,0) is
    // the x face, and x == 0 makes that WEST. The whole NI crossing moves, not
    // just the NMU: DatMergeWrap is the NI's DAT port adapter and owns the
    // router-facing credit pool (nmu_wrap.hpp:123-126 seeds the NMU's own DAT
    // credit to NMU_ARBITER_FIFO_DEPTH, the merge's per-input depth, expressly
    // NOT the router's input depth), so an NMU wired straight at the router
    // would send against the wrong pool. Node 0's WEST is free -- the
    // inter-router link is node0 EAST <-> node1 WEST.
    const std::size_t ni_port[2] = {requester_port_id != 0 ? WEST : LOCAL, LOCAL};
    const char* topology = requester_port_id != 0 ? kPeriphTopologyYaml : kTopologyYaml;

    for (int k = 0; k < 2; ++k) {
        n[k].nmu.init(topology, /*src_id=*/static_cast<uint8_t>(k),
                      /*port_id=*/(k == 0) ? requester_port_id : 0,
                      /*dat_num_vc=*/1);
        // The NSU sits at the same endpoint as its NMU, so it takes the same
        // port_id: nsu/depacketize.hpp asserts dst_port_id == port_id_ on every
        // arriving request, and a peripheral NSU left at 0 would reject anything
        // addressed to it. Inert while node 0 is never a request target here.
        n[k].nsu.init(/*src_id=*/static_cast<uint8_t>(k),
                      /*port_id=*/(k == 0) ? requester_port_id : 0, /*dat_num_vc=*/1);
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
    const uint64_t addr =
        0x100000000ull;  // dst tile 1 (mesh_2x2_vc1: 4 GiB block stride, 32 MB memory tile)
    std::array<uint8_t, 64> wdata{};
    for (int b = 0; b < 64; ++b) wdata[b] = static_cast<uint8_t>(0xA0 + b);

    bool aw_sent = false, w_sent = false, b_seen = false;
    bool ar_sent = false, r_seen = false;
    std::array<uint8_t, 64> rdata_seen{};

    int first_req_flit_cycle = -1;
    int first_dat_flit_cycle = -1;
    int first_local_ready_cycle = -1;

    for (int cyc = 0; cyc < 400; ++cyc) {
        // ---------------- comb wiring from registered outputs ----------------
        for (int k = 0; k < 2; ++k) {
            // NI crossing (gen_tb_top.py: ni.tx_* -> router rx_*[port]). LOCAL
            // for a tile NI, the face port for a peripheral one -- see ni_port.
            const std::size_t p = ni_port[k];
            n[k].rtr_in.rx_req_valid[p] = nmu_q[k].tx_req_valid;
            n[k].rtr_in.rx_req_flit[p] = nmu_q[k].tx_req_flit;
            n[k].rtr_in.tx_req_ready[p] = nsu_q[k].rx_req_ready;
            n[k].nmu_in.tx_req_ready = rtr_q[k].rx_req_ready[p];
            n[k].nsu_in.rx_req_valid = rtr_q[k].tx_req_valid[p];
            n[k].nsu_in.rx_req_flit = rtr_q[k].tx_req_flit[p];

            n[k].rtr_in.rx_rsp_valid[p] = nsu_q[k].tx_rsp_valid;
            n[k].rtr_in.rx_rsp_flit[p] = nsu_q[k].tx_rsp_flit;
            n[k].rtr_in.tx_rsp_ready[p] = nmu_q[k].rx_rsp_ready;
            n[k].nsu_in.tx_rsp_ready = rtr_q[k].rx_rsp_ready[p];
            n[k].nmu_in.rx_rsp_valid = rtr_q[k].tx_rsp_valid[p];
            n[k].nmu_in.rx_rsp_flit = rtr_q[k].tx_rsp_flit[p];

            // DAT via merge.
            n[k].rtr_in.rx_dat_valid[p] = merge_q[k].tx_dat_valid;
            n[k].rtr_in.rx_dat_flit[p] = merge_q[k].tx_dat_flit;
            n[k].rtr_in.tx_dat_crdvalid[p] = merge_q[k].rx_dat_crdvalid;
            n[k].merge_in.rx_dat_valid = rtr_q[k].tx_dat_valid[p];
            n[k].merge_in.rx_dat_flit = rtr_q[k].tx_dat_flit[p];
            n[k].merge_in.tx_dat_crdvalid = rtr_q[k].rx_dat_crdvalid[p];
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
        if (first_dat_flit_cycle < 0 && nmu_q[0].tx_dat_valid) first_dat_flit_cycle = cyc;
        if (first_local_ready_cycle < 0 && rtr_q[0].rx_req_ready[ni_port[0]])
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
    EXPECT_GE(first_local_ready_cycle, 0) << "router rx_req_ready never asserted at the NI port";
    EXPECT_GE(first_req_flit_cycle, 0) << "NMU never emitted a REQ flit";
    // S3a T6 steering: this write's addr is data class (default SAM), so AW+W
    // must ride DAT -- confirms the stage's "three networks carry traffic"
    // success criterion end to end (REQ carries the AR above, RSP carries B).
    EXPECT_GE(first_dat_flit_cycle, 0)
        << "NMU never emitted a DAT flit -- Data-class AW/W steering regressed";
    EXPECT_TRUE(b_seen) << "no B response";
    EXPECT_TRUE(r_seen) << "no R response";
    *ok_data = r_seen && (rdata_seen == wdata);
}

// S3a T6 stage design §1 rationale case: masters A and B (two DIFFERENT
// NMUs) write the SAME NSU C concurrently. Before T6, AW rode REQ and W rode
// DAT -- two independently arbitrated networks with nothing stopping REQ
// from delivering AW_A,AW_B while DAT delivers W_B,W_A, mis-pairing AW_A
// with W_B at the slave (the design's counterexample). T6 puts both AW and W
// of one worm on the SAME network, so the source-side {AW,W} wormhole lock
// (already atomic per NMU, S3a T4/T5) plus C's own per-output wormhole lock
// (router::Router, credit DAT class) make that interleave structurally
// impossible regardless of which worm wins the race.
//
// Row topology A(0,0) - C(1,0) - B(2,0), mesh_x=3: both masters are one XY
// hop from the target, so contention lands at C's own inbound arbitration,
// not multi-hop routing noise. The SAM declares a 4x2 mesh while the routers
// are wired as a 3-wide row: the map states the smallest LEGAL mesh containing
// that row, because mesh dimensions are powers of two (sam_yaml.hpp), and the
// x=3 and y=1 tiles are filler nothing targets -- only the y=0 row is
// physically wired. The fourth column moves no address this test uses: clog2(3)
// and clog2(4) are both 2, so the coordinate field and every tile base in the
// y=0 row sit exactly where they did.
static std::string write_two_master_sam() {
    auto path = ni::cmodel::testing::unique_temp_path("two_master_sam.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 4, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x100000 }\n"
                           "    - { x: 1, y: 0, size: 0x100000 }\n"
                           "    - { x: 2, y: 0, size: 0x100000 }\n"
                           "    - { x: 3, y: 0, size: 0x100000 }\n"
                           "    - { x: 0, y: 1, size: 0x100000 }\n"
                           "    - { x: 1, y: 1, size: 0x100000 }\n"
                           "    - { x: 2, y: 1, size: 0x100000 }\n"
                           "    - { x: 3, y: 1, size: 0x100000 }\n";
    return path;
}

TEST(NiRouterChain, TwoMastersOneNsuConcurrentWritesDoNotCrossWire) {
    constexpr std::size_t A = 0, C = 1, B = 2;

    Node n[3];
    MemSlave slave[3];
    const std::string sam_path = write_two_master_sam();

    // A and B are masters (need the row SAM to translate their AW/AR
    // addresses to C's tile); C's own master face is idle, so its topology is
    // never consulted. dst tile 1 (x=1,y=0, C's position) base = 0x100000.
    n[A].nmu.init(sam_path.c_str(), /*src_id=*/0, /*port_id=*/0, /*dat_num_vc=*/1);
    n[B].nmu.init(sam_path.c_str(), /*src_id=*/2, /*port_id=*/0, /*dat_num_vc=*/1);
    n[C].nmu.init(kTopologyYaml, /*src_id=*/1, /*port_id=*/0, /*dat_num_vc=*/1);
    for (std::size_t k : {A, C, B}) {
        n[k].nsu.init(/*src_id=*/static_cast<uint8_t>(k), /*port_id=*/0, /*dat_num_vc=*/1);
        n[k].merge.init(1);
    }
    n[A].router.init(/*x=*/0, /*y=*/0, /*mesh_x=*/3, /*mesh_y=*/1, /*dat_num_vc=*/1);
    n[C].router.init(/*x=*/1, /*y=*/0, /*mesh_x=*/3, /*mesh_y=*/1, /*dat_num_vc=*/1);
    n[B].router.init(/*x=*/2, /*y=*/0, /*mesh_x=*/3, /*mesh_y=*/1, /*dat_num_vc=*/1);

    NmuOutputs nmu_q[3]{};
    NsuOutputs nsu_q[3]{};
    DatMergeOutputs merge_q[3]{};
    RouterOutputs rtr_q[3]{};

    // dst tile 1 (C) base = 0x100000; distinct, non-overlapping 32 B lines
    // for A and B (offsets 64 B apart) so a cross-wired write is visible as
    // wrong data at the WRONG address, exactly the counterexample's symptom.
    constexpr uint64_t kAddrA = 0x100000 + 0x00;
    constexpr uint64_t kAddrB = 0x100000 + 0x40;
    constexpr uint8_t kIdA = 3, kIdB = 4;
    std::array<uint8_t, 64> wdataA{}, wdataB{};
    for (int b = 0; b < 64; ++b) {
        wdataA[b] = static_cast<uint8_t>(0xA0 + b);
        wdataB[b] = static_cast<uint8_t>(0xB0 + b);
    }

    bool aw_sent_a = false, w_sent_a = false, b_seen_a = false;
    bool aw_sent_b = false, w_sent_b = false, b_seen_b = false;
    bool ar_sent_a = false, r_seen_a = false;
    bool ar_sent_b = false, r_seen_b = false;
    std::array<uint8_t, 64> rdata_a{}, rdata_b{};
    int overlap_cycles = 0;  // both writes racing C's inbound in the same cycle

    for (int cyc = 0; cyc < 600; ++cyc) {
        // ---------------- comb wiring from registered outputs ----------------
        for (std::size_t k : {A, C, B}) {
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
        // Row links: A.EAST <-> C.WEST, C.EAST <-> B.WEST.
        auto cross = [&](std::size_t lo, std::size_t hi) {
            n[lo].rtr_in.rx_req_valid[EAST] = rtr_q[hi].tx_req_valid[WEST];
            n[lo].rtr_in.rx_req_flit[EAST] = rtr_q[hi].tx_req_flit[WEST];
            n[lo].rtr_in.tx_req_ready[EAST] = rtr_q[hi].rx_req_ready[WEST];
            n[hi].rtr_in.rx_req_valid[WEST] = rtr_q[lo].tx_req_valid[EAST];
            n[hi].rtr_in.rx_req_flit[WEST] = rtr_q[lo].tx_req_flit[EAST];
            n[hi].rtr_in.tx_req_ready[WEST] = rtr_q[lo].rx_req_ready[EAST];

            n[lo].rtr_in.rx_rsp_valid[EAST] = rtr_q[hi].tx_rsp_valid[WEST];
            n[lo].rtr_in.rx_rsp_flit[EAST] = rtr_q[hi].tx_rsp_flit[WEST];
            n[lo].rtr_in.tx_rsp_ready[EAST] = rtr_q[hi].rx_rsp_ready[WEST];
            n[hi].rtr_in.rx_rsp_valid[WEST] = rtr_q[lo].tx_rsp_valid[EAST];
            n[hi].rtr_in.rx_rsp_flit[WEST] = rtr_q[lo].tx_rsp_flit[EAST];
            n[hi].rtr_in.tx_rsp_ready[WEST] = rtr_q[lo].rx_rsp_ready[EAST];

            n[lo].rtr_in.rx_dat_valid[EAST] = rtr_q[hi].tx_dat_valid[WEST];
            n[lo].rtr_in.rx_dat_flit[EAST] = rtr_q[hi].tx_dat_flit[WEST];
            n[lo].rtr_in.tx_dat_crdvalid[EAST] = rtr_q[hi].rx_dat_crdvalid[WEST];
            n[hi].rtr_in.rx_dat_valid[WEST] = rtr_q[lo].tx_dat_valid[EAST];
            n[hi].rtr_in.rx_dat_flit[WEST] = rtr_q[lo].tx_dat_flit[EAST];
            n[hi].rtr_in.tx_dat_crdvalid[WEST] = rtr_q[lo].rx_dat_crdvalid[EAST];
        };
        cross(A, C);
        cross(C, B);

        // ---------------- AXI master stimulus: A and B concurrently ----------------
        n[A].nmu_in.awvalid = aw_sent_a ? false : true;
        n[A].nmu_in.awid = kIdA;
        n[A].nmu_in.awaddr = kAddrA;
        n[A].nmu_in.awlen = 0;
        n[A].nmu_in.awsize = 5;
        n[A].nmu_in.awburst = 1;
        n[A].nmu_in.wvalid = (!w_sent_a && aw_sent_a);
        n[A].nmu_in.wdata = wdataA;
        n[A].nmu_in.wstrb = ~0ull;
        n[A].nmu_in.wlast = true;
        n[A].nmu_in.bready = true;
        n[A].nmu_in.arvalid = (b_seen_a && b_seen_b && !ar_sent_a);
        n[A].nmu_in.arid = kIdA;
        n[A].nmu_in.araddr = kAddrA;
        n[A].nmu_in.arlen = 0;
        n[A].nmu_in.arsize = 5;
        n[A].nmu_in.arburst = 1;
        n[A].nmu_in.rready = true;

        n[B].nmu_in.awvalid = aw_sent_b ? false : true;
        n[B].nmu_in.awid = kIdB;
        n[B].nmu_in.awaddr = kAddrB;
        n[B].nmu_in.awlen = 0;
        n[B].nmu_in.awsize = 5;
        n[B].nmu_in.awburst = 1;
        n[B].nmu_in.wvalid = (!w_sent_b && aw_sent_b);
        n[B].nmu_in.wdata = wdataB;
        n[B].nmu_in.wstrb = ~0ull;
        n[B].nmu_in.wlast = true;
        n[B].nmu_in.bready = true;
        n[B].nmu_in.arvalid = (b_seen_a && b_seen_b && !ar_sent_b);
        n[B].nmu_in.arid = kIdB;
        n[B].nmu_in.araddr = kAddrB;
        n[B].nmu_in.arlen = 0;
        n[B].nmu_in.arsize = 5;
        n[B].nmu_in.arburst = 1;
        n[B].nmu_in.rready = true;

        n[C].nmu_in.awvalid = false;
        n[C].nmu_in.wvalid = false;
        n[C].nmu_in.arvalid = false;
        n[C].nmu_in.bready = true;
        n[C].nmu_in.rready = true;

        // ---------------- AXI slave stimulus (all nodes; only C sees traffic) ----------------
        for (std::size_t k : {A, C, B}) slave[k].step(nsu_q[k], n[k].nsu_in);

        // ---------------- posedge ----------------
        for (std::size_t k : {A, C, B}) {
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

        // Real concurrency evidence: both worms racing C's inbound DAT ports
        // in the same cycle (A arrives via C's WEST, B via C's EAST).
        if (n[C].rtr_in.rx_dat_valid[WEST] && n[C].rtr_in.rx_dat_valid[EAST]) ++overlap_cycles;

        if (!aw_sent_a && n[A].nmu_in.awvalid && nmu_q[A].awready) aw_sent_a = true;
        if (!w_sent_a && n[A].nmu_in.wvalid && nmu_q[A].wready) w_sent_a = true;
        if (!b_seen_a && nmu_q[A].bvalid) b_seen_a = true;
        if (!ar_sent_a && n[A].nmu_in.arvalid && nmu_q[A].arready) ar_sent_a = true;
        if (!r_seen_a && nmu_q[A].rvalid) {
            r_seen_a = true;
            rdata_a = nmu_q[A].rdata;
        }

        if (!aw_sent_b && n[B].nmu_in.awvalid && nmu_q[B].awready) aw_sent_b = true;
        if (!w_sent_b && n[B].nmu_in.wvalid && nmu_q[B].wready) w_sent_b = true;
        if (!b_seen_b && nmu_q[B].bvalid) b_seen_b = true;
        if (!ar_sent_b && n[B].nmu_in.arvalid && nmu_q[B].arready) ar_sent_b = true;
        if (!r_seen_b && nmu_q[B].rvalid) {
            r_seen_b = true;
            rdata_b = nmu_q[B].rdata;
        }

        // ---------------- register ----------------
        for (std::size_t k : {A, C, B}) {
            nmu_q[k] = n[k].nmu_out;
            nsu_q[k] = n[k].nsu_out;
            merge_q[k] = n[k].merge_out;
            rtr_q[k] = n[k].rtr_out;
        }
    }

    EXPECT_GT(overlap_cycles, 0)
        << "A's and B's worms never actually raced C's inbound DAT ports in the same cycle -- "
           "this run proves nothing about contention";
    ASSERT_TRUE(b_seen_a) << "master A never got its B response";
    ASSERT_TRUE(b_seen_b) << "master B never got its B response";
    ASSERT_TRUE(r_seen_a) << "master A never got its R response";
    ASSERT_TRUE(r_seen_b) << "master B never got its R response";
    // The counterexample's symptom: a split AW/W pairing writes the WRONG
    // data at an address (or the right data at the wrong one). Checking both
    // readbacks against their OWN source's pattern catches either failure
    // mode -- not just "some corruption happened somewhere."
    EXPECT_EQ(rdata_a, wdataA) << "master A read back data that does not match what A wrote -- "
                                  "AW/W worm cross-wired with master B's";
    EXPECT_EQ(rdata_b, wdataB) << "master B read back data that does not match what B wrote -- "
                                  "AW/W worm cross-wired with master A's";
}

// One AXI write then a readback of the same address, node 0 -> node 1 (one XY
// hop east): the flit has to leave the NMU, cross two routers, reach the NSU's
// AXI master face, and the response has to come back the same way.
TEST(NiRouterChain, TwoNodeWriteThenReadbackMatches) {
    bool ok = false;
    run_chain(&ok);
    EXPECT_TRUE(ok) << "readback data does not match what was written";
}

// The same chain with the requesting NI on port 1 -- a real x-face peripheral
// at (0,0), crossing at WEST instead of LOCAL, declared by the peripherals
// block of mesh_2x2_vc1_periph.yaml. The target is still a port-0 tile at node
// 1, so only the return path carries a non-zero port.
//
// Two things have to hold at once, and each fails a different way. The echo:
// the NSU stamps src_port_id back as dst_port_id, and nmu::Depacketize aborts
// on any response whose dst_port_id is not 1, so a hardcoded or zeroed echo
// anywhere on the return path kills the run -- a bug invisible to every port-0
// test, which is why this test exists. The ejection: route_compute resolves
// dst_port_id 1 to WEST at x == 0, so a response reaching node 0 leaves by the
// face port. Wire the NI at LOCAL instead and the B is ejected to an
// unconnected port and silently lost.
TEST(NiRouterChain, NonZeroRequesterPortCompletesAndTheEchoNamesItBack) {
    bool ok = false;
    run_chain(&ok, /*requester_port_id=*/1);
    EXPECT_TRUE(ok) << "readback data does not match what was written from the port-1 NMU";
}

// Negative control for the test above: with REQ egress ready held low and no
// RSP/DAT ingress flit ever delivered, the NMU must produce no B and no R. If
// it does, a passing chain test proves nothing about the fabric.
TEST(NiRouterChain, DeadFabricProducesNoResponses) {
    NmuWrap nmu;
    nmu.init(kTopologyYaml, /*src_id=*/0, /*port_id=*/0, /*dat_num_vc=*/1);

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
        in.awaddr = 0x100000ull;
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
        in.araddr = 0x100000ull;
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

// Dual-class end-to-end, through the real fabric (spec :348, S3a T6): node 0
// writes+reads both a config-space (narrow) tile and a memory-space (data)
// tile on node 1 in one run, then closes with the S3a T3 same-ID cross-class
// read ordering guard's integration proof.
//
// The guard (nmu/rob.hpp's prev_cls_read_ term, already unit-tested) only
// becomes LIVE once DataR actually rides a different network than NarrowR
// (T6). Two same-id AR to the same dst, submitted config-then-memory: config
// (Narrow) rides REQ out / RSP back; memory (Data) rides REQ out / DAT back.
// Without the guard's class term, both take the same-destination bypass and
// race independently-arbitrated networks -- and per stage design §8 the DAT
// credit Router is a deeper pipeline than RSP's SimpleRouter (2-3 stages vs
// 1-2), so the data-class read submitted FIRST could genuinely surface
// SECOND at the master, an AXI4 IHI 0022 §A5.3 violation. The guard forces
// the class change onto the RoB path, which retires by submission order
// regardless of which network answers first.
TEST(NiRouterChain, DualClassEndToEndAndCrossClassReadOrder) {
    Node n[2];
    MemSlave slave[2];

    auto path = ni::cmodel::testing::unique_temp_path("dual_class_sam.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           // block_size declared explicitly, as every shipped topology
                           // does: node stride is 2x the memory tile so the node's
                           // config tile fits inside it.
                           "  block_size: 0x200000\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x100000 }\n"
                           "    - { x: 1, y: 0, size: 0x100000 }\n"
                           "    - { x: 0, y: 1, size: 0x100000 }\n"
                           "    - { x: 1, y: 1, size: 0x100000 }\n"
                           // Spec §5.1: every node owns one region per space, so
                           // config covers the mesh even though only node 1's tile
                           // is addressed below. Raster order after the memory
                           // tiles, matching the shipped topology YAMLs.
                           "    - { x: 0, y: 0, size: 0x1000, space: config }\n"
                           "    - { x: 1, y: 0, size: 0x1000, space: config }\n"
                           "    - { x: 0, y: 1, size: 0x1000, space: config }\n"
                           "    - { x: 1, y: 1, size: 0x1000, space: config }\n";

    // ROB Enabled: the T3 guard's class-change-forces-RoB-fallback path only
    // exists in Enabled mode (Disabled mode's single-outstanding interlock is
    // already class-agnostic by construction, stage design §2 scope notes).
    n[0].nmu.init(path.c_str(), /*src_id=*/0, /*port_id=*/0, /*dat_num_vc=*/1, ni::NMU_QUEUE_DEPTH,
                  ni::cmodel::nmu::RobMode::Enabled);
    n[1].nmu.init(kTopologyYaml, /*src_id=*/1, /*port_id=*/0, /*dat_num_vc=*/1);
    for (int k = 0; k < 2; ++k) {
        n[k].nsu.init(/*src_id=*/static_cast<uint8_t>(k), /*port_id=*/0, /*dat_num_vc=*/1);
        n[k].merge.init(1);
        n[k].router.init(/*x=*/static_cast<uint8_t>(k), /*y=*/0, /*mesh_x=*/2, /*mesh_y=*/2,
                         /*dat_num_vc=*/1);
    }

    NmuOutputs nmu_q[2]{};
    NsuOutputs nsu_q[2]{};
    DatMergeOutputs merge_q[2]{};
    RouterOutputs rtr_q[2]{};

    // Addresses ride through untouched, so MemSlave is keyed by the request
    // address itself and the two spaces are already distinct.
    constexpr uint64_t kMemAddr = 0x200000 + 0x100;  // node 1's memory tile (base = 1 * block_size)
    // Node 1's config tile sits inside its own block, above its memory tile:
    // base = 1 * block_size + 0x100000.
    constexpr uint64_t kCfgAddr = 0x300000;  // node 1's config tile base
    constexpr uint8_t kWriteId = 3, kOrderId = 5;
    std::array<uint8_t, 64> wdata_mem{}, wdata_cfg{};
    for (int b = 0; b < 64; ++b) wdata_mem[b] = static_cast<uint8_t>(0xA0 + b);
    wdata_cfg.fill(0);
    for (int b = 0; b < 8; ++b) wdata_cfg[b] = static_cast<uint8_t>(0xC0 + b);  // lane 0, 8 B

    // write_stage: 0 = memory-tile write, 1 = config-tile write, 2 = both done.
    int write_stage = 0;
    bool aw_sent = false, w_sent = false, b_seen = false;
    // ar_stage: 0 = offer memory AR (id=kOrderId), 1 = offer config AR (same
    // id), 2 = both submitted. Both ride the SAME AR channel back to back, so
    // both are outstanding together well before either R returns.
    int ar_stage = 0;
    bool ar_sent = false;
    std::vector<std::array<uint8_t, 64>> r_in_arrival_order;

    for (int cyc = 0; cyc < 600; ++cyc) {
        for (int k = 0; k < 2; ++k) {
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
        const bool writing = write_stage < 2;
        n[0].nmu_in.awvalid = writing && !aw_sent;
        n[0].nmu_in.awid = kWriteId;
        n[0].nmu_in.awaddr = (write_stage == 0) ? kMemAddr : kCfgAddr;
        n[0].nmu_in.awlen = 0;
        n[0].nmu_in.awsize = (write_stage == 0) ? 5 : 3;
        n[0].nmu_in.awburst = 1;
        n[0].nmu_in.wvalid = writing && aw_sent && !w_sent;
        n[0].nmu_in.wdata = (write_stage == 0) ? wdata_mem : wdata_cfg;
        n[0].nmu_in.wstrb = (write_stage == 0) ? ~0ull : 0xFFull;
        n[0].nmu_in.wlast = true;
        n[0].nmu_in.bready = true;
        n[0].nmu_in.arvalid = !writing && (ar_stage < 2) && !ar_sent;
        n[0].nmu_in.arid = kOrderId;
        n[0].nmu_in.araddr = (ar_stage == 0) ? kMemAddr : kCfgAddr;
        n[0].nmu_in.arlen = 0;
        n[0].nmu_in.arsize = (ar_stage == 0) ? 5 : 3;
        n[0].nmu_in.arburst = 1;
        n[0].nmu_in.rready = true;
        n[1].nmu_in.awvalid = false;
        n[1].nmu_in.wvalid = false;
        n[1].nmu_in.arvalid = false;
        n[1].nmu_in.bready = true;
        n[1].nmu_in.rready = true;

        for (int k = 0; k < 2; ++k) slave[k].step(nsu_q[k], n[k].nsu_in);

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

        if (writing) {
            if (!aw_sent && n[0].nmu_in.awvalid && nmu_q[0].awready) aw_sent = true;
            if (!w_sent && n[0].nmu_in.wvalid && nmu_q[0].wready) w_sent = true;
            if (!b_seen && nmu_q[0].bvalid) b_seen = true;
            if (b_seen) {
                ++write_stage;
                aw_sent = w_sent = b_seen = false;
            }
        } else if (ar_stage < 2) {
            if (!ar_sent && n[0].nmu_in.arvalid && nmu_q[0].arready) {
                ar_sent = true;
                ++ar_stage;
                ar_sent = false;  // re-arm for the second AR offer
            }
        }
        if (nmu_q[0].rvalid) r_in_arrival_order.push_back(nmu_q[0].rdata);

        for (int k = 0; k < 2; ++k) {
            nmu_q[k] = n[k].nmu_out;
            nsu_q[k] = n[k].nsu_out;
            merge_q[k] = n[k].merge_out;
            rtr_q[k] = n[k].rtr_out;
        }
    }

    ASSERT_EQ(write_stage, 2) << "memory-tile and config-tile writes did not both complete";
    ASSERT_EQ(ar_stage, 2) << "both cross-class ARs were not both submitted";
    ASSERT_EQ(r_in_arrival_order.size(), 2u)
        << "expected exactly 2 R beats (memory-class then config-class)";
    EXPECT_EQ(r_in_arrival_order[0], wdata_mem)
        << "same-ID cross-class read order violated: the memory-class (Data, DAT-network) read, "
           "submitted FIRST, must surface at the master BEFORE the config-class (Narrow, "
           "RSP-network) read submitted second -- S3a T3 guard regression";
    EXPECT_EQ(r_in_arrival_order[1], wdata_cfg)
        << "same-ID cross-class read order violated: the config-class read arrived out of "
           "submission order";
}
