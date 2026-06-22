# Development guide

This document covers workflow, repository conventions, build system,
generated files, scenario authoring, test targeting, cosim debugging,
and the pre-submit checklist.

For system architecture and component descriptions, see
`docs/architecture.md`.

## Table of contents

1. [Workflow](#1-workflow)
2. [Repository conventions](#2-repository-conventions)
3. [Build system](#3-build-system)
4. [Generated files and specgen](#4-generated-files-and-specgen)
5. [Adding a scenario](#5-adding-a-scenario)
6. [Targeted tests](#6-targeted-tests)
7. [Debugging cosim](#7-debugging-cosim)
8. [Pre-submit checklist](#8-pre-submit-checklist)
9. [References](#9-references)

---

## 1. Workflow

### Branching and PRs

Feature work is done on a named branch. PRs target `main`. Branches
should be short-lived (one logical change per branch). Rebase against
main before opening a PR; do not merge main into the feature branch
with a merge commit.

### Commit format

Every commit message must follow the format:

~~~
type(scope): description (English)
~~~

Valid types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`,
`chore`, `perf`, `build`, `revert`.

Scope is a short noun identifying the affected area (e.g. `nmu`,
`cosim`, `scenarios`, `makefile`). The description is imperative mood,
lowercase, no trailing period.

### Pre-submit gate

Run `make check` before every commit that touches code or docs:

~~~bash
make check    # lint_scenarios + lint_docs + build-cmodel + build-verilator + ctest
~~~

Both lint checks and the full ctest suite must be green. If a test
that previously passed now fails, fix it -- do not skip or comment out
the test. Do not disable ctest discovery to hide failures.

### Never --no-verify

Git hook bypass (`--no-verify`) is forbidden. If a hook fails,
investigate and fix the root cause.

### Prefer new commits over amending

When a pre-commit hook fails, the commit did NOT happen. Creating a
new commit after fixing the issue is correct. Amending the previous
commit is wrong -- it may modify unrelated work. Amend only when the
user explicitly requests it.

### CLAUDE.md scope note

`CLAUDE.md` at the repo root is internal AI-assistant tooling (prompt
context, skill invocations, memory references). It is not contributor
onboarding documentation. New human contributors should read this
document (`docs/development.md`) and `docs/architecture.md` instead.

---

## 2. Repository conventions

### Naming

- C++ variables and methods: `snake_case`.
- C++ types and classes: `PascalCase`.
- Files: `snake_case` for C++ source files (e.g. `nmu_shell_adapter.hpp`
  holds class `NmuShellAdapter`); the file name matches the class name
  in snake_case form.
- Module / class names: full word, no abbreviations. `Arbiter` not
  `Arb`; `NmuShellAdapter` not `NmuShellAdpt`. File name must match
  class name.
- Scenario directories: `AX4-CAT-NNN_slug` (uppercase category code,
  zero-padded NNN, lowercase hyphen-separated slug).
- Markdown files: `lowercase-kebab-case.md`.

### C++17 and clang-format

All C++ source (`.hpp`, `.cpp`) must be formatted with `clang-format`
before committing. The repo root `.clang-format` enforces:

- Google style base.
- IndentWidth 4.
- ContinuationIndentWidth 4.
- ColumnLimit 100.
- No camelCase variable names (enforced by convention, not clang-format).

After editing any `.hpp` or `.cpp`, run:

~~~bash
clang-format -i path/to/file.hpp path/to/file.cpp
~~~

Continuation lines (function arguments that wrap) use 4-space indent,
not alignment indent.

### YAML schema

Scenario YAML files must conform to the schema in
`tests/scenarios/README.md`. Required fields: `schema_version: 1`,
`metadata.name`, `metadata.category`. The `name` field must equal the
parent directory basename. Category code must agree with the `CAT`
prefix in the directory name.

### Markdown

- Encoding: UTF-8, no BOM.
- Maintained docs (README.md, docs/architecture.md, docs/development.md,
  docs/_archive/README.md, tests/scenarios/README.md)
  must be ASCII-only. The `lint_docs` target enforces this.
- Headings: sentence case (only first word and proper nouns capitalized).
- Link paths: forward slash `/` separator.
- No trailing whitespace; Unix line endings preferred.

### Path separators

Use forward slash `/` in all tool invocations, Makefile rules, and
documentation. On Windows, MSYS2 shell and CMake both accept forward
slashes. Double backslash `\\` is acceptable in C++ string literals only.

### Python invocation

Use `py -3` (not `python3`) for all Python tool invocations on the
Windows host. This resolves through the Windows py launcher to the
correct Python 3.x installation.

### Per-instance handle ABI

`cosim/c/cmodel_dpi.cpp` exposes a `cmodel_<shell>_create(name)`
function per shell type. Each returns a 64-bit integer handle
(`unsigned long long`, SV `longint unsigned`) encoding a `HandleBlock*`
registered in `g_handle_registry`. Cycle handlers take
`unsigned long long ctx` as leading argument and validate via
`REQUIRE_HANDLE` before adapter access. `cmodel_finalize` walks the
registry destroying all live handles. (The handle is a plain integer,
not SV `chandle`: VCS rejects `chandle` as a module port, so the wraps
pass the handle as `longint unsigned` and the C side casts it back to
`HandleBlock*` at the DPI boundary.)

This replaces the prior 5-singleton invariant. See
`docs/superpowers/specs/2026-06-09-multi-instance-dpi-design.md` for
the design rationale.

---

## 3. Build system

### Entry points

From the repo root:

~~~bash
make build           # c_model + Verilator (correct dep order)
make build-cmodel    # c_model only (CMake + ninja) -> build/cmodel/
make build-verilator # Verilator binaries -> build/verilator/
make test            # run c_model ctest suite
make check           # lint + build + full ctest
make clean           # remove all build artifacts
~~~

The root Makefile builds only. Simulation runs from each simulator's own
directory; run logs land in that directory's `output/<scenario>/run.log`:

~~~bash
cd cosim/verilator
make run-genamba                                # gen_amba role-1 (Tasks A-G)
make run-genamba GENAMBA_SCENARIO=<ax4-id>      # specific scenario
make run-tb-top                                 # wire-level cosim, default scenario
make run-tb-top SCENARIO=AX4-BUR-002_incr_8beat # specific scenario

cd cosim/vcs                                    # Linux workstation only
make run-genamba / run-tb-top
~~~

All build artifacts live under the top-level `build/` tree:
`build/cmodel/` (CMake), `build/verilator/` (obj_dir + obj_genamba),
`build/vcs/` (simv + csrc).

### Recursive make

The top-level Makefile orchestrates only. Subdir Makefiles own their
own build and clean targets. `$(MAKE) -C cosim/verilator` delegates to
`cosim/verilator/Makefile` for the Verilator build step.

### PYTHON3 auto-detection

Both the root and `cosim/verilator/Makefile` auto-detect the Python
interpreter: they prefer the Windows `py -3` launcher when present (the
MSYS2 mingw64 `python3` often lacks PyYAML, which the scenario/perf
scripts need) and fall back to `python3` on Linux/macOS. No manual
`PYTHON3=` override is needed on either host; set it in `local.mk` only
if the auto-detection picks the wrong interpreter.

### Dual-simulator support (Verilator / VCS)

Both cosim testbenches build under either simulator from the same source
lists (`cosim/sources.mk`); simulator-specific flags live in
`cosim/verilator/Makefile` and `cosim/vcs/Makefile`.

~~~bash
cd cosim/verilator && make run-genamba   # Verilator (Windows + Linux)
cd cosim/vcs       && make run-genamba   # VCS (Linux workstation only)
~~~

Layout of the split:

- `cosim/sources.mk` -- simulator-neutral file lists, include dirs,
  `+define+`s.
- `cosim/verilator/` -- Verilator flags, the C++ main drivers (`main.cpp`
  drives tb_top's clock; `main_genamba.cpp` is an eval loop), and the
  Verilator 5.036 workarounds (`--output-split 0`, backslash-path sed,
  `$(EXEEXT)`).
- `cosim/vcs/` -- VCS flags only. No C++ main: VCS owns simulation time.
  tb_genamba is self-clocked already; tb_top is wrapped by
  `cosim/sv/tb_top_vcs.sv` (clock + reset + timeout + final-block
  finalize, mirroring `main.cpp`). Adjust the `[WORKSTATION]` block in
  `cosim/vcs/Makefile` (vcs path, license, site flags) before first use.

The vendored-task patches (B-latch reads, R-shadow array) are
simulator-neutral and identical under both flows. The `SIM=vcs` path has
been dry-run validated only -- first run on a real VCS install pending.

#### FSDB waveform dumping (VCS only)

Opt-in per run; default off (regression and ctest are unaffected):

~~~bash
cd cosim/vcs
make run-tb-top SCENARIO=AX4-BUR-002_incr_8beat FSDB=1   # -> output/<scenario>/tb_top.fsdb
make run-genamba FSDB=1                                   # -> output/genamba_<scenario>/tb_genamba.fsdb
make run-all-fsdb                                         # all 37 scenarios + genamba, summary at end
~~~

Requirements: `VERDI_HOME` defaults to `/tools/verdi_2020.03` (the
workstation's install, taken from its local reference Makefile -- untracked
`cosim/ref/`); override it if the layout
differs. FSDB builds produce separate `simv_tb_top_fsdb` / `simv_genamba_fsdb`
binaries beside the normal ones; toggling `FSDB` never reuses a binary from
the other mode.
For memory dumping / interactive Verdi debug, enable the heavier ref-flow
combo: `FSDB_EXTRA="-debug_access+all -debug_all +fsdb+all +vcsd"`.

`run-all-fsdb` always exits 0 -- it is not a regression gate; failing
patterns are reported and their partial fsdb is kept for debug.
`AX4-INF-*` failures are annotated "fails by design" in the summary.

First-run validation on the workstation (record results in the
`[WORKSTATION]` block of `cosim/vcs/Makefile`):

1. `FSDB_PLI` paths exist (`$VERDI_HOME/share/PLI/VCS/LINUXAMD64/{novas.tab,pli.a}`).
2. Whether `LD_LIBRARY_PATH` needs the FSDB runtime libs.
3. Open one fsdb in Verdi: top-level AXI interfaces and DPI wrapper
   boundaries must be visible (not merely a loadable file).

#### VCD waveform tracing (Verilator)

Twin of the FSDB flow for the Verilator side -- usable on the Windows dev
host without VCS/Verdi. Opt-in per run; default off:

~~~bash
cd cosim/verilator
make run-tb-top SCENARIO=AX4-BUR-002_incr_8beat TRACE=1  # -> output/<scenario>/tb_top.vcd
make run-genamba TRACE=1                                 # -> output/genamba_<scenario>/tb_genamba.vcd
make run-all-trace                                       # all 37 scenarios + genamba, summary at end
~~~

Trace builds verilate with `--trace` into separate obj dirs
(`obj_dir_trace` / `obj_genamba_trace`); toggling `TRACE` never reuses the
other mode's build. The dump code in `main.cpp` / `main_genamba.cpp` is
`#if VM_TRACE`-guarded, so non-trace builds are unchanged. `run-all-trace`
follows `run-all-fsdb` semantics (always exits 0; a scenario counts PASS
only if its vcd is non-empty; `AX4-INF-*` annotated "fails by design").

View locally with GTKWave, or transfer to the workstation for Verdi (Verdi
opens VCD directly). VCD is uncompressed text -- `gzip` before transfer.

### Verified toolchain versions

The build is developed and verified on Windows 11 + MSYS2 (mingw64).
Recorded versions are the combination known to work; a Linux host should
match the Verilator major.minor where possible.

| Tool | Verified version | Notes |
|---|---|---|
| Verilator | 5.036 (MSYS2 mingw64) | `--output-split 0` on the genamba target works around a 5.036 coroutine-split bug; harmless on newer versions |
| GCC (g++) | 15.2.0 (mingw64) | C++17 |
| CMake | MSYS2 mingw64 build | ninja generator |
| GoogleTest / yaml-cpp | pinned by CMake FetchContent | no system install needed |
| Python | 3.13 (`py -3`) / 3.12 (mingw64 `python3`) | lint + specgen tooling |

On a new host (e.g. a Linux workstation): `git clone`, install
verilator + g++ + cmake + ninja + python3 from the distro or a user-space
prefix, then `make check` -- build artifacts are per-clone (the whole
`build/` tree and per-sim `output/` dirs are gitignored), so Windows and
Linux working copies never share or clobber binaries. Line endings are pinned by `.gitattributes` (LF in repo
objects; shell/build files LF in the working tree on every platform).

### One command, every host

The goal is that `make build` (and `make build-cmodel`) is the same line on
Windows and on the Linux workstation -- host differences are auto-detected,
not passed as flags:

- **CMake**: auto-prefers `cmake3` when present (RHEL's modern 3.x), else
  `cmake`. Covers the case where an unrelated toolchain shadows an ancient
  cmake on PATH (e.g. a Xilinx SDK's 3.3.2). Build needs cmake >= 3.14
  (FetchContent_MakeAvailable + gtest 1.14).
- **std::filesystem**: g++ 8.x keeps it in a separate library; the build links
  `stdc++fs` automatically for GCC < 9 (CMake via `CMAKE_CXX_COMPILER_VERSION`,
  cosim Makefiles via the compiler's `-dumpversion`). g++ >= 9 and non-GCC need
  nothing. (RHEL/CentOS 7's g++ 4.8 is too old for C++17 entirely -- use
  `scl enable devtoolset-N bash` and run everything in that shell.)
- **Offline deps**: see below -- auto-engages if `~/noc_offline_deps` exists.

Anything the auto-detection gets wrong can be pinned per-machine in a
gitignored `local.mk` at the repo root (read by both the root Makefile and the
cosim Makefiles), e.g.:

~~~make
CMAKE      := /opt/cmake/bin/cmake
DEPS_SRC   := /scratch/noc_deps
VERDI_HOME := /tools/verdi_2020.03
~~~

### Offline / firewalled hosts

FetchContent cannot download googletest/yaml-cpp there. Copy `googletest-src/`
and `yaml-cpp-src/` from an online host's `build/cmodel/_deps/` (`.git` subdirs
not needed) into `~/noc_offline_deps`:

~~~bash
mkdir -p ~/noc_offline_deps
tar xzf noc_offline_deps.tar.gz -C ~/noc_offline_deps
make build-cmodel        # DEPS_SRC auto-detected from ~/noc_offline_deps
~~~

`DEPS_SRC` defaults to `~/noc_offline_deps` if that directory exists (override
in `local.mk` for a different path) and configures with
`FETCHCONTENT_FULLY_DISCONNECTED=ON`, so an accidental download attempt fails
loudly instead of hanging on the firewall.

### CMakeCache.txt and configure

CMake configure runs only when `build/cmodel/CMakeCache.txt` is absent
(first time or after `make clean-cmodel`). Subsequent `make build-cmodel`
calls are pure `cmake --build`, which avoids re-running configure-time
side-effect custom targets (e.g. codegen_check) in a different
subprocess environment.

`file(GLOB CONFIGURE_DEPENDS ...)` is used for the scenario list and for
codegen inputs. Adding or removing a scenario YAML or a codegen template
will trigger CMake reconfigure automatically on the next build.

### clean-cmodel timing

`make clean-cmodel` removes `build/cmodel/`. The next `make build-cmodel`
will run a full CMake configure. If CMake fails after a clean, check that
the required tools (Python 3, PyYAML, Ninja or Make) are on PATH before
running configure.

---

## 4. Generated files and specgen

### Generated artifacts

The following files are auto-generated and must not be edited by hand:

- `specgen/generated/cpp/` -- C++ headers produced by specgen from the
  authored JSON sources (`specgen/source/*.json`); regenerated via
  `codegen.py`, drift-checked at build time by `codegen_check`.
- `c_model/FEATURE_INVENTORY.md` -- feature inventory markdown generated
  from `specgen/source/noc_function_blocks.json` by
  `specgen/tools/gen_inventory.py` (run manually after editing the JSON;
  drift-gated by `specgen/tests/test_feature_inventory.py`).

Generated files are excluded from `lint_docs` and from clang-format runs.

### Regenerating

To regenerate C++ headers:

~~~bash
py -3 specgen/tools/codegen.py --target cpp --domain packet
py -3 specgen/tools/codegen.py --target cpp --domain signals
py -3 specgen/tools/codegen.py --target cpp --domain registers
~~~

To regenerate SV headers:

~~~bash
py -3 specgen/tools/codegen.py --target sv --domain packet
py -3 specgen/tools/codegen.py --target sv --domain signals
py -3 specgen/tools/codegen.py --target sv --domain registers
~~~

### Drift check

The CMake build runs `codegen_check` as a custom target to detect drift
between the YAML sources and the committed generated headers. To run it
manually:

~~~bash
py -3 specgen/tools/codegen.py --check
~~~

Exit code 0 means the committed headers match what the generator would
produce from the current JSON sources. Exit code 1 means drift -- regenerate and
commit the updated headers before `make check` will pass.

### specgen sub-project guide

See `specgen/docs/guide/index.md` for the specgen sub-project design,
template authoring, and extension guide.

---

## 5. Adding a scenario

1. Pick a category code (CAT) and the next available sequence number
   (NNN) within that category. Create the directory:

   ~~~bash
   mkdir tests/scenarios/AX4-CAT-NNN_my_slug
   ~~~

2. Write `scenario.yaml` with `schema_version: 1` and a full
   `metadata:` block. The `name` field must equal the directory basename
   exactly.

3. Add `data.txt` for write transactions. The parser
   (`c_model/include/axi/axi_master.hpp:251,327-330`) requires at least
   `(len + 1) * (1 << size)` bytes total (extra bytes past that point
   are read but unused), where `len` and `size` are the AXI awlen /
   awsize fields of the transaction (number of beats minus one, and
   beat byte-width log2). Tokens are whitespace-delimited hex bytes
   (one `uint8_t` per token); per-line layout (e.g. one beat per
   line, 32 bytes per line for a 256-bit data bus) is a project
   convention, not enforced by the parser. Add `strb.txt` or
   `excl.txt` if the scenario requires them. Infrastructure scenarios
   that deliberately reference a missing data file (e.g. INF-001) do
   not provide an actual data file.

4. Run `make check`. The scenario is picked up automatically by both
   the c_model integration test and the cosim integration test via
   CMake `CONFIGURE_DEPENDS`.

5. INF-prefix scenarios are skipped from both run-all paths (marker
   `INF_DEDICATED_TEST`, set in
   `c_model/tests/axi/test_integration.cpp:87` and
   `cosim/tests/test_cosim_integration.cpp:61`) and should only be
   exercised through their dedicated test.

6. Commit with a body paragraph citing the IHI 0022H section or
   protocol property the scenario exercises.

---

## 6. Targeted tests

### Running specific tests

Use ctest `-R` to filter by test name:

~~~bash
cd build/cmodel
ctest -R NmuShellAdapter --output-on-failure   # all NmuShellAdapter tests
ctest -R multi_beat --output-on-failure        # all tests matching multi_beat
ctest --output-on-failure                      # all tests
~~~

### Registering new test files

Add an `add_cmodel_test(test_<subject>)` call (the project wrapper
around `gtest_discover_tests`, defined at
`c_model/tests/CMakeLists.txt:13`) in the appropriate subdirectory
`CMakeLists.txt` under `c_model/tests/`. C++ test files follow the
naming convention `test_<subject>.cpp`. Every new file must:

- Include at least one test that exercises the primary new behaviour.
- Include at least one test that verifies the primary error path.
- Pass `make check` before the first commit that adds the file.

### Unit test vs integration test

Unit tests exercise one component in isolation using fakes or stubs for
collaborators. Integration tests exercise the full c_model pipeline or
the full cosim harness. Prefer unit tests for new logic; add an
integration test only when the interaction between components is the
property under test.

---

## 7. Debugging cosim

### run-tb-top sanity check

Before suspecting c_model or checker bugs, run the default scenario
and inspect the log:

~~~bash
cd cosim/verilator && make run-tb-top   # AX4-BAS-003_single_write_read_aligned
cat cosim/verilator/output/AX4-BAS-003_single_write_read_aligned/run.log
~~~

A passing run ends with `$finish` and no assertion failure lines.

### ctest path for cosim tests

The cosim ctest executables are registered in `build/cmodel/` by the
CMake configuration that includes cosim tests. To run cosim tests
directly:

~~~bash
cd build/cmodel
ctest -R Cosim --output-on-failure       # matches CosimIntegration
~~~

The cosim ctest name (PascalCase) is `CosimIntegration` (see
`cosim/tests/CMakeLists.txt`). `ctest -R` matches against test names
with a regex, so `-R cosim` (lowercase) matches zero tests.

The Vtb_top binary must be built (`make build-verilator`) before cosim
ctests can run.

### Vtb_top output log

Vtb_top writes run.log to `cosim/verilator/output/<SCENARIO>/run.log` when
invoked via `make run-tb-top`. When invoked via ctest, output goes to stdout, captured
by `--output-on-failure`.

### NmuShellAdapter multi-beat unit test

The multi-beat W burst coverage path through the cosim adapter layer is
exercised by:

~~~bash
cd build/cmodel
ctest -R NmuShellAdapter.multi_beat_w_burst_full_rate_aw_available --output-on-failure
~~~

This test proves that each of the 8 W beats in an AWLEN=7 burst is
individually visible on the wire bundle at successive ticks.

### gen_amba role-1 testbench

The gen_amba role-1 testbench (`tb_genamba`) is a separate cosim target
from `tb_top`. It drives the NMU/NSU bridge with a gen_amba golden master
BFM through seven AXI4 patterns (baseline, burst, outstanding, outstanding
burst, same-ID, mixed R+W, deep pressure) -- see
`docs/superpowers/specs/2026-06-08-genamba-role1-testbench-design.md`
for the design and the matching findings document for the Phase 1 outcome.

Build + run from repo root:

~~~bash
cd cosim/verilator
make run-genamba                                     # default scenario
make run-genamba GENAMBA_SCENARIO=AX4-BUR-002_incr_8beat  # override
~~~

The default scenario is `AX4-BAS-001_single_write_no_read`. All current
Tasks A-G are scenario-independent (the BFM owns the stimulus); the
`+scenario=` plusarg only feeds `cmodel_init` so the DPI lifecycle has
something valid to point at.

Build artefacts land in `build/verilator/obj_genamba/` -- separate from
`tb_top`'s `build/verilator/obj_dir/` because Verilator generates each
`--top-module` into a single `--Mdir` and two tops would clobber each
other.

Per-cycle AW/W/B/AR/R handshake dumps are gated behind
`+define+GENAMBA_DBG_AXI` (default off; runs stay quiet). Enable for
bring-up debug:

~~~bash
make -C cosim/verilator clean-genamba run-genamba \
    VERILATOR_EXTRA_FLAGS=+define+GENAMBA_DBG_AXI
~~~

If `make run-genamba` fails with "Can't open perl script /mingw64/bin/verilator"
or similar PATH errors, the shell does not have MSYS2 paths. The Makefile's
TOOLPATH prefix adds them unconditionally in every recipe (no-op on Linux),
but if you're in a non-Git-Bash shell (PowerShell / cmd) call the wrapper
directly:

~~~bash
./cosim/verilator/run_genamba.sh \
    +scenario=tests/scenarios/AX4-BAS-001_single_write_no_read/scenario.yaml
~~~

---

## 8. Pre-submit checklist

Before opening a PR or merging a branch:

- [ ] `make check` passes clean (lint_scenarios + lint_docs + build +
      ctest -- all green, no SKIPs that are not pre-existing).
- [ ] `clang-format -i` applied to every `.hpp` and `.cpp` file touched.
- [ ] Commit message follows `type(scope): description` format (English).
- [ ] New behaviour covered by at least one new test that was passing
      before the PR is opened.
- [ ] No `--no-verify` used in any commit on the branch.
- [ ] Any IHI 0022H section exercised by new scenarios is cited in the
      commit body.
- [ ] SV files reviewed per `rtl-style` guidelines (naming suffixes,
      no latches, correct blocking / non-blocking use).

---

## 9. References

- `docs/architecture.md` -- system context, component map, tick
  discipline, cosim boundary.
- `tests/scenarios/README.md` -- scenario naming convention, YAML
  schema, IHI 0022H section coverage table.
- `specgen/docs/guide/index.md` -- specgen sub-project guide.
- `docs/_archive/README.md` -- historical archive index and doc class
  definitions.
- IHI 0022H (AMBA AXI4 protocol specification, ARM Ltd.) -- referenced
  inline throughout.
