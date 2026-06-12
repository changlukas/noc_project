# wait_valid / context-gated ready policy — design

Date: 2026-06-12
Status: Approved (user-specified policy, refined in conversation)
Scope: `c_model/include/cosim/{nmu,nsu,slave,master}_shell_adapter.hpp`,
their unit tests, and (separately) vendored Task A AW→W serialization.

## Goal

Replace the vacancy-based ready policy ("ready=1 whenever the queue has
space, regardless of valid") with the user's two-class policy:

- **Address channels (AW/AR)** — strict wait_valid, one-shot: ready is 0 by
  default; when VALID is observed and the unit can accept, ready asserts for
  exactly one wire cycle; the transfer completes on that cycle; ready
  returns to 0.
- **Follow-on channels (W, B, R)** — context-gated pre-assert: once the
  prerequisite handshake completed (AW accepted → W expected; AW/AR issued →
  B/R expected), ready pre-asserts while buffer capacity allows, WITHOUT
  waiting for valid, and holds for the duration of the context (W: until
  WLAST beat received; B: while writes outstanding; R: while read beats
  outstanding).

~~Additional rule: AW acceptance gated while a W burst is open.~~
**Retracted during implementation**: the gating deadlocks legitimate
multi-outstanding patterns that post several AWs before streaming data
(genamba Task C hung structurally; the same serialization would hurt the
RoB/multi-ID paths). `w_expected` instead ACCUMULATES across accepted AWs,
so WREADY stays pre-asserted until all owed beats arrive. The stricter
wb2axip `!awready while wr_pending>1` view remains handled where it always
was — the wb2axip-incompatible scenario skip list — not baked into the
model.

## Per-component table

| Adapter | Drives | New policy |
|---|---|---|
| NmuShellAdapter / SlaveShellAdapter | AWREADY | one-shot: `awvalid && !prev_awready && can_accept_aw` |
| | WREADY | burst-hold: `w_expected>0 && can_accept_w` |
| | ARREADY | one-shot: `arvalid && !prev_arready && can_accept_ar` |
| NsuShellAdapter / MasterShellAdapter | BREADY | outstanding-hold: `outstanding_writes>0 && can_accept_b` (master shell: capacity always true) |
| | RREADY | outstanding-hold: `expected_r_beats>0 && can_accept_r` |

State added per adapter: previous-cycle ready outputs (handshake detection),
`w_expected` (W beats owed, ACCUMULATED across accepted AWs — AWLEN+1 added
per AW handshake, decremented per W handshake; WLAST correctness is left to
the protocol checkers), and on the request side `outstanding_writes` /
`expected_r_beats` (loaded at AW/AR handshakes from the issued lengths,
decremented at B/R handshakes).

## Semantics changes (load-bearing)

1. **Beats are consumed only on true handshake ticks** (`valid &&
   prev_ready`), no longer on first sight of valid. This applies to the
   AW/W/AR pushes in NMU/slave AND to the B/R injections in NSU/master —
   the latter currently inject whenever valid is high, which double-counts
   once ready can be low while valid is held.
2. **Cost**: one bubble cycle per AW/AR transaction and at W-burst start;
   W bursts then run at full rate (burst-hold). Multi-outstanding AW is
   preserved (no burst gating — see the retraction above). wb2axip
   headroom: MAXSTALL=32 vs worst-case 2-cycle ready latency — safe.
3. The master shell's beta-tick guard (ready&&prev_valid) remains correct
   and is now mirrored on every receiving channel.

## Acceptance

1. Updated adapter unit tests pass (idle ⇒ all readys low; two-phase AW/AR
   handshake; W burst-hold full-rate after one bubble; B/R gated on
   outstanding context).
2. Full ctest suite green (435 ± updated tests).
3. Verilator: genamba Tasks A–G pass (TRACE=0/1); tb_top CosimIntegration
   scenarios pass through the wb2axip checkers (MAXSTALL/MAXDELAY; the
   `!awready while wr_pending>1` rule remains covered by the scenario skip
   list for patterns it structurally rejects).
4. Waveform spot-check (VCD): NMU awready/wready idle-low; awready pulses
   one cycle after AWVALID; wready rises after the AW handshake and holds
   through the burst.

## Companion change (separate commit)

Vendored Task A `axi_master_write` fork→sequential: AW (handshake completes
inside write_aw) → W → B, making all genamba write traffic AW-then-W like
Tasks B–G. AXI-legal either way; user prefers the conservative ordering.
