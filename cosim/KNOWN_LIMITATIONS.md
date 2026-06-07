# cosim/ — Known Limitations

## Resolved in Stage 5b

### §2 Multi-beat W burst wire visibility — ARCHITECTURALLY RESOLVED; wb2axip-verification scope-blocked

Stage 5a snapshot model lost beats W[1..N-1] of multi-beat bursts. β tick
discipline (registered SV wires, 1-cycle latency per hop) makes every beat
wire-visible. This is independently verified at the C++ adapter layer by
`c_model/tests/cosim/test_nmu_shell_adapter.cpp` test
`NmuShellAdapter.multi_beat_w_burst_visible_per_cycle` — proves each of 8 W
beats from an AWLEN=7 burst is observable one-per-cycle on the wire bundle.

**End-to-end wb2axip verification of multi-beat W bursts is BLOCKED** by an
upstream wb2axip constraint (see §6). Multi-beat scenarios run via T10 unit
test only; not exercised through `CosimIntegration` ctest set.

Last verified: 2026-06-05 (T10 unit test PASS commit 852fafa).

### §3 cmodel_finalize not called on timeout — RESOLVED

Stage 5b DPI error propagation (return code + SV `$fatal`) calls
`cmodel_finalize()` at SV side cycle end before `$fatal`. See spec §7.5.

## Carried unchanged from Stage 5a

### §1 faxi_wstrb.v permissive stub

`cosim/sv/wb2axip/faxi_wstrb.v` was created as a permissive stub during the
Stage 5a build-fix pass (commit `822a780`). `o_valid` is hardwired to `1'b1`,
disabling WSTRB alignment checking. Stage 5b carries this stub unchanged.

Follow-up: pull a proper upstream `faxi_wstrb.v` or implement the alignment
check natively.

### §4 read_dump tmp accumulation

`c_model/include/cosim/master_shell_adapter.hpp` (when implemented) inherits
the per-instance read_dump `.tmp` filename from Stage 5a AxiDpiAdapter. Files
accumulate in build dir per ctest run. Cosmetic; cleanup via per-instance
destructor unlink is a follow-up.

### §5 Timing master direction differs from spec §3 anchored decision

Stage 5b harness keeps Stage 5a's "C++ drives clock" pattern (`main.cpp`
toggles `clk_i` and drives `rst_ni`). The spec §3 decision says "SV master DPI
direction" — the C++ harness is the timing master at the Verilator level, while
the SV side owns the cycle-by-cycle wire propagation. This nuance is consistent
with main plan §5.4 "low-friction first Verilator" goal; documented for future
VCS DPI-RTL port (which will reverse the role).

## New limitations introduced in Stage 5b

### §6 wb2axip line 805-807 enforces stricter-than-spec single-burst-at-a-time

`cosim/sv/wb2axip/faxi_slave.v:805-807`:
```verilog
always @(*)
if (f_axi_wr_pending > 1)
    `SLAVE_ASSERT(!i_axi_awready);
```

This forces AWREADY=0 while any AW's W burst is mid-flight (wr_pending > 1,
i.e., more than 1 W beat still expected). It prevents a slave from accepting
a new AW while a previous AW's W beats are still arriving.

**Not an AXI4 mandate.** Author note at `faxi_slave.v:583-587` explicitly states
this is "not strictly required by the specification, but is required in order
to make these properties work" — a wb2axip-internal simplification for formal
engine convergence.

**AXI4 IHI 0022H §A3.3 + §A5.2.2** allow multi-outstanding AW with pipelined W
(W beats serialized in AW order, because no WID in AXI4). Slave may pre-assert
AWREADY for next AW while current AW's W burst is still in flight, as long as
it tracks the AW order internally to demultiplex incoming W beats.

**c_model NMU is spec-compliant**: `axi_slave_port.hpp:101-103 can_accept_aw()`
returns pure queue-vacancy without single-burst ordering constraint.

**Impact on Stage 5b smoke set**: wb2axip-bound scenarios are scope-limited to
single-beat configurations (AWLEN=0 throughout). Multi-beat / true
multi-outstanding scenarios are verified at C++ adapter / Scoreboard layer
only (T10 unit test + per-shell unit tests).

**Forbidden workarounds attempted + reverted**:
- T15 (commit 0008c28) added wr_pending_gt1 AWREADY suppression in
  NmuShellAdapter + SlaveShellAdapter to silence the assertion. Side-effect:
  `push_aw()` still consumed beat into c_model queue while reporting
  AWREADY=0 on wire → C++/SV state mismatch. Reverted in commit 9701cb5.

**Acceptable resolution paths** (none done in Stage 5b PoC):
- (a) Fork wb2axip with `F_OPT_SINGLE_WRITE_BURST` parameter added (Apache 2.0
  attribution must be updated accordingly). Set to 0 to bypass the assertion.
- (b) Use a different AXI4 protocol checker without this internal
  simplification (candidates: SVA-AXI4-FVIP under Tabby CAD, AmbaInception,
  OSVVM, or vendor IP). Evaluate when porting to VCS in next stage.
- (c) Accept current scope limit (PATH TAKEN HERE): wb2axip verifies
  single-beat traffic; multi-beat is C++-layer verified.

DO NOT modify `cosim/sv/wb2axip/faxi_slave.v` to silently remove the
assertion. That is OSS-attribution violation + verification-integrity
violation per memory `feedback-verification-ip-fault-injection`.
