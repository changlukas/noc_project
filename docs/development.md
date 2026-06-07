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

All three lint checks and the full ctest suite must be green. If a test
that previously passed now fails, fix it -- do not skip or comment out
the test. Do not disable ctest discovery to hide failures.

### Never --no-verify

Git hook bypass (`--no-verify`) is forbidden. If a hook fails,
investigate and fix the root cause. Hooks run `make check` on the staged
tree; a passing hook means the commit is known-good at submission time.

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
- Files: `snake_case` for C++ source files; `PascalCase` only where
  required to match a class name (e.g. `NmuShellAdapter.cpp`).
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
  docs/_archive/README.md, tests/scenarios/README.md, spec/ni/README.md)
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

### Hermetic singleton invariant

Each `cosim/c/*_shell_adapter.hpp` owns exactly one c_model component.
Cross-component references are forbidden:

- `cosim/c/<comp_a>_dpi.cpp` must not reference `g_<comp_b>_adapter`.
- `*_shell_adapter.hpp` must not include another shell's adapter header.
- No C++ component may hold a reference or pointer to a different
  component.

A future CI gate (`tools/check_cosim_hermetic.sh`, planned) will enforce
this via grep. Today the hermetic invariant is enforced by code review;
the script is tracked as a future automation task.
If you need cross-component communication, the correct fix is to extend
the c_model component API -- not to bridge through the adapter layer.

---

## 3. Build system

### Entry points

From the repo root:

~~~bash
make build           # c_model + Verilator (correct dep order)
make build-cmodel    # c_model only (CMake + ninja)
make build-verilator # Verilator binary (depends on build-cmodel)
make test            # run c_model ctest suite
make check           # lint + build + full ctest
make sim             # run default scenario through Vtb_top
make sim SCENARIO=AX4-BUR-002_incr_8beat   # specific scenario
make clean           # remove all build artifacts
~~~

### Recursive make

The top-level Makefile orchestrates only. Subdir Makefiles own their
own build and clean targets. `$(MAKE) -C cosim/verilator` delegates to
`cosim/verilator/Makefile` for the Verilator build step.

### PYTHON3 override for Verilator build

`cosim/verilator/Makefile` uses `PYTHON3 ?= python3` as the default
interpreter for the Verilator include-file helper. On Windows with MSYS2
`python3` may not resolve; use:

~~~bash
make build-verilator PYTHON3="py -3"
make check PYTHON3="py -3"
~~~

On Linux and macOS `python3` is correct and no override is needed.

### CMakeCache.txt and configure

CMake configure runs only when `c_model/build/CMakeCache.txt` is absent
(first time or after `make clean-cmodel`). Subsequent `make build-cmodel`
calls are pure `cmake --build`, which avoids re-running configure-time
side-effect custom targets (e.g. codegen_check) in a different
subprocess environment.

`file(GLOB CONFIGURE_DEPENDS ...)` is used for the scenario list and for
codegen inputs. Adding or removing a scenario YAML or a codegen template
will trigger CMake reconfigure automatically on the next build.

### clean-cmodel timing

`make clean-cmodel` removes `c_model/build/`. The next `make build-cmodel`
will run a full CMake configure. If CMake fails after a clean, check that
the required tools (Python 3, PyYAML, Ninja or Make) are on PATH before
running configure.

---

## 4. Generated files and specgen

### Generated artifacts

The following files are auto-generated at build time and must not be
edited by hand:

- `specgen/generated/cpp/` -- C++ headers produced by specgen from
  the NI spec YAML.
- `c_model/FEATURE_INVENTORY.md` -- feature inventory markdown generated
  from the same YAML sources.

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
produce from current YAML. Exit code 1 means drift -- regenerate and
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

3. Add `data.txt` for write transactions (one hex word per line, one
   line per beat). For multi-beat bursts, provide one word per beat.
   Add `strb.txt` or `excl.txt` if the scenario requires them.

4. Add a `metadata.ihi_ref:` field citing the IHI 0022H section the
   scenario is derived from (e.g. `ihi_ref: "A3.4.1"` for a burst
   scenario).

5. Run `make check`. The scenario is picked up automatically by both
   the c_model integration test and the cosim integration test via
   CMake `CONFIGURE_DEPENDS`.

6. If the cosim test SKIPs the new scenario with a `WB2AXIP_*` reason
   code, that is expected -- wb2axip does not model that case. No
   action needed; the SKIP is self-documenting.

7. Commit with a body paragraph citing the IHI 0022H section or
   protocol property the scenario exercises.

---

## 6. Targeted tests

### Running specific tests

Use ctest `-R` to filter by test name:

~~~bash
cd c_model/build
ctest -R NmuShellAdapter --output-on-failure   # all NmuShellAdapter tests
ctest -R multi_beat --output-on-failure        # all tests matching multi_beat
ctest --output-on-failure                      # all tests
~~~

### Registering new test files

Add a `GoogleTest::AddGoogleTest` (or equivalent) call in the
appropriate `CMakeLists.txt`. C++ test files under `c_model/tests/`
follow the naming convention `test_<subject>.cpp`. Every new file must:

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

### make sim sanity check

Before suspecting c_model or checker bugs, run the default scenario
and inspect the log:

~~~bash
make sim    # runs AX4-BAS-003_single_write_read_aligned
cat cosim/output/AX4-BAS-003_single_write_read_aligned/run.log
~~~

A passing run ends with `$finish` and no assertion failure lines.

### faxi_slave.v assertion at line 807

If the run.log contains a line referencing `faxi_slave.v:807`, this is
the wb2axip single-burst-at-a-time assertion (see
`docs/architecture.md` sec. 4). The scenario is sending multi-beat or
multi-outstanding write traffic that exceeds wb2axip's internal
constraint. This is not a c_model conformity failure.

Correct response: do not add scenario-specific skip logic. The
`wb2axip_block_reason()` predicate inspects scenario content (`len`,
`lock`, `max_outstanding_write`); if it does not already catch your
scenario, the scenario likely violates a wb2axip constraint that the
predicate does not yet capture. Add the constraint to the predicate,
not to the scenario. Alternatively, route the scenario through the
c_model integration test only (Layer 2).

Do NOT modify `faxi_slave.v` to remove the assertion.

### ctest path for cosim tests

The cosim ctest executables are registered in `c_model/build/` by the
CMake configuration that includes cosim tests. To run cosim tests
directly:

~~~bash
cd c_model/build
ctest -R cosim --output-on-failure
~~~

The Vtb_top binary must be built (`make build-verilator`) before cosim
ctests can run.

### Vtb_top output log

Vtb_top writes run.log to `cosim/output/<SCENARIO>/run.log` when invoked
via `make sim`. When invoked via ctest, output goes to stdout, captured
by `--output-on-failure`.

### NmuShellAdapter multi-beat unit test

The multi-beat W burst coverage path through the cosim adapter layer is
exercised by:

~~~bash
cd c_model/build
ctest -R NmuShellAdapter.multi_beat_w_burst_visible_per_cycle --output-on-failure
~~~

This test proves that each of the 8 W beats in an AWLEN=7 burst is
individually visible on the wire bundle at successive ticks, independent
of the wb2axip constraint.

### test_checker_fires_on_violation

The bringup test for the wb2axip checker itself:

~~~bash
cd c_model/build
ctest -R test_checker_fires_on_violation --output-on-failure
~~~

This test uses the INF-001 scenario (a deliberately invalid handshake)
and verifies that `faxi_slave.v` fires an assertion. If this test fails,
the checker is not connected to the DUT wires correctly -- debug the
DPI wire bundle mapping before proceeding with other cosim tests.

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
  discipline, cosim boundary, wb2axip structural limits.
- `spec/ni/README.md` -- normative NI specification.
- `tests/scenarios/README.md` -- scenario naming convention, YAML
  schema, IHI 0022H section coverage table.
- `specgen/docs/guide/index.md` -- specgen sub-project guide.
- `docs/_archive/README.md` -- historical archive index and doc class
  definitions.
- IHI 0022H (AMBA AXI4 protocol specification, ARM Ltd.) -- referenced
  inline throughout.
