# PicoRV32 iCache cocotb Verification Makefile
# Usage:
#   SETS=128 WAYS=1 WORDS_PER_LINE=1 ITERATIONS=5000 make test_hit
#   SETS=64 WAYS=4 WORDS_PER_LINE=4 ITERATIONS=10000 make test_miss
#   make test_all

# ---- cache geometry parameters (passed as Verilog parameters via COMPILE_ARGS) ----
SETS           ?= 64
WAYS           ?= 4
WORDS_PER_LINE ?= 4
ITERATIONS     ?= 1000
SEED           ?=
export SETS WAYS WORDS_PER_LINE ITERATIONS SEED

# ---- cocotb / simulator settings ----
SIM            ?= verilator
TOPLEVEL_LANG ?= verilog
TOPLEVEL       = icache

VERILOG_SOURCES  = $(realpath ../picosoc/icache.v)

# Verilator compile args: pass parameters, make all signals public for debug
COMPILE_ARGS  = -GSETS=$(SETS) -GWAYS=$(WAYS) -GWORDS_PER_LINE=$(WORDS_PER_LINE)
COMPILE_ARGS += --public-flat-rw -Wno-fatal

# ---- per-test targets ----
test_init:         MODULE = test_init
test_hit:          MODULE = test_hit
test_miss:         MODULE = test_miss
test_replace:      MODULE = test_replace
test_addr_decode:  MODULE = test_addr_decode
test_memory:       MODULE = test_memory
test_handshake:    MODULE = test_handshake
test_concurrency:  MODULE = test_concurrency
test_stress:       MODULE = test_stress


test_init test_hit test_miss test_replace test_addr_decode \
test_memory test_handshake test_concurrency test_stress: $(VERILOG_SOURCES)
	$(MAKE) MODULE=$(MODULE) sim

# ---- run all tests across all 5 configs ----
.PHONY: test_all
test_all:
	@echo "== C1: Direct-mapped, single-word lines =="
	SETS=256 WAYS=1 WORDS_PER_LINE=1 ITERATIONS=200 make clean test_init test_hit test_miss test_replace test_addr_decode test_memory test_handshake
	@echo "== C2: 4-way, single-word lines =="
	SETS=64 WAYS=4 WORDS_PER_LINE=1 ITERATIONS=200 make clean test_init test_hit test_miss test_replace test_addr_decode test_memory test_handshake
	@echo "== C3: 2-way, 4-word lines =="
	SETS=128 WAYS=2 WORDS_PER_LINE=4 ITERATIONS=200 make clean test_init test_hit test_miss test_replace test_addr_decode test_memory test_handshake
	@echo "== C4: Direct-mapped, 4-word lines =="
	SETS=64 WAYS=1 WORDS_PER_LINE=4 ITERATIONS=200 make clean test_init test_hit test_miss test_replace test_addr_decode test_memory test_handshake
	@echo "== C5: 4-way, 8-word lines =="
	SETS=64 WAYS=4 WORDS_PER_LINE=8 ITERATIONS=200 make clean test_init test_hit test_miss test_replace test_addr_decode test_memory test_handshake
	@echo "== Consolidating coverage =="
	python consolidate_coverage.py --dir . --out consolidated_coverage.json
	@rm -f coverage_*.json coverage_*.xml
	@echo "== All configs passed =="

# ---- cocotb include ----
include $(shell cocotb-config --makefiles)/Makefile.sim
