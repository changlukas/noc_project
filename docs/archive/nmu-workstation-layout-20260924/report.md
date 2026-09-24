# NMU workstation consolidation ? 2026-09-24

The September 22 validation directory is a historical standalone source/build/log snapshot. It still references older RTL names and is not the active environment.

All workstation environments now reside under `/home/mingwei/noc_project/nmu-standalone/`: the existing standalone remains at the root, co-simulation is in `cosim/`, and the old snapshot is in `archive/nmu-vcs-validation-20260922/`.

The relocation audit compared SHA256, file modes and symlink targets: all 1,202 co-simulation entries and 228 historical entries were preserved. Original root Makefile and manifest backups are in `archive/layout-before-20260924/`. Historical logs retain their original absolute paths.

Root commands retain standalone as the default. Use `make list TESTBENCH=cosim` and `make run TESTBENCH=cosim CASE=data_read_write` for memory acceptance. See [workstation layout](../../../sim/standalone/nmu/workstation-layout.md).

Validation: root list commands and co-simulation dry-run passed; relocated VCS `data_read_write` passed with 32 writes, 32 reads, 256 R beats and 16,384 checked bytes. FSDB was generated. The C++ library SHA256 remained `b4eacba6139b468691c890de702c5ec92a712b5f470b7f5f08653c7a42f99196`; only the SV executable was linked for the new absolute path. No full regression or C++ rebuild was performed. Four existing standalone-clean tests passed; Python compilation checks passed. Root standalone clean preserves the nested co-simulation and archive directories.

Source sync and report collection defaults now target the nested co-simulation directory. WSL source layout and the retained Windows checkout were not reorganized. Issue #84 remains open for user acceptance.
