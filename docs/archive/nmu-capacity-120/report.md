# NMU identity and tracking capacity

Issue: https://github.com/changlukas/noc_project/issues/120

## Contract

| Parameter | Default | Meaning and dependencies |
|---|---:|---|
| AXI_ID_WIDTH | 3 | External ID width, independent of the generated internal width |
| NOC_ID_WIDTH | 3 | Generated profile 1..8; packet offsets and SV/C++ records regenerated together |
| MAX_ACTIVE_IDS | 8 | At most both AXI/NoC ID spaces; remap and ordering arrays scale to this count |
| MAX_OUTSTANDING_PER_ID | 32 | Range 1..256 retained; per-ID metadata and counters scale with this depth |
| B_ROB_DEPTH / R_ROB_DEPTH | 128 / 128 | Independent payload storage; auxiliary write-offset arrays now use DEPTH entries |

Default header and packet layout remain unchanged. REQ width is 133+NoC ID width,
RSP width is 123+NoC ID width and DAT width remains 633 for profiles 1..8.
`ORDERING_TAG_WIDTH` remains 8. It bounds the current ROB base encoding and the
existing per-ID legality guard; the header has no total outstanding-count field.

## Storage and timing

The order entry contains 19 bits. Read lane context contains 20 bits for the
512-bit AXI port. Per-ID metadata arrays therefore store
`MAX_ACTIVE_IDS * MAX_OUTSTANDING_PER_ID * (2*19 + 20)` bits, excluding counters,
pointers, remap and ROB payload. Default metadata is 14848 bits. Active3/per-ID4
uses 696 bits. These are declared storage bits, not synthesized area.

ROB write offsets formerly used 256 entries per direction; at the unchanged
128-entry default each direction now uses 128 entries. The 8-bit offsets total
2048 bits rather than 4096 bits across B and R. Payload capacity is unchanged.
The B instance ties `wr_last_i` high, so its offsets remain zero and are eligible
for constant removal by synthesis. A separate B-only storage implementation is
therefore not justified by the declared-bit count alone.
No pipeline, queue or transaction pool was added. Arbitration fanout grows with
active IDs, and per-ID table read multiplexers grow with per-ID depth. Timing,
area and power remain unmeasured until synthesis. A constant non-power-of-two
arbitration wrap is required to keep selected indices inside the allocated table.

## Verification status

Generator and pattern tests: 203 PASS. Default control same-ID cross-destination
reorder: VCS PASS with B/R buffered-response coverage and checked readback.
Active3/per-ID4 control cross-ID reorder and active1/per-ID1 control burst also
passed VCS. Wider-profile capacity comparisons and data burst passed. Focused buffered
arbitration at active3 also passed. Nine co-simulation runs and one focused
ordering run passed. The final narrower external interface test used AXI ID width 1,
NoC ID width 3 and the automatically derived active-ID limit 2. The same wider-profile DPI library hash and modification
time were retained across all five wider-profile runs (see results.json).

Initial diagnostic runs are retained: VCS rejected a TB monitor placed before
its clock declaration (fixed). MODE=rand at the existing two-cycle destination
delay did not produce B disorder, so its coverage gate failed. The checker was
not relaxed. Control reorder is used for the focused ROB acceptance case.

## Matched capacity measurements

All rows use internal ID width 4, external AXI width 8, 32-entry transport FIFOs,
the same 10 ns AXI/NoC period and 48 writes followed by 48 checked reads.
B/R ROB occupancy and inserted response delays are zero for these cases.

| Input recipe | Active IDs | Per-ID limit | Cycles | Transactions/cycle | Peak AXI W/R outstanding | ID-limit AW/AR stall cycles | Per-ID AW/AR stall cycles |
|---|---:|---:|---:|---:|---|---|---|
| 12 external IDs | 3 | 4 | 726 | 0.1322 | 3 / 3 | 302 / 285 | 0 / 0 |
| Same files | 12 | 4 | 186 | 0.5161 | 35 / 21 | 0 / 0 | 0 / 0 |
| One external ID | 12 | 1 | 2160 | 0.0444 | 1 / 1 | 0 / 0 | 1034 / 987 |
| Same files | 12 | 32 | 186 | 0.5161 | 32 / 21 | 0 / 0 | 6 / 0 |

Active-ID capacity improves batch throughput by 726/186 = 3.90x in the first pair.
Per-ID capacity improves it by 2160/186 = 11.61x in the second pair. These are
configuration comparisons, not speedups from a pipeline change. Increasing
capacity costs metadata state and wider selection logic. The remaining six AW
stall cycles at per-ID32 cannot be added directly to completion time because
accepted requests continue progressing downstream concurrently.

The default limits were not raised. The results support independently sizing
active IDs and per-ID depth to the workload, with synthesis needed to evaluate
the corresponding timing/area cost. They do not establish a universal optimum.

## Cross-file consistency

Updated references: docs/nmu-spec.md, docs/noc-target-spec.md,
docs/nmu-verification-plan.md, docs/nsu-spec.md, docs/router-spec.md,
docs/nsu-verification-plan.md, docs/noc-performance-parameters.md,
docs/verification-environment.md, docs/trade-off.md and rtl/README.md.
Historical measurement conditions retain their original 3-bit profile.

The old target summary listed a 1..32 per-ID range, while the NMU specification,
RTL legality guard and issue audit already defined 1..256. The summary now follows
the issue's retained 1..256 contract; no new upper-bound increase was implemented.

## Deferred architecture work

No shared global transaction pool or ROB hole allocator was implemented. The
per-direction theoretical admission limit is active IDs times per-ID depth;
ROB storage, downstream capacity and traffic dependencies can bind earlier.
The current suffix allocator can leave freed holes unusable until trailing
allocations retire. Any allocator replacement needs separate PPA review.

## Reproduction

From the WSL checkout, prepare the wider profile using the same shared generator:

```sh
python3 sim/cosim/nmu/prepare.py \
  --rtl-stage sim/standalone/nmu/output/i8_n5_b128_r1/stage \
  --out build/nmu-capacity/wide/stage \
  --profile sim/cosim/nmu/profiles/id4.yml \
  --extra-catalog sim/test_patterns/cosim/capacity_perf.json
python3 sim/tools/sync_nmu_workstation.py \
  --source build/nmu-capacity/wide/stage \
  --remote-dir /home/mingwei/noc_project/nmu-standalone/cosim
```

From the workstation `nmu-standalone/cosim/`, the matched pairs are:

```sh
make run WAVE=1 CASE=capacity_active_ids MAX_ACTIVE_IDS=3 MAX_OUTSTANDING_PER_ID=4
make run WAVE=1 CASE=capacity_active_ids MAX_ACTIVE_IDS=12 MAX_OUTSTANDING_PER_ID=4
make run WAVE=1 CASE=capacity_per_id MAX_ACTIVE_IDS=12 MAX_OUTSTANDING_PER_ID=1
make run WAVE=1 CASE=capacity_per_id MAX_ACTIVE_IDS=12 MAX_OUTSTANDING_PER_ID=32
python3 test_pipeline.py --ordering-only --report build/capacity120/ordering
```

The extra capacity recipes are in the auto-mode catalog. They use 48 single-beat
control writes followed by checked readback, one destination and no inserted
backpressure or reorder delay. The first pair uses 12 external IDs; the second
uses one. Total cycles count from the first accepted request through the last
accepted response, including the write/read barrier. Reported transactions per
cycle are batch throughput, not an isolated sustained link-bandwidth result.
Admission-stall counters sample valid and not-ready on AW/AR and classify the
remap's active-ID or per-ID capacity condition. They do not claim to enumerate
all independent downstream stall causes. Peak outstanding counts are measured at
the external AXI interface and include requests buffered before NoC injection.

The co-simulation profile keeps NSU downstream records at the NoC width for the
existing model ABI, while NMU external AXI width is independently configured.
The model NSU uses its existing full-passthrough mode at the resolved ID-space
size. This does not implement the separately planned target-side partial remap.

## Workstation handoff and build reuse

The workstation is restored to the default profile: NoC ID3, external AXI ID3,
active8 and per-ID32. NMU B/R ROB capacities remain 128 entries. Both source trees
were synchronized directly over SSH and SHA256-verified: 679 files in cosim and
204 in the standalone root. No build products were removed. Signal hierarchy and
RC paths are unchanged; ID signals in the RC use full names without fixed bit slices.

Switching the generated headers back to a previously built profile exposed a
cache defect: synchronized mtimes caused an unnecessary default DPI rebuild even
though its source-hash directory already existed. The Makefile now relies on the
project-source content key, retaining ordinary vendor-header dependencies. SV
sources and compile flags are also covered by their content key. The final dry-run
check changes a project-header mtime and verifies no C++ or SV rebuild for the
same profile, while changing a capacity requests only a new SV build.

No synthesis results are claimed. Width1/4/8 packet generation is covered by unit
tests; VCS functional evidence uses NoC widths 3 and 4. The documented rand reorder
coverage gap remains visible and is not counted among passing acceptance runs.
