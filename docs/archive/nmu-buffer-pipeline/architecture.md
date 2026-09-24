# Buffer placement and pipeline contract

### NMU RTL transport buffers and output slices (2026-09-24)

The RTL request path is ID remap, AW/W/AR input CDC FIFOs, SAM and ordering admission, write context, packet encoding, channel/VC assignment, then REQ and per-write-VC DAT output FIFOs. Encoding itself contains no transaction FIFO. One AW context is retained until the final W beat enters its output slice. The next AW may replace it on that cycle. External AW outstanding capacity still includes the input FIFO and ID/order tables.

The response path is independent B/control-R and per-read-VC DAT input FIFOs, R beat selection, response decoding, ordering/ROB, B/R output CDC FIFOs, then ID restoration. Input-buffer channel decode only selects storage. Pack/unpack output slices are optional and default to bypass. ROB storage is separate from transport FIFO capacity.

| NMU top parameter | Default | Meaning |
|---|---|---|
| `AXI_FIFO_DEPTH` | 32 | Compatibility default for the five AXI channel depths below |
| `AW_FIFO_DEPTH`, `W_FIFO_DEPTH`, `AR_FIFO_DEPTH` | `AXI_FIFO_DEPTH` | Independently configurable TX input CDC FIFO depths |
| `B_FIFO_DEPTH`, `R_FIFO_DEPTH` | `AXI_FIFO_DEPTH` | Independently configurable RX output CDC FIFO depths |
| `REQ_FIFO_DEPTH` | 32 | Encoded REQ output FIFO entries |
| `DAT_TX_FIFO_DEPTH` | 32 | Encoded DAT output FIFO entries per active write VC |
| `B_RX_FIFO_DEPTH`, `R_RX_FIFO_DEPTH` | 32 | RSP input B and control-R FIFO entries |
| `DAT_RX_VC_DEPTH` | 32 | DAT input entries per active read VC, matching upstream advertised credit |
| `REQ_AW_REG_TYPE`, `REQ_W_REG_TYPE`, `REQ_AR_REG_TYPE` | 0 | Independent REQ encoding output slices |
| `DAT_AW_REG_TYPE`, `DAT_W_REG_TYPE` | 0 | Independent DAT encoding output slices |
| `B_REG_TYPE`, `R_REG_TYPE` | 0 | Independent response decoding output slices |

Register types use the existing slice implementation: 0 bypass, 1 simple, 2 spill. CDC depths are powers of two and at least two. `DAT_RX_VC_DEPTH` retains that restriction. Synchronous REQ depth is a positive power of two at NMU top; the remaining synchronous output/class depths must be positive. Existing ROB defaults and generated C++ defaults are unchanged. The module defaults above override the older common NI depth defaults for this RTL integration.

DAT VC assignment uses local output FIFO space. It does not consume downstream credit. The link output arbiter selects a nonempty VC with credit, preserves order within each VC, and consumes credit on the actual transmitted flit. Credit returned in the same cycle can authorize that transfer. Registered receive credit returns one cycle after the receive FIFO pop, independent of a later unpack output register or ROB retirement.

## Capacity-test adaptation

The default 32-entry B output CDC FIFO can absorb the original single-ID limit of 32 writes, so that workload cannot fill the B receive FIFO. Capacity cases now issue 320 requests over eight IDs and four destinations, with each ID fixed to one destination. Each ID has 40 requests, exceeding the 32 outstanding limit. Control uses four-beat bursts and data uses eight-beat bursts. Destination-local addresses are disjoint and packed within the existing SAM apertures. No address-map size or ROB parameter is changed.

## Storage comparison

For the old packetizer with depth D, payload storage was D times (three AW request records + one AR request record + two write-beat records). Each write-beat record contained a full AXI W payload, full AW request record and beat index. This includes the old owner queue.

The replacement contains REQ_FIFO_DEPTH times REQ flit width, plus NUM_WR_VC times DAT_TX_FIFO_DEPTH times DAT flit width, plus one AW context and beat index. Optional register slices add their own one/two-entry channel storage. The approved increase in AXI CDC and RSP input defaults to 32 is a separate storage increase and must not be presented as an area reduction. No synthesis area or frequency result is available.

Output FIFOs remain non-fall-through. Empty-to-first-output latency is one cycle at these boundaries. Pack/unpack add zero cycles in bypass and one minimum cycle when registered. DAT arbitration has no extra payload register. With queued data and credit, the selected output transfers every cycle; a full FIFO must first release space before accepting another input because the unchanged cc_fifo forbids a push while full.

## Timing and area limits

Bypass keeps encoding/decoding on the surrounding combinational path; timing closure may require enabling the existing slices. Per-VC output arbitration adds a mux and credit eligibility path after FIFO heads. Credit and ready fanout scale with the configured VC count. The one-entry write context removes duplicated metadata queues but reduces AW processing lookahead; AXI admission remains buffered. These are RTL structural observations, not synthesis frequency or area measurements.
