// dat_merge_wrap — DPI wrapper for DatMergeWrap, the NI-level merge point for
// the DAT network's shared LOCAL port (S3a T5, controller ruling; translate
// of floo_nw_chimney.sv's wide-link merge, see wrap/dat_merge_wrap.hpp).
//
// Spec §4.3: a port is one physical rx/tx pair. DAT is not single-producer/
// single-consumer at LOCAL like REQ/RSP are -- DataAw/DataW originate at NMU
// and are consumed at NSU, while DataR originates at NSU and is consumed at
// NMU, all sharing dat_router_'s single LOCAL rx/tx pair. This module sits
// between nmu_wrap/nsu_wrap's DAT pins and router_wrap's DAT LOCAL port
// (wired at ni_wrap.sv); NMU/NSU are unaware it is there.
//
// Port naming: nmu_*/nsu_* prefix the NI-facing side (mirrors nmu_wrap.sv's/
// nsu_wrap.sv's own tx_dat_*/rx_dat_* names, from THEIR view); unprefixed
// tx_dat_*/rx_dat_* is the router-facing side (from THIS module's view,
// matching router_wrap.sv's LOCAL-port DAT pins).
//
// Registered-DPI-tick discipline (shared by every wrap in this directory):
// on every posedge clk_i the module samples the PREVIOUS cycle's registered
// wire inputs, pushes them to the C++ model via DPI set_inputs, advances the
// model via tick, pulls outputs via get_outputs, and registers those outputs
// nonblocking so they are visible to SV wires from the NEXT cycle onward.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.

`timescale 1ns/1ps

`ifndef DAT_MERGE_WRAP_SV
`define DAT_MERGE_WRAP_SV

module dat_merge_wrap #(
    parameter int unsigned DAT_NUM_VC     = ni_params_pkg::NOC_NUM_VC_DFLT,
    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  longint unsigned   ctx_i,

    // NMU-facing (from nmu_wrap's own tx_dat_*/rx_dat_* view).
    input  logic                      nmu_tx_dat_valid_i,
    input  logic [DAT_FLIT_WIDTH-1:0] nmu_tx_dat_flit_i,
    output logic [DAT_NUM_VC-1:0]     nmu_tx_dat_crdvalid_o,
    output logic                      nmu_rx_dat_valid_o,
    output logic [DAT_FLIT_WIDTH-1:0] nmu_rx_dat_flit_o,

    // NSU-facing (from nsu_wrap's own tx_dat_*/rx_dat_* view).
    input  logic                      nsu_tx_dat_valid_i,
    input  logic [DAT_FLIT_WIDTH-1:0] nsu_tx_dat_flit_i,
    output logic [DAT_NUM_VC-1:0]     nsu_tx_dat_crdvalid_o,
    output logic                      nsu_rx_dat_valid_o,
    output logic [DAT_FLIT_WIDTH-1:0] nsu_rx_dat_flit_o,

    // Router-facing (this module's own view; connects to router_wrap's DAT
    // LOCAL port, i.e. rx_dat_valid[LOCAL]/tx_dat_valid[LOCAL] etc.).
    output logic                      tx_dat_valid_o,
    output logic [DAT_FLIT_WIDTH-1:0] tx_dat_flit_o,
    input  logic [DAT_NUM_VC-1:0]     tx_dat_crdvalid_i,
    input  logic                      rx_dat_valid_i,
    input  logic [DAT_FLIT_WIDTH-1:0] rx_dat_flit_i,
    output logic [DAT_NUM_VC-1:0]     rx_dat_crdvalid_o
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern; arg order mirrors cmodel_dpi.h DatMerge decls.
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_dat_merge_set_inputs(
        input  longint unsigned            ctx,
        input  bit                    nmu_tx_dat_valid,
        input  bit [DAT_FLIT_WIDTH-1:0] nmu_tx_dat_flit,
        input  bit                    nsu_tx_dat_valid,
        input  bit [DAT_FLIT_WIDTH-1:0] nsu_tx_dat_flit,
        input  bit [DAT_NUM_VC-1:0]   tx_dat_crdvalid,
        input  bit                    rx_dat_valid,
        input  bit [DAT_FLIT_WIDTH-1:0] rx_dat_flit
    );

    import "DPI-C" context function void cmodel_dat_merge_tick(input longint unsigned ctx);

    import "DPI-C" context function void cmodel_dat_merge_get_outputs(
        input  longint unsigned              ctx,
        output bit [DAT_NUM_VC-1:0]     nmu_tx_dat_crdvalid,
        output bit                      nmu_rx_dat_valid,
        output bit [DAT_FLIT_WIDTH-1:0] nmu_rx_dat_flit,
        output bit [DAT_NUM_VC-1:0]     nsu_tx_dat_crdvalid,
        output bit                      nsu_rx_dat_valid,
        output bit [DAT_FLIT_WIDTH-1:0] nsu_rx_dat_flit,
        output bit                      tx_dat_valid,
        output bit [DAT_FLIT_WIDTH-1:0] tx_dat_flit,
        output bit [DAT_NUM_VC-1:0]     rx_dat_crdvalid
    );

    // Lifecycle / error polling lives in tb_top.sv.

    // -------------------------------------------------------------------------
    // Output registers (registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit [DAT_NUM_VC-1:0]     nmu_tx_dat_crdvalid_q;
    bit                      nmu_rx_dat_valid_q;
    bit [DAT_FLIT_WIDTH-1:0] nmu_rx_dat_flit_q;
    bit [DAT_NUM_VC-1:0]     nsu_tx_dat_crdvalid_q;
    bit                      nsu_rx_dat_valid_q;
    bit [DAT_FLIT_WIDTH-1:0] nsu_rx_dat_flit_q;
    bit                      tx_dat_valid_q;
    bit [DAT_FLIT_WIDTH-1:0] tx_dat_flit_q;
    bit [DAT_NUM_VC-1:0]     rx_dat_crdvalid_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            nmu_tx_dat_crdvalid_q <= '0;
            nmu_rx_dat_valid_q    <= '0;
            nmu_rx_dat_flit_q     <= '0;
            nsu_tx_dat_crdvalid_q <= '0;
            nsu_rx_dat_valid_q    <= '0;
            nsu_rx_dat_flit_q     <= '0;
            tx_dat_valid_q        <= '0;
            tx_dat_flit_q         <= '0;
            rx_dat_crdvalid_q     <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_dat_merge_set_inputs(
                ctx_i,
                nmu_tx_dat_valid_i, nmu_tx_dat_flit_i,
                nsu_tx_dat_valid_i, nsu_tx_dat_flit_i,
                tx_dat_crdvalid_i,
                rx_dat_valid_i, rx_dat_flit_i
            );

            // Step 2: advance C++ model one cycle.
            cmodel_dat_merge_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit [DAT_NUM_VC-1:0]     t_nmu_tx_dat_crdvalid;
                bit                      t_nmu_rx_dat_valid;
                bit [DAT_FLIT_WIDTH-1:0] t_nmu_rx_dat_flit;
                bit [DAT_NUM_VC-1:0]     t_nsu_tx_dat_crdvalid;
                bit                      t_nsu_rx_dat_valid;
                bit [DAT_FLIT_WIDTH-1:0] t_nsu_rx_dat_flit;
                bit                      t_tx_dat_valid;
                bit [DAT_FLIT_WIDTH-1:0] t_tx_dat_flit;
                bit [DAT_NUM_VC-1:0]     t_rx_dat_crdvalid;
                cmodel_dat_merge_get_outputs(
                    ctx_i,
                    t_nmu_tx_dat_crdvalid, t_nmu_rx_dat_valid, t_nmu_rx_dat_flit,
                    t_nsu_tx_dat_crdvalid, t_nsu_rx_dat_valid, t_nsu_rx_dat_flit,
                    t_tx_dat_valid, t_tx_dat_flit,
                    t_rx_dat_crdvalid
                );
                nmu_tx_dat_crdvalid_q <= t_nmu_tx_dat_crdvalid;
                nmu_rx_dat_valid_q    <= t_nmu_rx_dat_valid;
                nmu_rx_dat_flit_q     <= t_nmu_rx_dat_flit;
                nsu_tx_dat_crdvalid_q <= t_nsu_tx_dat_crdvalid;
                nsu_rx_dat_valid_q    <= t_nsu_rx_dat_valid;
                nsu_rx_dat_flit_q     <= t_nsu_rx_dat_flit;
                tx_dat_valid_q        <= t_tx_dat_valid;
                tx_dat_flit_q         <= t_tx_dat_flit;
                rx_dat_crdvalid_q     <= t_rx_dat_crdvalid;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive outputs from registered state
    // -------------------------------------------------------------------------

    assign nmu_tx_dat_crdvalid_o = nmu_tx_dat_crdvalid_q;
    assign nmu_rx_dat_valid_o    = nmu_rx_dat_valid_q;
    assign nmu_rx_dat_flit_o     = nmu_rx_dat_flit_q;
    assign nsu_tx_dat_crdvalid_o = nsu_tx_dat_crdvalid_q;
    assign nsu_rx_dat_valid_o    = nsu_rx_dat_valid_q;
    assign nsu_rx_dat_flit_o     = nsu_rx_dat_flit_q;
    assign tx_dat_valid_o        = tx_dat_valid_q;
    assign tx_dat_flit_o         = tx_dat_flit_q;
    assign rx_dat_crdvalid_o     = rx_dat_crdvalid_q;

endmodule

`endif  // DAT_MERGE_WRAP_SV
