# Shared test configuration for Verilator and VCS.
ID_WIDTH ?= 8
NOC_HALF_PERIOD ?= 5
BUFFER_DEPTH ?= 128
READ_ROB_ENABLED ?= 1
PATTERN ?= neighbor
WAVE ?= 0
patterns := neighbor uniform_random hotspot directed
