Depends on: #83 (closed).

## Objective and revised scope

Build a staged NMU standalone acceptance platform using one production RTL NMU, one C++ Router and four C++ NSUs, with an AXI memory behind each NSU. Verify real memory write/readback through the complete round trip. This replaces the old response-ordering implementation scope, whose main datapath already exists.

Topology:
AXI file master + independent scoreboard -> RTL NMU -> single C++ Router -> four C++ NSU/AXI memory endpoints; responses return through that same Router to the RTL NMU.

Use native REQ/RSP/DAT. One Router owns all three networks; do not instantiate a mesh or replace the production NMU with its C++ model. Keep the current deterministic standalone tests as focused regressions.

## Existing components to reuse (source inspection, not co-simulation VCS acceptance)

- ref_model/top/router_wrap.sv and nsu_wrap.sv, plus their per-instance DPI handles and C++ models.
- Connect RTL NMU and C++ NSU to different ports of one Router using existing routing configuration. Do not instantiate DAT merge. Confirm endpoint coordinates, SAM global/local addresses and reverse routing before integration. If existing routing cannot express this topology, review the topology before changing the model.
- Existing axi_file_master methods, axi_sim_mem_intf and axi_scoreboard usage in sim/tb/test/user_node_endpoint.sv. Reuse only the needed stimulus/memory/checker wiring; avoid importing the full tile crossbar/performance campaign.
- sim/tools/gen_standalone_patterns.py, gen_test_patterns.py and the shared AXI file encoder. Keep reusable inputs under sim/test_patterns, shared by simulator backends.
- Existing sim/vcs/Makefile and sim/build_config.mk as the DPI build reference; keep standalone user commands and workstation FSDB/report flow.

## Integration requirements and known gaps

1. Reuse the registered DPI tick discipline, generated flit layouts, reset lifecycle and existing model handles. Check sampled handshake/credit timing against RTL rather than assuming C++-only co-sim timing is identical.
2. NMU and NSU use separate Router ports. Connect real endpoint receive credits directly; no DAT merge or early credit return adapter is in scope.
3. Audit sender seed capacities against each actual receiver FIFO, including registered credit timing. No unbounded adapter queue may hide DUT backpressure.
4. The C++ NMU ingress queue remains unbounded and is deferred because this platform replaces that model with RTL. Preserve this limitation for subsequent model work. Do not change production RTL or parameter defaults merely to fit the model.
5. Confirm workstation C++17 compiler, yaml-cpp, DPI ABI and VCS M-2017 compatibility. Build the required model/DPI dependencies once, then reuse cached objects unless their sources/configuration change. The workstation remains offline and receives sources/dependencies directly over SSH with SHA256 verification.

## Pattern reuse assessment

| Existing source | Reuse plan | Required adaptation / limit |
|---|---|---|
| Standalone control/data single/burst cases | Reuse names, AXI file format, transaction generator/encoder | Map to the approved four-destination SAM; replace synthetic R data and synthetic BRESP expectations with memory semantics |
| Standalone outstanding/multi-ID/backpressure/reset | Reuse scenario intent and driver | Audit achieved concurrency and reset/memory lifetime; directed cases have no injected stalls unless the case explicitly tests stalls |
| Standalone ctrl_rand/data_rand/request_rand | Reuse seeded transaction generation | Prevent ambiguous overlapping read/write dependencies and uninitialized reads; preserve byte strobes and legal bursts |
| Existing C++ simulation patterns | Reuse gen_test_patterns.py readback/prefill generation and the file-master write-complete-then-read schedule | Generate an endpoint-aware unicast workload, not unchanged 2x2/4x4 neighbor/collective traffic |
| Cross-destination reorder / forced response order | Retain existing deterministic standalone coverage | The approved four-destination extension checks actual B/R ingress disorder and per-ID retirement |

The shared generator retains standalone recipes and adds four-destination co-simulation ordering cases.

## Memory correctness and stimulus scheduling

- Reuse the existing file-master channel methods. Initial memory-integrity cases complete writes including B before issuing dependent readback, following the existing C++ co-sim endpoint. This is a data-dependency barrier, not custom per-request AW/AR pacing or an outstanding cap.
- Later concurrent read/write cases use disjoint address ranges or explicit dependencies so the expected data is defined.
- Read-only cases require deterministic preload or a checked initialization phase. Partial-strobe writes must not make untouched bytes unknowable to the checker.
- Scoreboard expectations come from original AXI writes/known initialization, not DUT-decoded packets. Check byte-lane/strobe effects, burst addressing, IDs, BRESP/RRESP, RLAST, counts, same-ID ordering, loss/duplication and complete drain.
- Include deliberate data corruption evidence showing that memory readback checking detects an error. No disabling of scoreboard checks for concurrent traffic.

## Stages

1. Confirm reuse and boundary compatibility, approve the minimal topology/SAM/credit adapter and pattern matrix.
2. Integrate one Router and four NSUs with RTL NMU and existing AXI memory/checker; run control/data single and burst write/readback under VCS.
3. Add approved outstanding/random/backpressure/reset cases using existing generators and driver; retain separate deterministic standalone coverage.
4. Archive exact commands, seed/configuration, source hashes, actual transfer/compare counts and FSDB/report paths; user reviews acceptance before closure.

## Acceptance

- [x] Every tested request traverses RTL NMU, the actual C++ Router and C++ NSU, and AXI memory; no local shortcut or synthetic response path substitutes for them.
- [x] Control/data single/burst write/readback pass with nonzero independently checked memory bytes and expected transaction counts.
- [ ] The approved concurrency/order/reset cases pass, with unsupported/vacuous coverage explicitly identified.
- [ ] All real ready/valid and DAT credit boundaries are preserved, including RTL receive backpressure and reset reseeding.
- [ ] Relevant RTL-only baseline regressions remain available and passing.
- [x] VCS runs directly on the workstation, reports are retrieved, synchronized source hashes match, and C++ objects are not rebuilt for pattern-only reruns.
- [ ] User reviews results before closure.

## Boundaries and related issues

No Router RTL, NSU RTL, full mesh, collective traffic, compute workload, same-direction bypass (#119), or speculative PPA changes. A real memory model is explicitly in scope for this issue, superseding the earlier no-memory restriction for the original synthetic standalone only.

#85 should retain complementary response-path coverage rather than duplicate completed acceptance. #86 overlaps this revised co-simulation-loopback scope and must be reconciled after the actual #84 coverage is known; it is not automatically closed by this scope update.

## Approved platform organization

Use sim/cosim/nmu/, top tb_nmu_cosim, and workstation /home/mingwei/noc_project/nmu-standalone/cosim/. Keep sim/standalone/nmu/ and shared sim/test_patterns/. Initial AXI/NoC clocks match and use existing two-stage reset synchronization. No intentional stalls outside explicit backpressure cases. User approved implementation; issue remains open until user accepts results.

## VCS functional acceptance (2026-09-23)

User approved DAT credit depth 32. The co-simulation profile generates matching SV/C++ Router and NI receive depths. NSU sender initialization uses the actual Router depth before the first tick. Global production defaults and the legacy initialization API remain unchanged.

The initial 14-case VCS matrix passed on be16: control/data single and burst read/write, same_id_in_order, same_id_outstanding, multi_id_outstanding, ctrl_rand, data_rand and request_rand. Total: 284 writes, 284 readbacks, 988 R beats, 8,503 checked bytes. Random seed 1. Deliberate read-data corruption is detected by the existing scoreboard. GCC 9.3 / VCS M-2017 linking and runtime were verified. Source synchronization and report retrieval use SHA256 checks.

Build directories use source/flag content hashes to avoid stale binaries caused by workstation clock skew. SV and pattern reruns reuse the C++ DPI library.

This is initial functional acceptance only. Partial-strobe holes, FIXED/WRAP bursts, forced cross-destination reorder, explicit backpressure/full-capacity recovery and in-flight reset remain outside the measured matrix. C++ NSU REQ ingress remains unbounded in the reference model. Startup reset was exercised, not reset during traffic.

Local focused C++ tests: 6 passed. Final Python sim/tools suite: 598 passed after correcting three existing whitespace-sensitive source assertions. No functional RTL or required check was removed. Final request_rand, corruption and FSDB-enabled data_write_burst checks also pass.

Implementation and evidence: sim/cosim/nmu/ and docs/archive/nmu-cosim-84/report.md on branch feat/nmu-cosim-84 (implementation commit 23d38e8d), synchronized to /home/mingwei/noc_project/nmu-standalone/cosim. Issue remains OPEN pending user acceptance and the remaining coverage.

## Additional user-approved acceptance: PASS

All eight added cases pass VCS: ctrl_backpressure, data_backpressure, ctrl_capacity_recover, data_capacity_recover, ctrl_partial_write, data_partial_write, ctrl_read_write and data_read_write. Total 272 writes, 248 reads, 1,904 R beats and 65,416 checked bytes, including initialization and final readback.

- B/R stalls and stable response payload were exercised.
- Both capacity cases reached 32 outstanding requests per ID in each direction. B receive FIFO and control R / data DAT receive FIFOs became full. AW/AR stalled, then all transfers drained correctly after release.
- Partial writes preserve masked bytes and update selected bytes after full initialization.
- Concurrent cases use disjoint address regions and observe 128 W transfers during read outstanding and 128 R transfers during write outstanding, followed by write-region readback.

Three affected baseline cases and the deliberate corruption test also pass. Full sim/tools Python suite: 599 passed. No production RTL, C++ source or DUT parameter change, and no C++ rebuild. Shared pattern.txt now lists 22 cases. Evidence: docs/archive/nmu-cosim-84/additional/report.md.

Issue remains OPEN for user acceptance. Reset during traffic, forced reorder, FIXED/WRAP and additional seeds are not claimed. Existing model-capacity limitations remain documented.


## Approved four-destination ordering extension (2026-09-24)

Expand the co-simulation TB to a single Router at (1,1), LOCAL RTL NMU, and four direct C++ NSU/memory endpoints: NORTH (1,2), EAST (2,1), SOUTH (1,0), WEST (0,1). Routing bounds are 4x4 to satisfy the existing generator's power-of-two X requirement; no mesh or extra Router instances are introduced. All endpoint port IDs are zero.

Reuse cross_id_out_of_order and same_id_cross_dst_reorder with MODE=control/data/rand. Only ordering cases enable destination-dependent B/R delay; other cases bypass it. Use unchanged upstream one-cycle AXI delayers to cover every integer cycle in the minimum-delay search. Require real B/R ingress disorder, same-ID buffered retirement where applicable, and existing AXI memory data/order/count checks. Preserve DAT credit depth 32, C++ library reuse, and update the waveform RC for all four wrappers. Minimum common delay is 2 measured cycles for seed 1: control fails disorder coverage at 0/1; all six ordering mode combinations pass at 2. The FSDB-enabled 24-case regression passes. Strengthened payloads distinguish transactions, and all four affected control/data runs pass again. Deliberate corruption is detected. Python suite: 602 passed. All 490 RC paths resolve in FSDB; C++ library hash is unchanged, with no C++ rebuild. Reports and inputs were retrieved with SHA256 verification. Evidence: docs/archive/nmu-cosim-84/four-destination/report.md. Issue remains OPEN for user acceptance.
