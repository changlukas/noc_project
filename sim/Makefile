# Simulation launcher. Everything a run touches lives under this directory, so
# the run targets live here rather than at the repo root; the root Makefile
# keeps the build and test gates.
#
#   make -C sim CONFIG=<config> PATTERN=<p> [SEED=<n>]
#
# Run logs land in verilator/output/<scenario>/run.log.
#
# This file FORWARDS to verilator/Makefile rather than including it: that
# Makefile includes ../build_config.mk by relative path and writes its logs to a
# relative output/, so it has to run with verilator/ as its working directory.

.PHONY: help sim build run gen sim-injection-sweep

# `make -C sim CONFIG=... PATTERN=...` carries no goal, so the default has to be the
# run, not the first target in the file.
.DEFAULT_GOAL := sim

help:
	@echo "Simulate (from the repo root; inside sim/ they are sim, build, run, gen):"
	@echo "  make sim-gen CONFIG=<config> PATTERN=<p>          stimulus only; a run needs this first"
	@echo "  make sim CONFIG=<config> PATTERN=<p> [SEED=<n>]   build, then run"
	@echo "  make sim-build CONFIG=<config>                    build only, no run"
	@echo "  make sim-run CONFIG=<config> PATTERN=<p>          run only; errors if not built"
	@echo "  make sim-gen CONFIG=mesh_4x4 PATTERN=neighbor && make sim CONFIG=mesh_4x4 PATTERN=neighbor"
	@echo "  CONFIG:  a sim/configs/*.yml basename: $(_VALID_CONFIGS)"
	@echo "  PATTERN: $(_VALID_PATTERNS)"
	@echo "  The DAT VC count (noc.DAT_NUM_VC) and the NMU read RoB mode"
	@echo "  (nmu.READ_ROB_ENABLED) are specgen constants: edit"
	@echo "  specgen/source/constants.yaml and rebuild, they are not run knobs."
	@echo "  Vars: INJECTION_MODE=0|1|2 INJECTION_RATE= INJECTION_COUNT= IDS_PER_INITIATOR= MAX_UNIQUE_IDS= MAX_OUTSTANDING= HOTSPOT= HOTSPOT_PERIPHERALS=1; SEED unset draws + prints a random seed"
	@echo "  Forensics: TIMEOUT_CYCLES=<n> fires the watchdog early, so a hang dumps per-node outstanding and last_progress without waiting out the formula"
	@echo ""
	@echo "  make sim CONFIG=mesh_2x2 DMA=1               pulp iDMA on every master face"
	@echo "  DMA=1 ignores PATTERN: stimulus is gen_dma_jobs.py's per-node job files, and"
	@echo "  the run ends when every DMA has retired every job and every destination"
	@echo "  region matches its source. Vars: DMA_JOBS_PER_NODE= DMA_LENGTH= DMA_RW=read|write"
	@echo "  (all three feed the job files AND the top's compare) DECERR_FAULT=1 DECERR_FAULT_WR=1"
	@echo ""
	@echo "  make sim-injection-sweep PATTERN=<p>         saturation sweep, long"
	@echo ""
	@echo "Build and test gates stay at the repo root: make build / make test / make pytest"

# PYTHON3 is not part of build_config.mk, so each Makefile that needs it
# carries the line.
PYTHON3 ?= python3

# CONFIG selects the configuration: sim/configs/<CONFIG>.yml, the one file that
# describes a geometry and its address map. PATTERN selects a spatial pattern or
# multicast. SEED unset -> a random 30-bit seed is drawn and printed so any run
# is replayable.
# BUILD_ROOT/PYTHON3/VERILATOR/FILELIST_F are NOT passed down -- they flow from
# the repo-root local.mk through build_config.mk.
CONFIG  ?= mesh_4x4
PATTERN ?= neighbor
_VALID_CONFIGS := $(patsubst configs/%.yml,%,$(wildcard configs/*.yml))
ifeq ($(filter $(CONFIG),$(_VALID_CONFIGS)),)
$(error CONFIG must be one of: $(_VALID_CONFIGS) (got '$(CONFIG)'))
endif
_VALID_PATTERNS := neighbor transpose uniform_random all_to_all hotspot multicast \
                   bit_complement bit_reverse shuffle bit_rotation tornado
ifeq ($(filter $(PATTERN),$(_VALID_PATTERNS)),)
$(error PATTERN must be one of: $(_VALID_PATTERNS) (got '$(PATTERN)'))
endif
# multicast readback is N reads per write (one per member replica), which breaks
# the 1:1 read/write pairing INJECTION_MODE=2's per-id B interlock assumes, and
# mode 1 disarms the scoreboard. Directed two-phase (mode 0) only.
ifeq ($(PATTERN),multicast)
ifneq ($(filter-out 0,$(INJECTION_MODE)),)
$(error PATTERN=multicast supports INJECTION_MODE=0 only (got '$(INJECTION_MODE)'))
endif
endif
# RANDOM is 0..32767; RANDOM*32768+RANDOM draws a uniform 30-bit seed (< 2**30),
# staying under Verilator's +verilator+seed+ int32 ceiling (< 2147483648). The old
# $RANDOM$RANDOM string-concat could reach 10 digits (~3.3e9) and abort the run.
_SEED   := $(if $(SEED),$(SEED),$(shell bash -c 'echo $$(( RANDOM * 32768 + RANDOM ))'))

# verilator/test_patterns/mesh_2x2/neighbor/ is TRACKED: it is the worked
# example README.md walks through, and the README spells the SEED=1 that
# produced it. Nothing enforced it, so generating that one config-and-pattern
# pair without a seed overwrote the tracked files with random-seed content and
# left the tree dirty for the next reader to explain. Naming the seed is what an
# intentional regeneration looks like; every other pair is untracked and free.
# Scoped to `gen`, the only goal that writes the tree -- a run reads it.
ifneq ($(filter gen,$(MAKECMDGOALS)),)
ifeq ($(SEED),)
ifeq ($(CONFIG)-$(PATTERN),mesh_2x2-neighbor)
$(error regenerating the tracked mesh_2x2/neighbor example needs an explicit SEED; \
the committed files are SEED=1)
endif
endif
endif

# Forwarded only when set, so verilator/Makefile's own defaults apply otherwise.
# INJECTION_COUNT's default depends on INJECTION_MODE and is computed there.
_INJ_ARGS := \
    $(if $(INJECTION_MODE),INJECTION_MODE=$(INJECTION_MODE)) \
    $(if $(INJECTION_RATE),INJECTION_RATE=$(INJECTION_RATE)) \
    $(if $(INJECTION_COUNT),INJECTION_COUNT=$(INJECTION_COUNT)) \
    $(if $(IDS_PER_INITIATOR),IDS_PER_INITIATOR=$(IDS_PER_INITIATOR)) \
    $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
    $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) \
    $(if $(B_ROB_DEPTH),B_ROB_DEPTH=$(B_ROB_DEPTH)) \
    $(if $(R_ROB_DEPTH),R_ROB_DEPTH=$(R_ROB_DEPTH)) \
    $(if $(MAX_TXNS_PER_ID),MAX_TXNS_PER_ID=$(MAX_TXNS_PER_ID)) \
    $(if $(BURST_LEN),BURST_LEN=$(BURST_LEN)) \
    $(if $(MCAST_FAULT),MCAST_FAULT=$(MCAST_FAULT)) \
    $(if $(DECERR_FAULT),DECERR_FAULT=$(DECERR_FAULT)) \
    $(if $(DECERR_FAULT_WR),DECERR_FAULT_WR=$(DECERR_FAULT_WR)) \
    $(if $(TIMEOUT_CYCLES),TIMEOUT_CYCLES=$(TIMEOUT_CYCLES))

# DMA=1 puts a pulp iDMA on every endpoint's master face, on its own top. It
# reads sim/tools/gen_dma_jobs.py's job files instead of a spatial PATTERN, and
# ends when every DMA has retired every job its file holds and every
# destination region matches its source, so PATTERN names nothing on this path.

sim run:
	@echo ">>> $@ CONFIG=$(CONFIG) PATTERN=$(PATTERN) SEED=$(_SEED)"
	$(MAKE) -C verilator $(if $(ELABORATE_ONLY),elaborate,run-directed) \
	    TOPOLOGY=$(CONFIG) RUN_CLASS=directed \
	    $(if $(filter run,$@),NO_BUILD=1) \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT)) \
	    $(if $(HOTSPOT_PERIPHERALS),HOTSPOT_PERIPHERALS=$(HOTSPOT_PERIPHERALS)) \
	    $(if $(MCAST_SHAPE),MCAST_SHAPE=$(MCAST_SHAPE))

# Stimulus generation alone: no build, no run. Writes test_patterns/<geometry>/<pattern>/.
gen:
	@echo ">>> gen CONFIG=$(CONFIG) PATTERN=$(PATTERN) SEED=$(_SEED)"
	$(MAKE) -C verilator gen-directed \
	    TOPOLOGY=$(CONFIG) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT)) \
	    $(if $(HOTSPOT_PERIPHERALS),HOTSPOT_PERIPHERALS=$(HOTSPOT_PERIPHERALS)) \
	    $(if $(MCAST_SHAPE),MCAST_SHAPE=$(MCAST_SHAPE))

# Build alone. PATTERN names nothing here: the binary a topology builds is the
# same one every pattern runs on, because a pattern is stimulus read at runtime.
build:
	@echo ">>> build CONFIG=$(CONFIG)"
	$(MAKE) -C verilator build TOPOLOGY=$(CONFIG) RUN_CLASS=directed

# Injection-rate sweep: nine rates, one point per make sim, at whatever VC count
# specgen/source/constants.yaml currently holds. The VC dimension is not swept
# here -- noc.DAT_NUM_VC is a tracked file, and a sweep would have to edit it
# mid-run. Four curves is four edits and four sweeps; each run writes its own
# result.csv and plot_injection_sweep.py globs them.
# MAX_UNIQUE_IDS and MAX_OUTSTANDING are inherited, not forced. Both are shipped
# NI parameters, and the figure's subject is the machine as built. The bring-up
# step measures what other settings would buy, and reports it as a number.
SWEEP_RATES ?= 0.05 0.1 0.2 0.3 0.4 0.5 0.7 0.85 1.0

# Set tools/gen_tb_top.py's _MST_BACKPRESSURE to "ideal" before running this,
# and put it back afterwards. The default is "random", which stalls the master's
# R and B channels, and a consumer that stalls first is what the curve then
# measures instead of fabric saturation. Same reason _MEM_LATENCY stays "ideal".
# Generate once, up front: every point in the loop below runs the same
# configuration, so one generation covers all of them.
sim-injection-sweep:
	$(MAKE) gen CONFIG=$(CONFIG) PATTERN=$(PATTERN) SEED=$(_SEED) \
	    INJECTION_MODE=1 $(if $(INJECTION_COUNT),INJECTION_COUNT=$(INJECTION_COUNT))
	@for r in $(SWEEP_RATES); do \
	    echo ">>> sweep rate $$r"; \
	    $(MAKE) sim CONFIG=$(CONFIG) PATTERN=$(PATTERN) SEED=$(_SEED) \
	        INJECTION_MODE=1 INJECTION_RATE=$$r \
	        $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
	        $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) || exit 1; \
	done
	$(PYTHON3) tools/plot_injection_sweep.py $(PATTERN)
