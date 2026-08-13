# Simulation launcher. Everything a run touches lives under this directory, so
# the run targets live here rather than at the repo root; the root Makefile
# keeps the build and test gates.
#
#   make -C sim TB=<topo> PATTERN=<p> [SEED=<n>]
#
# Run logs land in verilator/output/<scenario>/run.log.
#
# This file FORWARDS to verilator/Makefile rather than including it: that
# Makefile includes ../build_config.mk by relative path and writes its logs to a
# relative output/, so it has to run with verilator/ as its working directory.

.PHONY: help sim sim-injection-sweep

# `make -C sim TB=... PATTERN=...` carries no goal, so the default has to be the
# run, not the first target in the file.
.DEFAULT_GOAL := sim

help:
	@echo "Simulate (from repo root, or drop the -C sim if you are already here):"
	@echo "  make -C sim TB=<topo> PATTERN=<p> [SEED=<n>]"
	@echo "  make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor"
	@echo "  TB:      topology from topologies/"
	@echo "  PATTERN: $(_VALID_PATTERNS)"
	@echo "  Vars: INJECTION_MODE=0|1|2 READ_ROB=0|1 INJECTION_RATE= INJECTION_COUNT= IDS_PER_INITIATOR= MAX_UNIQUE_IDS= MAX_OUTSTANDING= HOTSPOT= HOTSPOT_PERIPHERALS=1; SEED unset draws + prints a random seed"
	@echo "  Forensics: TIMEOUT_CYCLES=<n> fires the watchdog early, so a hang dumps per-node outstanding and last_progress without waiting out the formula"
	@echo ""
	@echo "  make -C sim sim-injection-sweep PATTERN=<p>   saturation sweep, long"
	@echo ""
	@echo "Build and test gates stay at the repo root: make build / make test / make pytest"

# PYTHON3 is not part of build_config.mk, so each Makefile that needs it
# carries the line.
PYTHON3 ?= python3

# TB selects the testbench (topology; accepts a tb_ prefix). PATTERN selects a
# spatial pattern, beat_exact (the DPI word-packing probe) or multicast. SEED
# unset -> a random 30-bit seed is drawn and printed so any run is replayable.
# BUILD_ROOT/PYTHON3/VERILATOR/FILELIST_F are NOT passed down -- they flow from
# the repo-root local.mk through build_config.mk.
TB      ?= mesh_4x4_vc1
PATTERN ?= neighbor
_TOPO   := $(TB:tb_%=%)
_VALID_PATTERNS := neighbor transpose uniform_random all_to_all hotspot beat_exact multicast \
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

# Forwarded only when set, so verilator/Makefile's own defaults apply otherwise.
# INJECTION_COUNT's default depends on INJECTION_MODE and is computed there.
_INJ_ARGS := \
    $(if $(INJECTION_MODE),INJECTION_MODE=$(INJECTION_MODE)) \
    $(if $(READ_ROB),READ_ROB=$(READ_ROB)) \
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

# DMA=1 puts a pulp iDMA on every endpoint's master face, on its own top. It has
# no run path yet -- nothing decides when a DMA run is over -- so elaboration is
# the only goal it serves, and asking for a run says so rather than hanging.
ifeq ($(DMA),1)
ifneq ($(ELABORATE_ONLY),1)
$(error DMA=1 supports ELABORATE_ONLY=1 only for now)
endif
endif

sim:
	@echo ">>> sim TB=$(_TOPO) PATTERN=$(PATTERN) SEED=$(_SEED)"
	$(MAKE) -C verilator $(if $(ELABORATE_ONLY),elaborate,run-directed) \
	    TOPOLOGY=$(_TOPO) RUN_CLASS=directed \
	    PATTERN=$(PATTERN) SEED=$(_SEED) $(_INJ_ARGS) $(if $(HOTSPOT),HOTSPOT=$(HOTSPOT)) \
	    $(if $(HOTSPOT_PERIPHERALS),HOTSPOT_PERIPHERALS=$(HOTSPOT_PERIPHERALS)) \
	    $(if $(MCAST_SHAPE),MCAST_SHAPE=$(MCAST_SHAPE))

# Injection-rate sweep: four VC configs x nine rates, one point per make sim.
# MAX_UNIQUE_IDS and MAX_OUTSTANDING are inherited, not forced. Both are shipped
# NI parameters, and the figure's subject is the machine as built. The bring-up
# step measures what other settings would buy, and reports it as a number.
# Heavy: rebuilds Verilator once per VC config. Run on WSL.
SWEEP_RATES ?= 0.05 0.1 0.2 0.3 0.4 0.5 0.7 0.85 1.0
SWEEP_VCS   ?= 1 2 4 8

# Set tools/gen_tb_top.py's _MST_BACKPRESSURE to "ideal" before running this,
# and put it back afterwards. The default is "random", which stalls the master's
# R and B channels, and a consumer that stalls first is what the curve then
# measures instead of fabric saturation. Same reason _MEM_LATENCY stays "ideal".
sim-injection-sweep:
	@for vc in $(SWEEP_VCS); do \
	    for r in $(SWEEP_RATES); do \
	        echo ">>> sweep vc$$vc rate $$r"; \
	        $(MAKE) sim TB=tb_mesh_4x4_vc$${vc} PATTERN=$(PATTERN) SEED=$(_SEED) \
	            INJECTION_MODE=1 READ_ROB=1 INJECTION_RATE=$$r \
	            $(if $(MAX_UNIQUE_IDS),MAX_UNIQUE_IDS=$(MAX_UNIQUE_IDS)) \
	            $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) || exit 1; \
	    done; \
	done
	$(PYTHON3) tools/plot_injection_sweep.py $(PATTERN)
