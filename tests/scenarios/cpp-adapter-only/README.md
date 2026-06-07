# cpp-adapter-only — reserved for L2 (adapter) scenarios

Empty today. The cosim NMU/NSU shell-adapter unit tests
(`c_model/tests/cosim/test_*_shell_adapter.cpp`) construct their AXI
stimulus directly in-memory rather than parsing scenario YAML, so no files
live here yet.

Populate this sub-dir when a future adapter test wants to share a YAML
scenario with L1 (SV co-sim) or L3 (c_model unit) — but the pattern is
adapter-specific (e.g. exercises an injection-vs-passthrough split unique
to the shell adapter) and doesn't belong in `common/`.
