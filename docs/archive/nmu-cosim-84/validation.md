# Validation

- Workstation full matrix: make regress, 14 positive cases PASS. Original command output: remote/acceptance-final.log.
- Negative: make corrupt detects Unexpected RData. Final runner reports NMU_COSIM_CORRUPTION_DETECTED and no PASS marker for this case.
- Final runner/build checks: make run CASE=request_rand; make corrupt; make run_wave CASE=data_write_burst. Exit 0. final-tools.log.
- FSDB: build/report_wave1/data_write_burst.fsdb, 156437 bytes. Hash in remote/artifacts.json.
- C++ library SHA256 before/after SV and wave builds: b4eacba6139b468691c890de702c5ec92a712b5f470b7f5f08653c7a42f99196. Reused, not rebuilt.
- Source synchronization: 302 files verified. Downloaded 85 reports/inputs verified by SHA256. Manifests under remote/.
- Local C++: docker exec -w /workspace noc-kv-dev build/cmodel/tests/wrap/test_ni_router_chain, 6 passed.
- Python: docker exec -w /workspace noc-kv-dev python3 -m pytest sim/tools -q, 598 passed. First-run existing alignment-sensitive failures and their repair are documented separately.
- Earlier focused SV lint/elaboration: lint.log. Global codegen drift check passed while building the affected C++ test target.
- Final source diff whitespace check passed. Production RTL unchanged in this integration.

Scope and remaining coverage are listed in report.md. Issue #84 remains open.
