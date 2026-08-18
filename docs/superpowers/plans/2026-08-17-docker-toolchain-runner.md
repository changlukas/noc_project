# Docker Toolchain Runner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a NoC-specific Docker toolchain image and use it to run the existing 2x2 `make sim` gate before RTL implementation starts.

**Architecture:** Keep the NoC build system as-is. Add a Docker image that provides the Linux toolchain, then run the current Makefile targets inside that image with `BUILD_ROOT` pointing to a container-local directory. Add Sandcastle only after raw Docker proves the gate.

**Tech Stack:** Docker Desktop, Ubuntu Linux base image, Verilator, CMake, GCC/G++, Python, pytest, PyYAML, Make, Git, existing repo Makefiles, Sandcastle Docker provider.

**Spec:** `docs/backlog.md` Verification section and WSL/build standing rules.

## Global Constraints

- VCS is out of this round.
- Do not change RTL/ref-model behavior while building the toolchain image.
- All repository files created by this effort stay under `E:\05_NoC\noc_project`.
- Do not write generated project files into `E:\05_NoC\sandcastle` or any other sibling directory.
- Docker images, containers, and build cache live in Docker Desktop's managed storage; this is external runtime state, not a repo file.
- Planning source of truth is `.planning/2026-08-17-noc-docker-toolchain/`; `IMPLEMENTATION_PLAN.md` mirrors only AGENTS.md stage status.
- Keep source mounted read/write at `/workspace`, but keep heavy build output in container-local `$HOME/noc_build`.
- Every `2x2 verify` and `4x4 verify` run starts with:

```bash
rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv
rm -rf "$BUILD_ROOT"/verilator/obj_dir_*
```

- Use `CONFIG=`, not `TB=`.
- `2x2 verify` runs:

```bash
make sim CONFIG=mesh_2x2 PATTERN=neighbor
```

`4x4 verify` is manual regression work until the host Docker/WSL backend is stable under large
Verilator builds.

---

## File Map

Planned file tree:

```text
E:\05_NoC\noc_project\
+-- .dockerignore
+-- Makefile
+-- package.json
+-- package-lock.json
+-- IMPLEMENTATION_PLAN.md
+-- docker\
|   +-- noc-dev\
|       +-- Dockerfile
+-- .sandcastle\
|   +-- noc-runner.mts
+-- docs\
|   +-- backlog.md
|   +-- superpowers\
|       +-- plans\
|           +-- 2026-08-17-docker-toolchain-runner.md
+-- .planning\
    +-- .active_plan
    +-- 2026-08-17-noc-docker-toolchain\
        +-- task_plan.md
        +-- findings.md
        +-- progress.md
```

- Create: `docker/noc-dev/Dockerfile`
  - Builds the NoC toolchain image.
  - Recommended image name: `noc-dev:verilator-5.048`.
- Create: `.dockerignore`
  - Keeps build artifacts, sim output, generated object dirs, and planning noise out of image build context.
- Modify: `Makefile`
  - Add small Docker convenience targets only: image build, shell, check, and `2x2 verify`.
  - Existing non-Docker targets remain unchanged.
- Create: `.sandcastle/noc-runner.mts`
  - Minimal Sandcastle smoke after raw Docker gate passes.
- Modify: `docs/backlog.md`
  - Record verified Docker gate and remaining risk.

## Task 1: Build The NoC Docker Image

**Files:**
- Create: `docker/noc-dev/Dockerfile`
- Create: `.dockerignore`

**Interfaces:**
- Produces image: `noc-dev:verilator-5.048`
- Exposes working directory: `/workspace`
- Uses build output root: `/home/agent/noc_build`

- [ ] **Step 1: Create Dockerfile skeleton**

Use Ubuntu base, install build tools, Python packages, and create an `agent` user:

```dockerfile
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG VERILATOR_VERSION=5.048
ARG VERILATOR_JOBS=4

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    bison \
    ca-certificates \
    ccache \
    cmake \
    flex \
    g++ \
    git \
    help2man \
    libfl-dev \
    make \
    ninja-build \
    perl \
    python3 \
    python3-pip \
    python3-pytest \
    python3-yaml \
    wget \
    xz-utils \
    zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "v${VERILATOR_VERSION}" https://github.com/verilator/verilator.git /tmp/verilator \
 && cd /tmp/verilator \
 && autoconf \
 && ./configure \
 && make -j"${VERILATOR_JOBS}" \
 && make install \
 && cd / \
 && rm -rf /tmp/verilator

RUN if getent passwd 1000 >/dev/null; then \
      existing_user="$(getent passwd 1000 | cut -d: -f1)"; \
      usermod -l agent -d /home/agent -m "${existing_user}"; \
    else \
      useradd -m -u 1000 -s /bin/bash agent; \
    fi \
 && mkdir -p /home/agent/noc_build \
 && chown -R 1000:1000 /home/agent

USER agent
WORKDIR /workspace
ENV BUILD_ROOT=/home/agent/noc_build
ENV PYTHON3=python3
ENV VERILATOR=verilator

RUN git config --global --add safe.directory /workspace
```

- [ ] **Step 2: Confirm Verilator source build**

Build Verilator `v5.048` from source in the image so the container matches the version already proven by the WSL verify runs.

- [ ] **Step 3: Add `.dockerignore`**

Ignore generated/build artifacts:

```gitignore
.git
build
.planning
.claude
sim/verilator/output
sim/filelist_*.f
sim/tb/test/tb_top_*.sv
sim/tb/soc/tb_top_dma_*.sv
```

- [ ] **Step 4: Build image**

Run:

```bash
docker build -f docker/noc-dev/Dockerfile -t noc-dev:verilator-5.048 .
```

Expected: image builds without modifying the repo working tree.

- [ ] **Step 5: Smoke toolchain**

Run:

```bash
docker run --rm --entrypoint bash noc-dev:verilator-5.048 -lc \
  'verilator --version && cmake --version | head -1 && g++ --version | head -1 && python3 -m pytest --version'
```

Expected: all commands print versions and exit 0.

## Task 2: Run Existing Gates In Raw Docker

**Files:**
- No repo file changes expected.

**Interfaces:**
- Consumes image: `noc-dev:verilator-5.048`
- Uses host repo mounted at `/workspace`

- [ ] **Step 1: Run Docker shell smoke on mounted repo**

Run:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace noc-dev:verilator-5.048 \
  bash -lc 'git status --short && grep CMAKE_HOME_DIRECTORY $HOME/noc_build/cmodel/CMakeCache.txt || true'
```

Expected: `git status` does not fail with `safe.directory`.

- [ ] **Step 2: Run Verilator hello**

Run:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace noc-dev:verilator-5.048 \
  bash -lc 'make -C sim/verilator hello'
```

Expected: output ends with `TOOLCHAIN OK`.

- [ ] **Step 3: Run codegen drift gate**

Run:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace noc-dev:verilator-5.048 \
  bash -lc 'python3 specgen/tools/codegen.py --check'
```

Expected: exit 0.

- [ ] **Step 4: Run c_model tests**

Run:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace noc-dev:verilator-5.048 \
  bash -lc 'make test'
```

Expected: ctest exits 0.

- [ ] **Step 5: Run `2x2 verify` with mandatory pre-clean**

Run:

```bash
docker run --rm -v "$PWD:/workspace" -w /workspace noc-dev:verilator-5.048 bash -lc '
set -euo pipefail
rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv
rm -rf "$BUILD_ROOT"/verilator/obj_dir_*
make sim CONFIG=mesh_2x2 PATTERN=neighbor
'
```

Expected: latest 2x2 log contains `PASS: all`.

## Task 3: Add Developer Entry Points

**Files:**
- Modify: `Makefile`

**Interfaces:**
- Produces targets:
  - `make docker-build`
  - `make docker-shell`
  - `make docker-test`
  - `make docker-sim-tier2`

- [ ] **Step 1: Add Docker variables**

Add near existing host-tool variables:

```make
DOCKER ?= docker
DOCKER_IMAGE ?= noc-dev:verilator-5.048
DOCKER_BUILD_VOLUME ?= noc-dev-build-cache
DOCKER_RUN = $(DOCKER) run --rm -v "$(CURDIR):/workspace" -v "$(DOCKER_BUILD_VOLUME):/home/agent/noc_build" -w /workspace $(DOCKER_IMAGE)
```

- [ ] **Step 2: Add image build target**

```make
.PHONY: docker-build
docker-build:
	$(DOCKER) build -f docker/noc-dev/Dockerfile -t $(DOCKER_IMAGE) .
```

- [ ] **Step 3: Add shell/test/sim targets**

```make
.PHONY: docker-shell docker-test docker-sim-tier2
docker-shell:
	$(DOCKER_RUN) bash

docker-test:
	$(DOCKER_RUN) bash -lc 'make test'

docker-sim-tier2: docker-sim-smoke
```

- [ ] **Step 4: Verify targets**

Run:

```bash
make docker-build
make docker-test
make docker-sim-tier2
```

Expected: all exit 0.

## Task 4: Add Minimal Sandcastle Hook-Up

**Files:**
- Create: `.sandcastle/noc-runner.mts`
- Create: `package.json`
- Create: `package-lock.json`

**Interfaces:**
- Consumes image: `noc-dev:verilator-5.048`
- Uses Sandcastle `createSandbox()` and `docker()`

- [x] **Step 1: Pin the host-side runner dependencies**

`package.json` pins `@ai-hero/sandcastle` 0.12.0 and `tsx` 4.23.12. The lockfile makes the smoke
repeatable without installing an agent CLI in the NoC image.

- [x] **Step 2: Create Sandcastle smoke runner**

Create a minimal script that opens a Docker sandbox and runs one command:

```ts
import { createSandbox } from "@ai-hero/sandcastle";
import { docker } from "@ai-hero/sandcastle/sandboxes/docker";

await using sandbox = await createSandbox({
  branch: "sandcastle/noc-docker-smoke",
  copyToWorktree: ["sim/verilator/Makefile"],
  sandbox: docker({
    imageName: "noc-dev:verilator-5.048",
    containerUid: 1000,
    containerGid: 1000,
  }),
});

const result = await sandbox.exec("make -C sim/verilator hello");
process.stdout.write(result.stdout);
process.stderr.write(result.stderr);

const restore = await sandbox.exec("git restore -- sim/verilator/Makefile");
if (restore.exitCode !== 0) {
  throw new Error(`Failed to clean sandbox worktree:\n${restore.stderr}`);
}

process.exitCode = result.exitCode;
```

- [x] **Step 3: Run Sandcastle smoke**

Run with the pinned local Sandcastle install:

```bash
npm run sandcastle:smoke
```

Expected: exits 0 and prints `TOOLCHAIN OK`.

- [x] **Step 4: Do not move full gate into Sandcastle yet**

Keep the full gate under raw Docker until one full RTL-phase loop needs agent orchestration.

## Task 5: Update Backlog And Handoff

**Files:**
- Modify: `docs/backlog.md`

**Interfaces:**
- Records verified commands and residual risks.

- [x] **Step 1: Record Docker verification**

Add a short note under build environment stating:

```markdown
Docker `noc-dev:verilator-5.048` has passed `make test` and `2x2 verify` in a container.
The image standardizes tool versions. It still relies on Docker Desktop's Linux backend on this Windows host.
```

- [x] **Step 2: Record Sandcastle status**

If Task 4 passed, record:

```markdown
Sandcastle can launch the NoC Docker image and run `make -C sim/verilator hello`.
Full gates remain raw Docker until RTL planning needs agent orchestration.
```

- [x] **Step 3: Final verification before RTL planning**

Run:

```bash
git diff --check
git status --short
```

Expected: only intentional tracked changes are present.

## Decisions Confirmed Before Execution

1. Verilator install method:
   - Build `v5.048` from source in Docker.
2. Add Docker convenience targets to root `Makefile`:
   - Yes, because RTL bring-up will repeat these gates often.
3. Sandcastle timing:
   - Add only a smoke hook now; keep full gate in raw Docker until image is proven.

## Self-Review

- VCS is explicitly excluded.
- Plan does not change ref model, RTL behavior, config schema, or parameter defaults.
- Each task has a runnable verification command.
- The first working layer is raw Docker; Sandcastle is not in the critical path until after the image works.
