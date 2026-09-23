# Validation

- Eight additional control/data cases PASS, with scenario coverage required by the TB. See metrics.json and the batch logs.
- Baselines ctrl_write_burst, data_write_burst and request_rand PASS on the final TB. Original 14-case AXI inputs remain byte-identical to prior acceptance.
- make corrupt detects the intentional R-data mismatch and emits CORRUPTION_DETECTED without PASS.
- data_capacity_recover and data_read_write pass in FSDB mode. Files are 179340 and 153158 bytes respectively. Hashes are recorded in remote/artifacts.json.
- docker exec -w /workspace noc-kv-dev python3 -m pytest sim/tools -q: 599 passed.
- 344 synchronized source/input files and 138 retrieved reports/inputs verified by SHA256.
- C++ DPI library remains SHA256 b4eacba6139b468691c890de702c5ec92a712b5f470b7f5f08653c7a42f99196. No C++ rebuild, production RTL change, model-source change or parameter-value change.
- Source diff whitespace check passed. Raw simulator logs are preserved as emitted, including tool clock-skew notices. Content-hashed builds prevent stale binary reuse across source changes.

The first backpressure attempt and empty-file parser warning were correctly rejected. Both are documented in plan.md, with final accepted runs recorded separately. No checks or warnings were disabled.

The local shared catalog is sim/test_patterns/cosim/cases.json. It is committed explicitly despite the repository's generated-pattern ignore rule. The workstation pattern.txt and patterns/cases.list contain all 22 supported cases. Stale preparation inputs collected alongside them are not additional accepted cases.

Issue #84 remains open pending user review. The completed four-scenario implementation plan is archived in this directory.
